// SPDX-License-Identifier: GPL-3.0-only
//
// T-2.4/T-2.5 — fixed-width bitmap font blitter.
//
// Fonts are fixed-width (see PLAN.md T-2.4): text_width is len * advance, no
// per-glyph measurement. A glyph is stored baked into its FONTBOUNDINGBOX cell
// (rows[box_h], each row's low box_w bits MSB-first). draw_text pastes cell i
// at (x + i*advance, y) — the same model PIL uses for the Python's .pil fonts,
// which is what makes T-2.7 pixel parity possible.

#pragma once

#include <cstdint>

#include "canvas.h"

namespace nb {

struct Glyph {
    uint8_t rows[12];  // up to 12 box rows; only the first box_h are used
};

struct Font {
    const Glyph* glyphs;
    int first;      // ASCII code of glyphs[0]
    int count;      // number of glyphs
    uint8_t advance;
    uint8_t box_w;
    uint8_t box_h;
    int ascent;     // from FONT_ASCENT (informational)
};

// Fixed-width: no glyph lookup needed.
inline int text_width(const Font& f, int len) { return len * f.advance; }

// Draws s with the glyph mask top-left at (x, y). Missing glyphs render blank.
void draw_text(Canvas16& c, const Font& f, int x, int y, const char* s,
               uint16_t color);

// T-2.5 — 1px dark halo, matching the Python's _draw_outlined: draw at the
// eight ±1 neighbour offsets in `outline`, then once at (x, y) in `color`.
void draw_text_outlined(Canvas16& c, const Font& f, int x, int y,
                        const char* s, uint16_t color, uint16_t outline);

}  // namespace nb
