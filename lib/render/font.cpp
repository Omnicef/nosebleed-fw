// SPDX-License-Identifier: GPL-3.0-only

#include "font.h"

namespace nb {

void draw_text(Canvas16& c, const Font& f, int x, int y, const char* s,
               uint16_t color) {
    if (!c.valid() || !s) return;
    int cx = x;
    for (const char* p = s; *p; ++p, cx += f.advance) {
        const int idx = static_cast<unsigned char>(*p) - f.first;
        if (idx < 0 || idx >= f.count) continue;  // out of table: blank
        const Glyph& g = f.glyphs[idx];
        for (int row = 0; row < f.box_h; ++row) {
            const unsigned bits = g.rows[row];
            if (!bits) continue;
            for (int col = 0; col < f.box_w; ++col) {
                if (bits & (1u << (f.box_w - 1 - col)))
                    c.set(cx + col, y + row, color);
            }
        }
    }
}

void draw_text_outlined(Canvas16& c, const Font& f, int x, int y,
                        const char* s, uint16_t color, uint16_t outline) {
    // Match the Python _draw_outlined: eight halo copies, then the fill on top.
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy) draw_text(c, f, x + dx, y + dy, s, outline);
    draw_text(c, f, x, y, s, color);
}

}  // namespace nb
