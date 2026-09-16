// SPDX-License-Identifier: GPL-3.0-only
// Seqlock publish/snapshot — see cache.h for the protocol and why a bare
// pointer swap is not enough with two recycled buffers.

#include "cache.h"

#include <cstring>

namespace nb {
namespace data {

GameList* DataCache::writable(int league) {
    Slot& s = slots_[league];
    // Full barrier before touching the buffer: the change-counter bump must
    // be globally visible before any fill byte is, so a snapshot racing the
    // refill can never accept torn data with an unchanged counter.
    s.seq[s.alt].fetch_add(1, std::memory_order_acq_rel);
    GameList* b = &s.bufs[s.alt];
    std::memset(b, 0, sizeof *b);
    return b;
}

void DataCache::publish(int league, int64_t now_utc) {
    Slot& s = slots_[league];
    const unsigned a = s.alt;
    s.bufs[a].fetched_utc = now_utc;
    // Buffer is now complete and stable (even counter), then make it
    // current; the release store publishes every fill write.
    s.seq[a].fetch_add(1, std::memory_order_acq_rel);
    s.alt = a ^ 1;
    s.cur.store(&s.bufs[a], std::memory_order_release);
}

bool DataCache::snapshot(int league, GameList* dst) const {
    const Slot& s = slots_[league];
    for (int attempt = 0; attempt < 8; ++attempt) {
        const GameList* src = s.cur.load(std::memory_order_acquire);
        if (!src) return false;
        const int i = (src == &s.bufs[0]) ? 0 : 1;
        const uint32_t v = s.seq[i].load(std::memory_order_acquire);
        if (v & 1u) continue;  // buffer mid-refill; cur will move next commit
        std::memcpy(dst, src, sizeof *dst);
        if (s.cur.load(std::memory_order_acquire) == src &&
            s.seq[i].load(std::memory_order_acquire) == v)
            return true;
    }
    return false;
}

int64_t DataCache::last_fetch_age(int league, int64_t now_utc) const {
    const GameList* cur = slots_[league].cur.load(std::memory_order_acquire);
    if (!cur) return -1;
    return now_utc - cur->fetched_utc;
}

bool DataCache::is_stale(int league, int64_t poll_interval_s, int64_t now_utc,
                         double factor) const {
    const int64_t age = last_fetch_age(league, now_utc);
    if (age < 0) return true;
    return age > static_cast<double>(poll_interval_s) * factor;
}

}  // namespace data
}  // namespace nb
