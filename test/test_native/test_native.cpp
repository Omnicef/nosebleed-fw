// SPDX-License-Identifier: GPL-3.0-only
//
// Host render tests (T-2.1, T-2.2, T-2.4, T-2.6, T-2.7). Built by `pio test
// -e native`. lib/render is compiled for the laptop here — that is the whole
// point of the purity rule. PNGs land in test/out/ for eyeballing; the hard
// gate is the RGB565 clock parity assertion (T-2.7).

#include "unity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ctime>
#include <type_traits>
#include <atomic>
#include <chrono>
#include <thread>

#include "canvas.h"
#include "cache.h"
#include "date_window.h"
#include "poll.h"
#include "espn_json.h"
#include "config.h"
#include "game.h"
#include "timezone.h"
#include "tzmap.h"
#include "font.h"
#include "font_data.h"
#include "golden_clock.h"
#include "golden_logo.h"
#include "logo.h"
#include "logos.h"
#include "png_writer.h"
#include "primitives.h"

using namespace nb;

void setUp(void) {}
void tearDown(void) {}

// T-5.2 — self-consistent generation payloads. Every field of every game is
// a pure function of `gen`, so a snapshot that mixes two generations is
// detectable exactly, not statistically. Returns 0 or the offending gen.
static int check_generation(const data::GameList& l) {
    using namespace data;
    if (l.count < 1 || l.count > kMaxGamesPerLeague) return -1;
    const uint32_t gen = static_cast<uint32_t>(l.games[0].start_utc);
    char id[16];
    std::snprintf(id, sizeof id, "%u", gen);
    if (l.count != 1 + static_cast<int>(gen % kMaxGamesPerLeague)) return static_cast<int>(gen);
    for (int i = 0; i < l.count; i++) {
        const Game& g = l.games[i];
        if (g.start_utc != (int64_t)gen || std::strcmp(g.id, id) != 0 ||
            g.period != (int16_t)(gen % 1000) || g.away_score != (int16_t)(gen % 10) ||
            g.home_score != (int16_t)(gen % 100) || g.situation.outs != (int16_t)(gen % 4) ||
            g.away.colour != gen * 2654435761u)
            return static_cast<int>(gen);
    }
    return 0;
}

static void fill_generation(data::GameList* l, uint32_t gen) {
    using namespace data;
    l->count = 1 + static_cast<int>(gen % kMaxGamesPerLeague);
    char id[16];
    std::snprintf(id, sizeof id, "%u", gen);
    for (int i = 0; i < l->count; i++) {
        Game& g = l->games[i];
        copy_str(g.id, sizeof g.id, id);
        copy_str(g.status_display, sizeof g.status_display, "Gen");
        g.period = static_cast<int16_t>(gen % 1000);
        g.away_score = static_cast<int16_t>(gen % 10);
        g.home_score = static_cast<int16_t>(gen % 100);
        g.start_utc = gen;
        g.situation.outs = static_cast<int16_t>(gen % 4);
        g.away.colour = gen * 2654435761u;
    }
}

static void expand_to_rgb(const Canvas16& c, uint8_t* out) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(c.w) * c.h; ++i) {
        uint8_t r, g, b;
        unpack565(c.px[i], r, g, b);
        out[i * 3] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
}

// T-2.1 — canvas allocates the widest strip and frees cleanly.
static void test_canvas_alloc_strip(void) {
    Canvas16 c = canvas_alloc(2520, 32);  // ~158 KB, 35 cards
    TEST_ASSERT_TRUE(c.valid());
    TEST_ASSERT_EQUAL_INT(2520, c.w);
    TEST_ASSERT_EQUAL_INT(32, c.h);
    c.set(2519, 31, 0xFFFF);
    c.set(-1, 0, 0x1234);      // out of range, must not crash or write
    c.set(2520, 0, 0x1234);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, c.get(2519, 31));
    TEST_ASSERT_EQUAL_UINT16(0, c.get(3000, 40));  // OOB read reads black
    canvas_free(c);
    TEST_ASSERT_NULL(c.px);
}

// RGB565 bit layout: R=15:11, G=10:5, B=4:0 — exactly what the HUB75
// driver's color565to888 unpacks. A transposed shift/mask here shows up on
// hardware as a channel swap (the G/B swap chased at T-2.8). Pure R/G/B/W
// so the expected words are unambiguous.
static void test_rgb565_pack(void) {
    TEST_ASSERT_EQUAL_UINT16(0xF800, rgb565(255, 0, 0));    // RED
    TEST_ASSERT_EQUAL_UINT16(0x07E0, rgb565(0, 255, 0));    // GREEN
    TEST_ASSERT_EQUAL_UINT16(0x001F, rgb565(0, 0, 255));    // BLUE
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, rgb565(255, 255, 255));  // WHITE
    TEST_ASSERT_EQUAL_UINT16(0x0000, rgb565(0, 0, 0));      // black
    uint8_t r, g, b;
    unpack565(rgb565(0, 255, 0), r, g, b);  // pack and unpack must be inverses
    TEST_ASSERT_EQUAL_UINT8(0, r);
    TEST_ASSERT_EQUAL_UINT8(255, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);
    unpack565(rgb565(0, 0, 255), r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);
    TEST_ASSERT_EQUAL_UINT8(255, b);  // unpack rescales 5-bit (<<3 | >>2): round-trip is faithful
}

// Fixed-width text width: len * advance, no glyph measurement.
static void test_text_width(void) {
    TEST_ASSERT_EQUAL_INT(42, text_width(FONT_SPLEEN_6X12, 7));  // "3:30 PM"
    TEST_ASSERT_EQUAL_INT(45, text_width(FONT_SPLEEN_5X8, 9));   // "Thu 12/25"
    TEST_ASSERT_EQUAL_INT(0, text_width(FONT_TOM_THUMB, 0));
    TEST_ASSERT_EQUAL_INT(FONT_TOM_THUMB.advance, text_width(FONT_TOM_THUMB, 1));
}

// T-2.2 — primitives render and clip; dump one PNG to eyeball.
static void test_primitives(void) {
    Canvas16 c = canvas_alloc(64, 32);
    line(c, 0, 0, 63, 31, 0xF81F);
    rect(c, 4, 4, 20, 12, 0xFFFF);
    fill_rect(c, 30, 4, 16, 8, 0x07E0);
    fill_ellipse(c, 12, 24, 6, 5, 0xF800);
    ellipse(c, 48, 24, 7, 5, 0x001F);
    const Point diamond[4] = {{32, 16}, {40, 24}, {32, 32}, {24, 24}};
    fill_polygon(c, diamond, 4, 0xFFE0);

    // fill_rect must set exactly its interior, and leave the outside alone.
    TEST_ASSERT_EQUAL_UINT16(0x07E0, c.get(30, 4));
    TEST_ASSERT_EQUAL_UINT16(0x07E0, c.get(45, 11));
    TEST_ASSERT_EQUAL_UINT16(0x0000, c.get(46, 12));  // just outside
    // diamond centre is filled, corner outside is not.
    TEST_ASSERT_EQUAL_UINT16(0xFFE0, c.get(32, 24));

    uint8_t rgb[64 * 32 * 3];
    expand_to_rgb(c, rgb);
    TEST_ASSERT_TRUE_MESSAGE(write_png_rgb("test/out/primitives.png", 64, 32, rgb),
                             "primitives.png write failed (wrong cwd?)");
    canvas_free(c);
}

// T-2.4 — the three bundled fonts render; dump a sheet to eyeball.
static void test_font_sheet(void) {
    Canvas16 c = canvas_alloc(128, 32);
    draw_text(c, FONT_SPLEEN_6X12, 0, 0, "Aa09", 0xFFFF);
    draw_text(c, FONT_SPLEEN_5X8, 0, 13, "Aa09", 0x07E0);
    draw_text(c, FONT_TOM_THUMB, 0, 24, "Aa09", 0xF81F);
    int lit = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(c.w) * c.h; ++i)
        if (c.px[i]) ++lit;
    TEST_ASSERT_TRUE_MESSAGE(lit > 40, "font sheet rendered almost nothing");
    uint8_t rgb[128 * 32 * 3];
    expand_to_rgb(c, rgb);
    TEST_ASSERT_TRUE_MESSAGE(write_png_rgb("test/out/fonts.png", 128, 32, rgb),
                             "fonts.png write failed");
    canvas_free(c);
}

// T-2.7 — THE GATE: firmware clock card is byte-identical to the golden
// Pillow render, in the RGB565 domain. Zero differing pixels or it fails.
static void test_clock_parity(void) {
    Canvas16 c = canvas_alloc(GOLDEN_W, GOLDEN_H);

    // Exact positions ClockWidget.cards() computes for 2025-12-25 15:30.
    const char* time_str = "3:30 PM";
    const char* date_str = "Thu 12/25";
    const int tx = (GOLDEN_W - text_width(FONT_SPLEEN_6X12, 7)) / 2;  // 11
    const int dx = (GOLDEN_W - text_width(FONT_SPLEEN_5X8, 9)) / 2;   // 9
    draw_text(c, FONT_SPLEEN_6X12, tx, 2, time_str, rgb565(255, 255, 255));
    draw_text(c, FONT_SPLEEN_5X8, dx, 22, date_str, rgb565(140, 140, 140));

    int diffs = 0;
    int first = -1;
    for (int i = 0; i < GOLDEN_W * GOLDEN_H; ++i) {
        if (c.px[i] != GOLDEN_CLOCK[i]) {
            if (first < 0) first = i;
            ++diffs;
        }
    }

    uint8_t rgb[GOLDEN_W * GOLDEN_H * 3];
    expand_to_rgb(c, rgb);
    TEST_ASSERT_TRUE_MESSAGE(write_png_rgb("test/out/clock_fw.png", GOLDEN_W, GOLDEN_H, rgb),
                             "clock_fw.png write failed");

    char msg[96];
    std::snprintf(msg, sizeof(msg), "clock parity: %d px differ (first @x=%d y=%d)",
                  diffs, first % GOLDEN_W, first / GOLDEN_W);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diffs, msg);
    canvas_free(c);
}

// T-3.5 — blit_logo on the REAL atlas bytes (px+mask from logos.bin via
// golden_logo.h) must equal Pillow's card.paste(logo, box, mask=logo) on the
// same art. Zero differing pixels.
static void test_blit_logo_parity(void) {
    Canvas16 c = canvas_alloc(GCARD_W, GCARD_H);
    fill_rect(c, 0, 0, GCARD_W, GCARD_H, GLOGO_BG);
    const LogoArt art{LOGO_PX, LOGO_MASK, LOGO_W, LOGO_H};
    TEST_ASSERT_TRUE(blit_logo(c, LOGO_X, LOGO_Y, art));

    int diffs = 0;
    for (int i = 0; i < GCARD_W * GCARD_H; ++i)
        if (c.px[i] != GOLD_PASTE[i]) ++diffs;

    uint8_t rgb[GCARD_W * GCARD_H * 3];
    expand_to_rgb(c, rgb);
    TEST_ASSERT_TRUE_MESSAGE(write_png_rgb("test/out/logo_fw.png", GCARD_W, GCARD_H, rgb),
                             "logo_fw.png write failed");

    // Partly-offscreen blit must clip, not crash or smear.
    TEST_ASSERT_TRUE(blit_logo(c, -10, -10, art));
    TEST_ASSERT_EQUAL_UINT16(GLOGO_BG, c.get(GCARD_W - 1, GCARD_H - 1));

    canvas_free(c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diffs, "logo paste parity: px differ");
}

// T-3.6 — no art in the atlas: abbreviation in team colour at exactly the
// Python's fallback position (golden B). Also: degenerate refs must be safe.
static void test_abbr_fallback(void) {
    uint16_t col = 0;
    TEST_ASSERT_TRUE(parse_hex565("c8102e", col));
    TEST_ASSERT_EQUAL_UINT16(FBCOL, col);
    TEST_ASSERT_TRUE(parse_hex565("#C8102E", col));  // leading '#' accepted
    TEST_ASSERT_EQUAL_UINT16(FBCOL, col);
    TEST_ASSERT_FALSE(parse_hex565("c8102", col));  // short input rejected
    TEST_ASSERT_FALSE(parse_hex565("zz102e", col));

    Canvas16 c = canvas_alloc(GCARD_W, GCARD_H);
    const int w = draw_abbr_fallback(c, FBX, FBY, FBH, "LIV", col);
    TEST_ASSERT_EQUAL_INT(15, w);  // 3 chars * spleen-5x8 advance

    int diffs = 0;
    for (int i = 0; i < GCARD_W * GCARD_H; ++i)
        if (c.px[i] != GOLD_FALLBACK[i]) ++diffs;

    // Empty/absent art: false, canvas untouched — never a crash (T-3.6).
    const LogoArt none{nullptr, nullptr, 0, 0};
    Canvas16 d = canvas_alloc(8, 8);
    TEST_ASSERT_FALSE(blit_logo(d, 2, 2, none));
    for (int i = 0; i < 64; ++i) TEST_ASSERT_EQUAL_UINT16(0, d.px[i]);

    canvas_free(c);
    canvas_free(d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diffs, "fallback parity: px differ");
}

// T-3.4 host half — the pure index parser (logos.h) against a synthetic
// 2-entry atlas, then the real logos.bin when present (it is a gitignored
// build artifact). Mirrors the device test so the byte-wise rd16/rd32/
// key_cmp/find path is host-proven before flashing anything.
static uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static void test_logos_parse_host(void) {
    using namespace nb::logos;
    static const uint8_t hdr[12] = {'N', 'B', 'L', 'G', 1, 0, 2, 0, 32, 0, 0, 0};
    // e0: epl:LIV off=56 w=2 h=1 (px F800,07E0 + mask 0x80)
    // e1: mlb:BOS off=61 w=1 h=1 (px 001F + mask 0x01)
    uint8_t idx0[22] = {0}, idx1[22] = {0};
    memcpy(idx0, "epl", 3); memcpy(idx0 + 8, "LIV", 3);
    idx0[12] = 56; idx0[16] = 2; idx0[18] = 1;
    memcpy(idx1, "mlb", 3); memcpy(idx1 + 8, "BOS", 3);
    idx1[12] = 61; idx1[16] = 1; idx1[18] = 1;
    const uint8_t blobs[8] = {0x00, 0xF8, 0xE0, 0x07, 0x80, 0x1F, 0x00, 0x01};
    uint8_t buf[64];
    memcpy(buf, hdr, 12);
    memcpy(buf + 12, idx0, 22);
    memcpy(buf + 34, idx1, 22);
    memcpy(buf + 56, blobs, 8);

    Header h;
    TEST_ASSERT_TRUE(parse_header(buf, h));
    TEST_ASSERT_EQUAL_UINT16(2, h.count);
    TEST_ASSERT_EQUAL_UINT16(32, h.logo_height);

    Ref r;
    TEST_ASSERT_TRUE(find(buf, h.count, "epl", "LIV", r));  // lower half
    TEST_ASSERT_EQUAL_UINT16(2, r.w);
    TEST_ASSERT_TRUE(memcmp(r.px, blobs, 4) == 0);
    TEST_ASSERT_EQUAL_UINT8(0x80, r.mask[0]);
    TEST_ASSERT_TRUE(find(buf, h.count, "mlb", "BOS", r));  // upper half
    TEST_ASSERT_EQUAL_UINT8(0x01, r.mask[0]);
    TEST_ASSERT_FALSE(find(buf, h.count, "mla", "ZZZ", r));  // miss between
    TEST_ASSERT_FALSE(find(buf, h.count, "nfl", "KC", r));   // miss above

    // Real atlas: same checks the device runs, when the artifact exists.
    FILE* f = fopen("logos.bin", "rb");
    if (f == nullptr) TEST_IGNORE_MESSAGE("logos.bin absent — run the atlas build");
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* atlas = static_cast<uint8_t*>(malloc(sz));
    const size_t got = fread(atlas, 1, sz, f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sz, got, "logos.bin short read");
    TEST_ASSERT_TRUE(parse_header(atlas, h));
    TEST_ASSERT_EQUAL_UINT16(144, h.count);
    struct Want { const char* l; const char* a; uint32_t hash; };
    static const Want wants[] = {
        {"mlb", "BOS", 0xa917f7d6u}, {"nba", "LAL", 0xeacc63aau},
        {"epl", "LIV", 0x1b0feca6u}, {"nhl", "BOS", 0x6322ff39u},
    };
    for (const auto& w : wants) {
        TEST_ASSERT_TRUE_MESSAGE(find(atlas, h.count, w.l, w.a, r), w.a);
        const size_t n = static_cast<size_t>(r.w) * r.h * 2 +
                         ((static_cast<size_t>(r.w) + 7) / 8) * r.h;
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(w.hash, fnv1a(r.px, n), w.a);
    }
    TEST_ASSERT_FALSE(find(atlas, h.count, "mlb", "ZZZ", r));
    free(atlas);
}

// T-4.1 — POD config structs + defaults (port of Marquee seed_defaults).
static void test_config_defaults(void) {
    using namespace nb::config;
    // 4 KB blob cap is a compile-time static_assert; re-check numerically.
    TEST_ASSERT_TRUE(sizeof(Config) < 4096);

    Config c;
    set_defaults(c);
    TEST_ASSERT_TRUE(validate(c));
    TEST_ASSERT_EQUAL_UINT16(kSchemaVersion, c.schema);
    // HardwareSetting(id=1) defaults
    TEST_ASSERT_EQUAL_UINT16(32, c.hw.rows);
    TEST_ASSERT_EQUAL_UINT16(64, c.hw.cols);
    TEST_ASSERT_EQUAL_UINT16(1, c.hw.chain_length);
    TEST_ASSERT_EQUAL_UINT8(80, c.hw.brightness);
    TEST_ASSERT_EQUAL_UINT8(1, c.hw.preemption_enabled);
    TEST_ASSERT_EQUAL_UINT16(30, c.hw.preemption_dwell_s);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, c.hw.scroll_speed);
    TEST_ASSERT_EQUAL_UINT8(8, c.hw.card_gap);
    TEST_ASSERT_EQUAL_STRING("", c.hw.timezone);
    // Widgets: boot_splash, clock, then 8 scoreboards (mlb first, rest sorted)
    TEST_ASSERT_EQUAL_UINT16(10, c.widget_count);
    TEST_ASSERT_EQUAL_STRING("boot_splash", c.widgets[0].id);
    TEST_ASSERT_EQUAL_STRING("clock", c.widgets[1].id);
    TEST_ASSERT_EQUAL_STRING("scoreboard_mlb", c.widgets[2].id);
    TEST_ASSERT_EQUAL_UINT8(1, c.widgets[2].enabled);
    TEST_ASSERT_EQUAL_STRING("mlb", c.widgets[2].league);
    TEST_ASSERT_EQUAL_STRING("scoreboard_college-football", c.widgets[3].id);
    TEST_ASSERT_EQUAL_STRING("scoreboard_womens-college-basketball",
                             c.widgets[c.widget_count - 1].id);  // longest id fits
    for (uint16_t i = 3; i < c.widget_count; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, c.widgets[i].enabled);
    // LeagueConfig: one row per slug, only mlb enabled, 20/120 cadence
    TEST_ASSERT_EQUAL_UINT16(kLeagueSlugCount, c.league_count);
    int mlb_rows = 0;
    for (uint16_t i = 0; i < c.league_count; ++i) {
        if (strcmp(c.leagues[i].id, "mlb") == 0) {
            ++mlb_rows;
            TEST_ASSERT_EQUAL_UINT8(1, c.leagues[i].enabled);
        } else {
            TEST_ASSERT_EQUAL_UINT8(0, c.leagues[i].enabled);
        }
        TEST_ASSERT_EQUAL_UINT16(20, c.leagues[i].poll_interval_live);
        TEST_ASSERT_EQUAL_UINT16(120, c.leagues[i].poll_interval_idle);
    }
    TEST_ASSERT_EQUAL_INT(1, mlb_rows);
    TEST_ASSERT_EQUAL_UINT16(0, c.favorite_count);
}

// T-4.2 — corruption gate shared with the device store: garbage, zeros,
// wrong schema and out-of-range counts must all be rejected (defaults
// fallback); a valid blob accepted. The device test writes real garbage
// to NVS; this pins the validate() contract both sides use.
static void test_config_validate(void) {
    using namespace nb::config;
    Config good;
    set_defaults(good);
    TEST_ASSERT_TRUE(validate(good));

    Config junk;
    memset(&junk, 0xFF, sizeof(junk));
    TEST_ASSERT_FALSE(validate(junk));
    memset(&junk, 0, sizeof(junk));
    TEST_ASSERT_FALSE(validate(junk));

    Config bad = good;
    bad.schema = kSchemaVersion + 1;  // future/other schema → rebuild, not trust
    TEST_ASSERT_FALSE(validate(bad));
    bad = good;
    bad.widget_count = kMaxWidgets + 1;  // out-of-range count → reject
    TEST_ASSERT_FALSE(validate(bad));
    bad = good;
    bad.favorite_count = 0xFFFF;
    TEST_ASSERT_FALSE(validate(bad));
}

// T-4.4: restart flag on CHANGED structural values only. The Marquee fix:
// resubmitting unchanged fields (field "presence") must not flag; brightness
// and other live edits must not flag; each geometry/timing field must.
static void test_config_structural_change(void) {
    using namespace nb::config;
    Config a, b;
    set_defaults(a);
    b = a;
    TEST_ASSERT_FALSE(hw_structural_changed(a.hw, b.hw));  // full resubmit, no edits

    b = a; b.hw.brightness = 40;                     // live fields: never flag
    TEST_ASSERT_FALSE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.display_mode = 1 - a.hw.display_mode;
    TEST_ASSERT_FALSE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.scroll_speed = a.hw.scroll_speed + 1.5f;
    TEST_ASSERT_FALSE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.reserved = 0xAB;                     // padding must not flag
    TEST_ASSERT_FALSE(hw_structural_changed(a.hw, b.hw));

    b = a; b.hw.rows = 16;                            TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.cols = 128;                           TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.chain_length = 2;                     TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.parallel = 2;                         TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.lsb_msb_transition_bit = 0;           TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.clkphase = 1;                         TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.latch_blanking = 3;                   TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.i2sspeed = 1;                         TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
    b = a; b.hw.double_buff = 1 - a.hw.double_buff;   TEST_ASSERT_TRUE(hw_structural_changed(a.hw, b.hw));
}

// T-4.5: generated IANA->POSIX table. NY exact string is the PLAN accept
// case; Sydney proves the southern-hemisphere rule survives the round trip.
static void test_config_tzmap(void) {
    using namespace nb::config;
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", tz_lookup("America/New_York"));
    TEST_ASSERT_EQUAL_STRING("AEST-10AEDT,M10.1.0,M4.1.0/3", tz_lookup("Australia/Sydney"));
    TEST_ASSERT_EQUAL_STRING("UTC0", tz_lookup("UTC"));
    TEST_ASSERT_NULL(tz_lookup("Mars/Olympus_Mons"));
    TEST_ASSERT_NULL(tz_lookup(""));
    TEST_ASSERT_NULL(tz_lookup(nullptr));
    // binary search guard: table must be strcmp-sorted
    for (size_t i = 1; i < kTzCount; i++)
        TEST_ASSERT_TRUE(std::strcmp(kTzNamePool + kTzTable[i - 1].name_off,
                                     kTzNamePool + kTzTable[i].name_off) < 0);
}

// T-4.6: port of Marquee test_timezone.py sections A + C (B is the ESPN
// date window, lands in Phase 5). "Faked clock" = injected epoch.
static time_t mk_epoch(int y, int mo, int d, int h, int mi = 0, int s = 0) {
    struct tm u = {};
    u.tm_year = y - 1900; u.tm_mon = mo - 1; u.tm_mday = d;
    u.tm_hour = h; u.tm_min = mi; u.tm_sec = s;
    return timegm(&u);
}

static void local_at(time_t e, struct tm* t) { localtime_r(&e, t); }

static void test_config_timezone(void) {
    using namespace nb::config;
    // A: empty -> UTC with indicator status; named -> applied; unknown -> UTC fallback
    TEST_ASSERT_EQUAL_INT((int)TzResult::kUtcEmpty, (int)apply_timezone(""));
    TEST_ASSERT_EQUAL_INT((int)TzResult::kApplied, (int)apply_timezone("America/New_York"));
    TEST_ASSERT_EQUAL_INT((int)TzResult::kUtcUnknown, (int)apply_timezone("Mars/Olympus_Mons"));

    // Anchor from the Python: 2026-06-15 02:00 UTC = June 14 22:00 EDT
    apply_timezone("America/New_York");
    struct tm t;
    local_at(mk_epoch(2026, 6, 15, 2), &t);
    TEST_ASSERT_EQUAL_INT(2026 - 1900, t.tm_year);
    TEST_ASSERT_EQUAL_INT(5, t.tm_mon);  // June
    TEST_ASSERT_EQUAL_INT(14, t.tm_mday);
    TEST_ASSERT_EQUAL_INT(22, t.tm_hour);
    TEST_ASSERT_EQUAL_INT(1, t.tm_isdst);

    // NY spring-forward 2026-03-08 02:00 local (07:00 UTC)
    local_at(mk_epoch(2026, 3, 8, 6, 59), &t);
    TEST_ASSERT_EQUAL_INT(8, t.tm_mday); TEST_ASSERT_EQUAL_INT(1, t.tm_hour); TEST_ASSERT_EQUAL_INT(0, t.tm_isdst);
    local_at(mk_epoch(2026, 3, 8, 7, 0), &t);
    TEST_ASSERT_EQUAL_INT(8, t.tm_mday); TEST_ASSERT_EQUAL_INT(3, t.tm_hour); TEST_ASSERT_EQUAL_INT(1, t.tm_isdst);
    // fall-back 2026-11-01 02:00 local (06:00 UTC)
    local_at(mk_epoch(2026, 11, 1, 5, 59), &t);
    TEST_ASSERT_EQUAL_INT(1, t.tm_hour); TEST_ASSERT_EQUAL_INT(1, t.tm_isdst);
    local_at(mk_epoch(2026, 11, 1, 6, 0), &t);
    TEST_ASSERT_EQUAL_INT(1, t.tm_hour); TEST_ASSERT_EQUAL_INT(0, t.tm_isdst);

    // Southern hemisphere: Sydney springs forward 2026-10-04 02:00 AEST (16:00 UTC)
    apply_timezone("Australia/Sydney");
    local_at(mk_epoch(2026, 10, 3, 15, 59), &t);
    TEST_ASSERT_EQUAL_INT(4, t.tm_mday); TEST_ASSERT_EQUAL_INT(1, t.tm_hour); TEST_ASSERT_EQUAL_INT(0, t.tm_isdst);
    local_at(mk_epoch(2026, 10, 3, 16, 0), &t);
    TEST_ASSERT_EQUAL_INT(4, t.tm_mday); TEST_ASSERT_EQUAL_INT(3, t.tm_hour); TEST_ASSERT_EQUAL_INT(1, t.tm_isdst);

    // C: the local-date-of-now that the Phase 5 window filter keys on:
    // now = 2026-06-15 02:00 UTC -> "today" is June 15 in UTC, June 14 in NY.
    const time_t now = mk_epoch(2026, 6, 15, 2);
    apply_timezone("UTC");
    local_at(now, &t);
    const int utc_day = t.tm_mday;
    apply_timezone("America/New_York");
    local_at(now, &t);
    TEST_ASSERT_EQUAL_INT(15, utc_day);
    TEST_ASSERT_EQUAL_INT(14, t.tm_mday);  // June 14 22:00 EDT
}

// T-5.1: POD data structs. Fixed-size, memcpy-able, sentinel optionals —
// the shape the pointer-swap cache publishes whole.
static void test_data_structs(void) {
    using namespace nb::data;
    // POD: the cache memcpy's GameList wholesale.
    static_assert(std::is_trivially_copyable<GameList>::value, "GameList must stay trivially copyable");
    static_assert(std::is_trivially_copyable<Game>::value, "Game must stay trivially copyable");
    static_assert(std::is_trivially_copyable<Situation>::value, "");
    static_assert(std::is_trivially_copyable<Team>::value, "");

    // Budget: whole DataCache (8 leagues x 2 buffers) must stay PSRAM-small.
    std::printf("  sizeof Team=%zu Game=%zu GameList=%zu  cache=%zu B\n", sizeof(Team),
                sizeof(Game), sizeof(GameList), sizeof(GameList) * 2 * 8);
    TEST_ASSERT_EQUAL_UINT(60, sizeof(Team));
    TEST_ASSERT_TRUE(sizeof(GameList) * 2 * 8 < 128u * 1024);

    Game g = {};
    g.period = kNoInt;
    g.away_score = kNoInt;
    TEST_ASSERT_EQUAL_INT(INT16_MIN, g.away_score);  // the None sentinel
    copy_str(g.id, sizeof g.id, "40123456789012345678");  // over-long: truncates, stays terminated
    TEST_ASSERT_EQUAL_UINT(15, static_cast<unsigned>(std::strlen(g.id)));
    copy_str(g.status_display, sizeof g.status_display, "Top 3rd");
    TEST_ASSERT_EQUAL_STRING("Top 3rd", g.status_display);
    copy_str(g.clock, sizeof g.clock, nullptr);  // missing field -> "", never crash
    TEST_ASSERT_EQUAL_STRING("", g.clock);

    // Whole-list copy is a real value copy (render snapshots this way).
    GameList a = {};
    a.count = 2;
    a.fetched_utc = 1234567890;
    copy_str(a.games[1].home.abbr, sizeof(a.games[1].home.abbr), "LAL");
    GameList b = a;
    copy_str(a.games[1].home.abbr, sizeof(a.games[1].home.abbr), "BOS");
    TEST_ASSERT_EQUAL_STRING("LAL", b.games[1].home.abbr);
    TEST_ASSERT_EQUAL_INT(1234567890, (int)b.fetched_utc);
}

// T-5.2: basic DataCache semantics — never-published, fill-visible-late,
// freshness/staleness (Marquee cache.py: factor 2.5 x poll interval).
static void test_data_cache_basic(void) {
    using namespace nb::data;
    static DataCache cache;  // 58 KB: static, never on a task stack (T-4.2 habit)

    GameList snap;
    TEST_ASSERT_FALSE(cache.snapshot(0, &snap));
    TEST_ASSERT_EQUAL_INT(-1, (int)cache.last_fetch_age(0, 5000));
    TEST_ASSERT_TRUE(cache.is_stale(0, 20, 5000));

    GameList* w = cache.writable(0);
    fill_generation(w, 7);
    TEST_ASSERT_TRUE(check_generation(*w) == 0);
    // Pre-publish the cache still serves nothing, and a snapshot during a
    // fill (poll parsing, render running) must see the OLD list, not the
    // half-filled buffer.
    cache.publish(0, 1000);
    TEST_ASSERT_TRUE(cache.snapshot(0, &snap));
    TEST_ASSERT_TRUE(check_generation(snap) == 0);
    TEST_ASSERT_EQUAL_INT(7, (int)snap.games[0].start_utc);
    TEST_ASSERT_EQUAL_INT(10, (int)cache.last_fetch_age(0, 1010));
    TEST_ASSERT_FALSE(cache.is_stale(0, 20, 1050));  // 50 <= 20*2.5
    TEST_ASSERT_TRUE(cache.is_stale(0, 20, 1051));   // 51 >  50

    GameList* w2 = cache.writable(0);
    TEST_ASSERT_TRUE(cache.snapshot(0, &snap) && snap.games[0].start_utc == 7);  // old still current
    fill_generation(w2, 8);
    cache.publish(0, 2000);
    TEST_ASSERT_TRUE(cache.snapshot(0, &snap));
    TEST_ASSERT_EQUAL_INT(8, (int)snap.games[0].start_utc);

    // Empty slate (all games finished = count 0) still counts as fetched.
    GameList* w3 = cache.writable(0);
    w3->count = 0;
    cache.publish(0, 3000);
    TEST_ASSERT_TRUE(cache.snapshot(0, &snap));
    TEST_ASSERT_EQUAL_INT(0, snap.count);
    TEST_ASSERT_EQUAL_INT(0, (int)cache.last_fetch_age(0, 3000));
}

// T-5.2 accept: "writing at 50 Hz while reading at 30 Hz produces no torn
// reads — verify explicitly; this is the rule the Python never had to obey."
// Two phases. First the CONTROL: the exact forbidden pattern (mutate the
// current list in place, reader holds a pointer to it) must FAIL this
// detector — proving the harness has teeth. Then the real DataCache with
// the same adversarial traffic must show zero violations.
static void test_data_cache_stress(void) {
    using namespace nb::data;
    using clock = std::chrono::steady_clock;
    const auto dur = std::chrono::milliseconds(1500);

    // --- control: forbidden in-place mutation. Two threads, one buffer. ---
    alignas(GameList) static unsigned char control_mem[sizeof(GameList)];
    GameList* shared = reinterpret_cast<GameList*>(control_mem);
    {
        std::atomic<bool> stop{false};
        std::atomic<int> torn{0};
        std::thread writer([&] {
            uint32_t gen = 0;
            while (!stop.load()) {
                fill_generation(shared, ++gen);  // in place: the forbidden pattern
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        std::thread reader([&] {  // "render": holds the pointer across the read
            const auto t0 = clock::now();
            volatile uint32_t spin = 0;
            while (!stop.load() && clock::now() - t0 < dur) {
                GameList* held = shared;  // no swap, no seqlock: same object
                int saw = -1;
                for (int i = 0; i < held->count; i++) {  // dwell ~game like a strip rebuild
                    int64_t g = held->games[i].start_utc;
                    for (uint32_t k = 0; k < 2000; k++) spin += k;  // work between games
                    if (g != saw && saw != -1) { torn.fetch_add(100); break; }
                    saw = (int)g;
                }
                if (check_generation(*held) != 0) torn.fetch_add(1);
            }
            stop.store(true);
        });
        writer.join(); reader.join();
        std::printf("  control (forbidden in-place mutation): %d violations — harness has teeth\n",
                    torn.load());
        TEST_ASSERT_TRUE_MESSAGE(torn.load() > 0,
                                 "stress detector cannot see a torn read — accept criteria void");
    }

    // --- real DataCache: same dual-core-style adversarial race. ---
    static DataCache cache;
    {
        std::atomic<bool> stop{false};
        std::atomic<int> torn{0}, reads{0}, busy{0}, max_busy{0};
        std::thread writer([&] {  // poll: 50 Hz cycles x 4 leagues = 200 Hz per league
            uint32_t gen = 0;
            const auto t0 = clock::now();
            while (!stop.load() && clock::now() - t0 < dur) {
                for (int l = 0; l < 4; l++) {
                    GameList* b = cache.writable(l);
                    fill_generation(b, ++gen);
                    cache.publish(l, gen);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            stop.store(true);
        });
        std::thread reader([&] {  // render: 30 Hz snapshots
            GameList* snap = new GameList;
            const auto t0 = clock::now();
            while (!stop.load() && clock::now() - t0 < dur) {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                if (!cache.snapshot(0, snap)) {
                    int b = ++busy;
                    if (b > max_busy.load()) max_busy.store(b);
                    continue;
                }
                reads++;
                int bad = check_generation(*snap);
                if (bad) torn.fetch_add(1);
            }
            delete snap;
        });
        writer.join(); reader.join();
        std::printf("  DataCache stress: %d reads, %d contended retries (max %d), %d torn\n",
                    reads.load(), busy.load(), max_busy.load(), torn.load());
        TEST_ASSERT_EQUAL_MESSAGE(0, torn.load(), "TORN READ in DataCache");
        TEST_ASSERT_TRUE(reads.load() > 10);
    }
}

// T-5.4 — filter + NestingLimit(20) proof on the exact code the device
// parses with. Fixtures are the Marquee corpus (never read whole by humans).
static void test_data_filter_mlb(void) {
    using namespace nb::data;
    std::ifstream f("test/fixtures/mlb_scoreboard.json");
    TEST_ASSERT_TRUE_MESSAGE(f.is_open(), "test/fixtures/mlb_scoreboard.json missing");

    JsonDocument filter;
    build_scoreboard_filter(filter);
    JsonDocument doc;  // host: default allocator (device: SpiRam, same code)
    const DeserializationError err = parse_scoreboard(f, filter, doc);
    TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(err), err.c_str());

    const size_t kept = measureJson(doc);
    std::printf("  mlb_scoreboard: %lu events, %lu B kept of 1457268 (NestingLimit 20 held)\n",
                (unsigned long)doc["events"].size(), (unsigned long)kept);
    TEST_ASSERT_EQUAL_UINT(15, doc["events"].size());
    TEST_ASSERT_TRUE_MESSAGE(kept > 0 && kept <= 8192, "filtered doc must retain <= 8 KB");

    // retained fields present, bulk of the payload provably gone
    TEST_ASSERT_TRUE(doc["events"][0]["id"].is<const char*>());
    TEST_ASSERT_TRUE(doc["events"][0]["competitions"][0]["status"]["type"]["state"].is<const char*>());
    TEST_ASSERT_TRUE(doc["events"][0]["competitions"][0]["competitors"][0]["team"]["abbreviation"].is<const char*>());
    TEST_ASSERT_TRUE(doc["events"][0]["competitions"][0]["venue"].isNull());
    TEST_ASSERT_TRUE(doc["events"][0]["competitions"][0]["odds"].isNull());
}

// T-5.5 — to_games must match the Python's norm_game field-for-field over
// all six fixtures. Goldens come from the Marquee's own code
// (tools/gen_games_golden.py); the C++ never sees the Python at test time.
static const char* const kFixtures[] = {
    "epl_scoreboard", "mlb_live", "mlb_scoreboard", "nba_scoreboard",
    "nfl_live", "nhl_scoreboard",
};

static int16_t gold_int(JsonVariantConst v) { return v.isNull() ? nb::data::kNoInt : (int16_t)v.as<int>(); }
static int gold_status(JsonVariantConst v) {
    const char* s = v.as<const char*>();
    return !strcmp(s, "in") ? nb::data::kStatusIn : !strcmp(s, "post") ? nb::data::kStatusPost : nb::data::kStatusPre;
}

static void gold_team(JsonObjectConst g, const nb::data::Team& t, const char* what, int& bad) {
    if (strcmp(g["id"].as<const char*>(), t.id) ||
        strcmp(g["name"].as<const char*>(), t.name) ||
        strcmp(g["abbr"].as<const char*>(), t.abbr) ||
        (uint32_t)strtoul(g["colour"].as<const char*>(), nullptr, 16) != t.colour) {
        ++bad;
        std::printf("    %s team: got id=%s name=%s abbr=%s col=%06x want id=%s name=%s abbr=%s col=%s\n",
                    what, t.id, t.name, t.abbr, t.colour,
                    g["id"].as<const char*>(), g["name"].as<const char*>(),
                    g["abbr"].as<const char*>(), g["colour"].as<const char*>());
    }
}

static void test_data_norm_golden(void) {
    using namespace nb::data;
    for (const char* name : kFixtures) {
        char path[128];
        std::snprintf(path, sizeof path, "test/fixtures/%s.json", name);
        std::ifstream f(path);
        TEST_ASSERT_TRUE_MESSAGE(f.is_open(), path);
        JsonDocument filter;
        build_scoreboard_filter(filter);
        JsonDocument doc;
        TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(parse_scoreboard(f, filter, doc)), name);

        static GameList list;
        const int n = to_games(doc, list);

        std::snprintf(path, sizeof path, "test/golden/%s.json", name);
        std::ifstream gf(path);
        TEST_ASSERT_TRUE_MESSAGE(gf.is_open(), path);
        JsonDocument gold;
        TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(deserializeJson(gold, gf)), name);
        TEST_ASSERT_EQUAL_INT(gold.size(), n);

        int bad = 0;
        int i = 0;
        for (JsonObjectConst g : gold.as<JsonArrayConst>()) {
            const Game& gm = list.games[i++];
            if (strcmp(g["id"].as<const char*>(), gm.id)) { ++bad; std::printf("    id: %s != %s\n", gm.id, g["id"].as<const char*>()); }
            if (gm.status != (uint8_t)gold_status(g["status"])) { ++bad; std::printf("    %s: status %d != %s\n", gm.id, gm.status, g["status"].as<const char*>()); }
            if (strcmp(g["status_display"].as<const char*>(), gm.status_display)) { ++bad; std::printf("    %s: status_display %s != %s\n", gm.id, gm.status_display, g["status_display"].as<const char*>()); }
            if (gm.period != gold_int(g["period"])) { ++bad; std::printf("    %s: period\n", gm.id); }
            if (strcmp(g["clock"].isNull() ? "" : g["clock"].as<const char*>(), gm.clock)) { ++bad; std::printf("    %s: clock %s\n", gm.id, gm.clock); }
            gold_team(g["away"], gm.away, gm.id, bad);
            gold_team(g["home"], gm.home, gm.id, bad);
            if (gm.away_score != gold_int(g["away_score"]) || gm.home_score != gold_int(g["home_score"])) { ++bad; std::printf("    %s: score %d-%d\n", gm.id, gm.away_score, gm.home_score); }
            if (gm.start_utc != (int64_t)g["start_utc"].as<int64_t>()) { ++bad; std::printf("    %s: start %lld != %lld\n", gm.id, (long long)gm.start_utc, (long long)g["start_utc"].as<int64_t>()); }
            JsonObjectConst s = g["situation"].is<JsonObjectConst>() ? g["situation"].as<JsonObjectConst>() : JsonObjectConst();
            const bool gold_has = g["situation"].is<JsonObjectConst>();
            if ((bool)gold_has != (bool)gm.has_situation) { ++bad; std::printf("    %s: situation presence\n", gm.id); continue; }
            if (!gold_has) continue;
            if (s["on_first"].as<bool>() != !!gm.situation.on_first ||
                s["on_second"].as<bool>() != !!gm.situation.on_second ||
                s["on_third"].as<bool>() != !!gm.situation.on_third ||
                s["is_red_zone"].as<bool>() != !!gm.situation.is_red_zone) { ++bad; std::printf("    %s: situation bases\n", gm.id); }
            if (gm.situation.balls != gold_int(s["balls"]) || gm.situation.strikes != gold_int(s["strikes"]) ||
                gm.situation.outs != gold_int(s["outs"]) || gm.situation.down != gold_int(s["down"]) ||
                gm.situation.distance != gold_int(s["distance"]) || gm.situation.yard_line != gold_int(s["yard_line"])) {
                ++bad; std::printf("    %s: situation counts down=%d dist=%d yard=%d\n", gm.id, gm.situation.down, gm.situation.distance, gm.situation.yard_line);
            }
            if (strcmp(s["possession"].isNull() ? "" : s["possession"].as<const char*>(), gm.situation.possession)) { ++bad; std::printf("    %s: possession %s\n", gm.id, gm.situation.possession); }
        }
        std::printf("  %s: %d games match the Python field-for-field\n", name, n);
        TEST_ASSERT_EQUAL_MESSAGE(0, bad, name);
    }
}

// T-5.6 — port of the Python's _filter_by_date + the ?dates= day string.
static time_t utc(int y, int mo, int d, int h, int mi) {
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
    return timegm(&t);
}

static void test_data_date_window(void) {
    using namespace nb::data;
    setenv("TZ", "America/New_York", 1);
    tzset();

    // 2026-06-15T03:00Z = 23:00 EDT Jun 14 -> local today 20260614
    const time_t now = utc(2026, 6, 15, 3, 0);
    char day[16];
    TEST_ASSERT_TRUE(local_day(day, sizeof day, now));
    TEST_ASSERT_EQUAL_STRING("20260614", day);

    static GameList list;
    memset(&list, 0, sizeof list);
    list.count = 5;
    list.games[0].start_utc = utc(2026, 6, 14, 16, 15);  // today 16:15 EDT — keep
    list.games[1].start_utc = utc(2026, 6, 13, 23, 30);  // yesterday        — keep
    list.games[2].start_utc = utc(2026, 6, 12, 23, 0);   // two days ago     — drop
    list.games[3].start_utc = utc(2026, 6, 16, 2, 0);    // = Jun 15 22:00 EDT — drop
    list.games[4].start_utc = 0;                          // date-unparseable — keep
    for (int i = 0; i < 5; ++i) std::snprintf(list.games[i].id, 16, "%d", i);

    TEST_ASSERT_EQUAL_INT(3, filter_yesterday_today(&list, now));
    TEST_ASSERT_EQUAL_STRING("0", list.games[0].id);
    TEST_ASSERT_EQUAL_STRING("1", list.games[1].id);
    TEST_ASSERT_EQUAL_STRING("4", list.games[2].id);

    // DST edge: 2026-03-09T06:30Z = 01:30 EDT Mar 9; the night before was the
    // spring-forward. Yesterday is still Mar 8 even though it was 23 h long.
    const time_t dst_now = utc(2026, 3, 9, 6, 30);
    TEST_ASSERT_TRUE(local_day(day, sizeof day, dst_now));
    TEST_ASSERT_EQUAL_STRING("20260309", day);
    // local_yesterday: plain day, DST night (23 h), and month boundary.
    TEST_ASSERT_TRUE(local_yesterday(day, sizeof day, now));
    TEST_ASSERT_EQUAL_STRING("20260613", day);
    TEST_ASSERT_TRUE(local_yesterday(day, sizeof day, dst_now));
    TEST_ASSERT_EQUAL_STRING("20260308", day);
    const time_t month_edge = utc(2026, 3, 1, 14, 0);  // 09:00 EST Mar 1
    TEST_ASSERT_TRUE(local_yesterday(day, sizeof day, month_edge));
    TEST_ASSERT_EQUAL_STRING("20260228", day);
    list.count = 2;
    list.games[0].start_utc = utc(2026, 3, 8, 18, 0);  // Mar 8 afternoon EDT — keep
    list.games[1].start_utc = utc(2026, 3, 6, 18, 0);  // Mar 6 — drop
    TEST_ASSERT_EQUAL_INT(1, filter_yesterday_today(&list, dst_now));
    TEST_ASSERT_EQUAL_STRING("0", list.games[0].id);
}

static void test_data_poll_scheduler(void) {
    using namespace nb::data;
    PollScheduler sch;
    const int64_t t0 = 1000000;

    PollCfg live{true, 20, 120}, off{false, 20, 120};
    for (int i = 0; i < PollScheduler::kLeagues; ++i) sch.set(i, off);
    sch.set(0, live);
    sch.set(5, live);
    sch.reset(t0);

    // Boot stagger: league 0 due now, league 5 five*5 s later; disabled never.
    TEST_ASSERT_TRUE(sch.due(0, t0));
    TEST_ASSERT_FALSE(sch.due(5, t0));
    TEST_ASSERT_TRUE(sch.due(5, t0 + 5 * PollScheduler::kBootStaggerS));
    sch.set(3, live);  // league 3 armed at t0+15
    TEST_ASSERT_EQUAL_INT64(t0, sch.next_wake(t0));

    // Idle cadence, then a live fetch flips to the live cadence.
    sch.done(0, t0, false);
    TEST_ASSERT_FALSE(sch.due(0, t0 + 119));
    TEST_ASSERT_TRUE(sch.due(0, t0 + 120));
    sch.done(0, t0 + 120, true);
    TEST_ASSERT_TRUE(sch.due(0, t0 + 140));
    TEST_ASSERT_EQUAL_INT64(t0 + 15, sch.next_wake(t0));  // league 3 leads

    // All-disabled next_wake falls back to now (caller sleeps its floor).
    sch.set(0, off);
    sch.set(3, off);
    sch.set(5, off);
    TEST_ASSERT_EQUAL_INT64(t0 + 999, sch.next_wake(t0 + 999));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_canvas_alloc_strip);
    RUN_TEST(test_rgb565_pack);
    RUN_TEST(test_text_width);
    RUN_TEST(test_primitives);
    RUN_TEST(test_font_sheet);
    RUN_TEST(test_clock_parity);
    RUN_TEST(test_blit_logo_parity);
    RUN_TEST(test_abbr_fallback);
    RUN_TEST(test_logos_parse_host);
    RUN_TEST(test_config_defaults);
    RUN_TEST(test_config_validate);
    RUN_TEST(test_config_structural_change);
    RUN_TEST(test_config_tzmap);
    RUN_TEST(test_config_timezone);
    RUN_TEST(test_data_structs);
    RUN_TEST(test_data_cache_basic);
    RUN_TEST(test_data_cache_stress);
    RUN_TEST(test_data_filter_mlb);
    RUN_TEST(test_data_norm_golden);
    RUN_TEST(test_data_date_window);
    RUN_TEST(test_data_poll_scheduler);
    return UNITY_END();
}
