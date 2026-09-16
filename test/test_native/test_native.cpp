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

#include "canvas.h"
#include "config.h"
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_canvas_alloc_strip);
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
    return UNITY_END();
}
