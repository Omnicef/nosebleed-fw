// SPDX-License-Identifier: GPL-3.0-only
//
// T-4.1 — defaults + validation. set_defaults() is a port of Marquee's
// seed_defaults() (config/defaults.py): splash + clock + one scoreboard per
// league (MLB first, rest alphabetically sorted, only MLB enabled), plus a
// LeagueConfig row per slug. Pure — no NVS, no Arduino.

#include "config.h"

#include <cstdio>
#include <cstring>

namespace nb {
namespace config {
namespace {

constexpr int kScoreboardTypeLen = 16;

void copy_str(char* dst, size_t dst_len, const char* src) {
    std::strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

// _SCOREBOARD_LEAGUES = ["mlb"] + sorted(rest)
void scoreboard_order(int i, char* out) {
    if (i == 0) {
        copy_str(out, kLeagueIdLen, "mlb");
        return;
    }
    // insertion-order list minus mlb, sorted lexicographically
    static const char* rest[kLeagueSlugCount - 1] = {
        "college-football", "epl", "mens-college-basketball", "nba",
        "nhl", "nfl", "womens-college-basketball",
    };
    copy_str(out, kLeagueIdLen, rest[i - 1]);
}

}  // namespace

bool validate(const Config& c) {
    return c.magic == kMagic && c.schema == kSchemaVersion &&
           c.widget_count <= kMaxWidgets &&
           c.league_count <= kMaxLeagues &&
           c.favorite_count <= kMaxFavorites;
}

void set_defaults(Config& c) {
    std::memset(&c, 0, sizeof(c));
    c.magic = kMagic;
    c.schema = kSchemaVersion;

    // HardwareSetting(id=1)
    c.hw.rows = 32;
    c.hw.cols = 64;
    c.hw.chain_length = 1;
    c.hw.parallel = 1;
    c.hw.brightness = 80;
    c.hw.boot_splash_s = 15;
    c.hw.preemption_enabled = 1;
    c.hw.preemption_dwell_s = 30;
    c.hw.timezone[0] = '\0';
    c.hw.display_mode = kDisplayScroll;
    c.hw.scroll_speed = 40.0f;
    c.hw.card_gap = 8;
    c.hw.clock_24h = 0;
    // HUB75-DMA defaults for a single 64x32 1/16-scan panel
    c.hw.lsb_msb_transition_bit = 1;  // ~110 Hz refresh (T-0.2 measured)
    c.hw.clkphase = 0;
    c.hw.latch_blanking = 2;
    c.hw.i2sspeed = 0;  // library default (80 MHz GPIO)
    c.hw.double_buff = 1;

    // WidgetConfig rows
    c.widget_count = 2 + kLeagueSlugCount;
    WidgetConfig& splash = c.widgets[0];
    copy_str(splash.id, sizeof(splash.id), "boot_splash");
    copy_str(splash.type, kScoreboardTypeLen, "boot_splash");
    splash.order = 0;
    splash.enabled = 1;
    splash.dwell_s = 15.0f;
    WidgetConfig& clock = c.widgets[1];
    copy_str(clock.id, sizeof(clock.id), "clock");
    copy_str(clock.type, kScoreboardTypeLen, "clock");
    clock.order = 1;
    clock.enabled = 1;
    clock.dwell_s = 10.0f;
    for (int i = 0; i < kLeagueSlugCount; ++i) {
        WidgetConfig& w = c.widgets[2 + i];
        scoreboard_order(i, w.league);  // _SCOREBOARD_LEAGUES order
        // Local buffer, then copy: writing w.id from w.league in one snprintf
        // trips gcc's -Wrestrict (both live in the same aggregate `c`); the
        // members never actually overlap, but the aliasing analysis can't see it.
        char wid[kWidgetIdLen];
        std::snprintf(wid, sizeof(wid), "scoreboard_%s", w.league);
        copy_str(w.id, sizeof(w.id), wid);
        copy_str(w.type, kScoreboardTypeLen, "scoreboard");
        w.order = static_cast<uint16_t>(2 + i);
        w.enabled = (i == 0) ? 1 : 0;  // only mlb
        w.dwell_s = 20.0f;
    }

    // LeagueConfig(id=slug, enabled=(slug == "mlb")), poll 20/120 from model
    c.league_count = kLeagueSlugCount;
    for (int i = 0; i < kLeagueSlugCount; ++i) {
        LeagueConfig& lc = c.leagues[i];
        copy_str(lc.id, sizeof(lc.id), kLeagueSlugs[i]);
        lc.enabled = (std::strcmp(kLeagueSlugs[i], "mlb") == 0) ? 1 : 0;
        lc.poll_interval_live = 20;
        lc.poll_interval_idle = 120;
    }

    c.favorite_count = 0;
}

bool hw_structural_changed(const HardwareSetting& old, const HardwareSetting& now) {
    // Field-by-field on purpose: a whole-struct memcmp would flag brightness
    // or timezone edits as "needs restart". `reserved` is excluded so adding
    // a live field later can't false-trigger.
    return old.rows != now.rows || old.cols != now.cols ||
           old.chain_length != now.chain_length || old.parallel != now.parallel ||
           old.lsb_msb_transition_bit != now.lsb_msb_transition_bit ||
           old.clkphase != now.clkphase ||
           old.latch_blanking != now.latch_blanking ||
           old.i2sspeed != now.i2sspeed ||
           old.double_buff != now.double_buff;
}

}  // namespace config
}  // namespace nb
