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

// T-9.2 — SSID 1–32, printable, no control chars (percent-decoded form
// fields can smuggle %0A through a length-only check and then desync any
// printf); passphrase empty (open) or 8–63 printable.
bool creds_valid(const Creds& c) {
    size_t sl = 0, pl = 0;
    while (sl < sizeof(c.ssid) && c.ssid[sl] != '\0') ++sl;
    while (pl < sizeof(c.pass) && c.pass[pl] != '\0') ++pl;
    if (sl == 0 || sl >= sizeof(c.ssid)) return false;  // empty / unterminated
    if (pl >= sizeof(c.pass)) return false;             // unterminated
    if (pl != 0 && pl < 8) return false;
    for (size_t i = 0; i < sl; ++i)
        if (static_cast<unsigned char>(c.ssid[i]) < 32 || static_cast<unsigned char>(c.ssid[i]) == 127)
            return false;
    for (size_t i = 0; i < pl; ++i)
        if (static_cast<unsigned char>(c.pass[i]) < 32 || static_cast<unsigned char>(c.pass[i]) == 127)
            return false;
    return true;
}

// T-10.3 — allowlist, not denylist: the value is concatenated into a
// query string, so anything outside [A-Za-z0-9._-] is rejected rather than
// escaped, and no encoder bug can ever be reachable. "" (unset) is valid —
// the fetch code treats it as "source off", the store just never sees it.
bool api_keys_valid(const ApiKeys& k) {
    const auto field_ok = [](const char* s, size_t cap) {
        for (size_t i = 0; i < cap && s[i] != '\0'; ++i) {
            const char c = s[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            if (!ok) return false;
        }
        return strnlen(s, cap) < cap;
    };
    return field_ok(k.gnews, sizeof(k.gnews)) && field_ok(k.finnhub, sizeof(k.finnhub));
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

    // Services — weather off until a location is set, ticker lists are
    // sane defaults, quiet hours off. Empty lat = no weather fetch.
    c.svc.imperial = 0;
    std::snprintf(c.svc.news_category, sizeof(c.svc.news_category), "general");
    std::snprintf(c.svc.news_country, sizeof(c.svc.news_country), "us");
    std::snprintf(c.svc.stock_symbols, sizeof(c.svc.stock_symbols), "AAPL,MSFT,NVDA,AMZN,GOOGL");
    std::snprintf(c.svc.crypto_ids, sizeof(c.svc.crypto_ids), "bitcoin,ethereum,solana");
    c.svc.quiet_enabled = 0;
    c.svc.quiet_start = 22 * 60;
    c.svc.quiet_end = 7 * 60;
    c.svc.quiet_brightness = 0;

    // WidgetConfig rows
    c.widget_count = 3 + kLeagueSlugCount;
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
    // T-10.2 weather card — present but off until the owner sets a location.
    WidgetConfig& weather = c.widgets[2];
    copy_str(weather.id, sizeof(weather.id), "weather");
    copy_str(weather.type, kScoreboardTypeLen, "weather");
    weather.order = 2;
    weather.enabled = 0;
    weather.dwell_s = 10.0f;
    for (int i = 0; i < kLeagueSlugCount; ++i) {
        WidgetConfig& w = c.widgets[3 + i];
        scoreboard_order(i, w.league);  // _SCOREBOARD_LEAGUES order
        // Local buffer, then copy: writing w.id from w.league in one snprintf
        // trips gcc's -Wrestrict (both live in the same aggregate `c`); the
        // members never actually overlap, but the aliasing analysis can't see it.
        char wid[kWidgetIdLen];
        std::snprintf(wid, sizeof(wid), "scoreboard_%s", w.league);
        copy_str(w.id, sizeof(w.id), wid);
        copy_str(w.type, kScoreboardTypeLen, "scoreboard");
        w.order = static_cast<uint16_t>(3 + i);
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
