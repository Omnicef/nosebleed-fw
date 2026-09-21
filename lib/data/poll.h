// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.7 — poll scheduler logic. Pure: no Arduino/ESP, host-testable. The
// task loop in main.cpp drives it; this file only answers "who is due" and
// "when should we wake".
//
// Marquee main.py::_poll_espn polls every enabled league each cycle and
// sleeps min(interval, >=15 s). Same cadence here, but per-league
// deadlines instead of a shared sleep: leagues get their own due time
// (live 20 s / idle 120 s per LeagueConfig), staggered at boot so the
// first cycle never bunches them. One task walks the due list sequentially,
// so "only one TLS session ever open" holds by construction.

#pragma once

#include <cstdint>

#include "cache.h"

namespace nb {
namespace data {

struct PollCfg {
    bool enabled;
    uint16_t live_s;  // cadence while any game has status kStatusIn
    uint16_t idle_s;  // cadence otherwise
};

// N sources, staggered deadlines, sequential caller. Instantiated once per
// poll domain: leagues (PollScheduler) and info feeds (InfoScheduler,
// T-10.1: weather + 3 ticker sources) — two schedulers, one task, so every
// fetch stays inside the single-TLS-session discipline.
template <int N> class PollSchedulerT {
  public:
    static constexpr int kCount = N;
    static constexpr int64_t kBootStaggerS = 5;  // first-cycle spacing per source

    void set(int src, const PollCfg& cfg) { cfg_[src] = cfg; }
    const PollCfg& cfg(int src) const { return cfg_[src]; }

    // Arm every source: source i first comes due `now + i*kBootStaggerS`.
    void reset(int64_t now) {
        for (int i = 0; i < N; ++i) due_[i] = now + i * kBootStaggerS;
    }

    bool due(int src, int64_t now) const {
        return cfg_[src].enabled && now >= due_[src];
    }

    // Record a completed fetch: next deadline = now + live/idle cadence.
    void done(int src, int64_t now, bool has_live) {
        due_[src] = now + (has_live ? cfg_[src].live_s : cfg_[src].idle_s);
    }

    // Earliest deadline over enabled sources; `now` if none (caller clamps
    // its own sleep). Feeds vTaskDelay directly.
    int64_t next_wake(int64_t now) const {
        int64_t best = -1;
        for (int i = 0; i < N; ++i) {
            if (!cfg_[i].enabled) continue;
            if (best < 0 || due_[i] < best) best = due_[i];
        }
        if (best < 0 || best < now) return now;  // nothing enabled / overdue: caller sleeps its floor
        return best;
    }

  private:
    PollCfg cfg_[N] = {};
    int64_t due_[N] = {};
};

using PollScheduler = PollSchedulerT<DataCache::kLeagues>;

// T-10.1/T-10.3 — the info feeds ride the same task, same discipline:
// weather (15 min), news / stocks / crypto on their own cadences, all
// sequential behind the one TLS session.
namespace info_src {
enum { kWeather = 0, kNews, kStocks, kCrypto, kCount = 4 };
}  // namespace info_src
using InfoScheduler = PollSchedulerT<info_src::kCount>;

}  // namespace data
}  // namespace nb
