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

class PollScheduler {
  public:
    static constexpr int kLeagues = DataCache::kLeagues;
    static constexpr int64_t kBootStaggerS = 5;  // first-cycle spacing per league

    void set(int league, const PollCfg& cfg) { cfg_[league] = cfg; }
    const PollCfg& cfg(int league) const { return cfg_[league]; }

    // Arm every league: league i first comes due `now + i*kBootStaggerS`.
    void reset(int64_t now);

    bool due(int league, int64_t now) const {
        return cfg_[league].enabled && now >= due_[league];
    }

    // Record a completed fetch: next deadline = now + live/idle cadence.
    void done(int league, int64_t now, bool has_live) {
        due_[league] = now + (has_live ? cfg_[league].live_s : cfg_[league].idle_s);
    }

    // Earliest deadline over enabled leagues; `now` if none (caller clamps
    // its own sleep). Feeds vTaskDelay directly.
    int64_t next_wake(int64_t now) const;

  private:
    PollCfg cfg_[kLeagues] = {};
    int64_t due_[kLeagues] = {};
};

}  // namespace data
}  // namespace nb
