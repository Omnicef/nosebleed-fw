// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.2 — DataCache. poll (core 0) publishes whole GameLists, render
// (core 1) takes consistent snapshots. Genuinely dual-core with real
// preemption, so "swap a pointer" alone is NOT enough: with two buffers a
// reader that holds the old pointer is still copying it when the writer
// recycles that buffer two commits later. The cure is a per-buffer change
// counter (seqlock): the snapshot memcpy is validated before/after and
// retried on any interleaved write.
//
// Rules honoured: render never blocks on I/O or a mutex — snapshot() is
// wait-free (bounded retries, then false => "keep the last strip"). Only
// poll calls writable()/publish(), and never two leagues at a time.
//
// League index = index into kLeagueSlugs (config.h). No Arduino includes.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "game.h"

namespace nb {
namespace data {

class DataCache {
  public:
    static constexpr int kLeagues = 8;  // kLeagueSlugCount; index = slug order

    // --- poll task only (single writer per league) ---

    // Begin an update: zeroes and returns the inactive buffer; caller fills
    // games[0..count) and sets count (<= kMaxGamesPerLeague), then publish().
    // Until publish() the cache keeps serving the previous list.
    GameList* writable(int league);

    // Make the filled buffer current. `now_utc` becomes the freshness stamp.
    void publish(int league, int64_t now_utc);

    // --- any task, lock-free readers ---

    // Consistent copy of the current list into *dst (caller-owned storage —
    // never a stack GameList, it is 3.6 KB). false = never published, or
    // the writer cycled buffers during the copy (call again next frame).
    bool snapshot(int league, GameList* dst) const;

    // Seconds since the last publish, or -1 if never.
    int64_t last_fetch_age(int league, int64_t now_utc) const;

    // Marquee cache.py rule: stale if never fetched or older than
    // factor * poll_interval.
    bool is_stale(int league, int64_t poll_interval_s, int64_t now_utc,
                  double factor = 2.5) const;

  private:
    struct Slot {
        GameList bufs[2];
        std::atomic<const GameList*> cur;  // null until first publish
        std::atomic<uint32_t> seq[2];      // per-buffer change counter
        unsigned alt;                      // next writable buffer; poll-only
    };
    Slot slots_[kLeagues];
};

}  // namespace data
}  // namespace nb
