// SPDX-License-Identifier: GPL-3.0-only
//
// Host render tests (T-2.1, T-2.2, T-2.4, T-2.6, T-2.7). Built by `pio test
// -e native`. lib/render is compiled for the laptop here — that is the whole
// point of the purity rule. PNGs land in test/out/ for eyeballing; the hard
// gate is the RGB565 clock parity assertion (T-2.7).

#include "unity.h"

#include <cstring>

#include "canvas.h"
#include "font.h"
#include "font_data.h"
#include "golden_clock.h"
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_canvas_alloc_strip);
    RUN_TEST(test_text_width);
    RUN_TEST(test_primitives);
    RUN_TEST(test_font_sheet);
    RUN_TEST(test_clock_parity);
    return UNITY_END();
}
