// SPDX-License-Identifier: GPL-3.0-only
//
// T-2.2 — drawing primitives on a Canvas16. Coordinates are screen pixels;
// everything is bounds-checked via Canvas16::set, so out-of-canvas draws clip.

#pragma once

#include <cstdint>

#include "canvas.h"

namespace nb {

void point(Canvas16& c, int x, int y, uint16_t col);
void hline(Canvas16& c, int x0, int x1, int y, uint16_t col);
void vline(Canvas16& c, int x, int y0, int y1, uint16_t col);
void line(Canvas16& c, int x0, int y0, int x1, int y1, uint16_t col);

void rect(Canvas16& c, int x, int y, int w, int h, uint16_t col);      // outline
void fill_rect(Canvas16& c, int x, int y, int w, int h, uint16_t col);  // solid

// Ellipse/circle: (cx, cy) centre, semi-axes rx, ry.
void ellipse(Canvas16& c, int cx, int cy, int rx, int ry, uint16_t col);
void fill_ellipse(Canvas16& c, int cx, int cy, int rx, int ry, uint16_t col);

// Convex/general polygon scanline fill. pts is a list of (x, y) pairs, n >= 3.
struct Point {
    int x, y;
};
void fill_polygon(Canvas16& c, const Point* pts, int n, uint16_t col);

}  // namespace nb
