// SPDX-License-Identifier: GPL-3.0-only

#include "primitives.h"

#include <algorithm>

namespace nb {

void point(Canvas16& c, int x, int y, uint16_t col) { c.set(x, y, col); }

void hline(Canvas16& c, int x0, int x1, int y, uint16_t col) {
    if (x0 > x1) std::swap(x0, x1);
    for (int x = x0; x <= x1; ++x) c.set(x, y, col);
}

void vline(Canvas16& c, int x, int y0, int y1, uint16_t col) {
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) c.set(x, y, col);
}

void line(Canvas16& c, int x0, int y0, int x1, int y1, uint16_t col) {
    // Bresenham.
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        c.set(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fill_rect(Canvas16& c, int x, int y, int w, int h, uint16_t col) {
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) c.set(x + i, y + j, col);
}

void rect(Canvas16& c, int x, int y, int w, int h, uint16_t col) {
    if (w <= 0 || h <= 0) return;
    hline(c, x, x + w - 1, y, col);
    hline(c, x, x + w - 1, y + h - 1, col);
    vline(c, x, y, y + h - 1, col);
    vline(c, x + w - 1, y, y + h - 1, col);
}

void ellipse(Canvas16& c, int cx, int cy, int rx, int ry, uint16_t col) {
    if (rx < 0 || ry < 0) return;
    if (rx == 0 || ry == 0) {  // degenerate: a line/point
        hline(c, cx - rx, cx + rx, cy, col);
        vline(c, cx, cy - ry, cy + ry, col);
        return;
    }
    const long rx2 = 1L * rx * rx, ry2 = 1L * ry * ry;
    int x = 0, y = ry;
    long p1 = ry2 - rx2 * ry + rx2 / 2;
    while (2L * ry2 * x <= 2L * rx2 * y) {  // region 1: slope magnitude < 1
        c.set(cx + x, cy + y, col); c.set(cx - x, cy + y, col);
        c.set(cx + x, cy - y, col); c.set(cx - x, cy - y, col);
        ++x;
        if (p1 < 0) {
            p1 += 2L * ry2 * x + ry2;
        } else {
            --y;
            p1 += 2L * (ry2 * x - rx2 * y) + rx2;
        }
    }
    long p2 = 2L * ry2 * x * x + ry2 * (2L * x + 1) * (2L * x + 1) +
              2L * rx2 * (y - 1) * (y - 1) - 2L * rx2 * ry2;
    while (y >= 0) {  // region 2: step in y
        c.set(cx + x, cy + y, col); c.set(cx - x, cy + y, col);
        c.set(cx + x, cy - y, col); c.set(cx - x, cy - y, col);
        --y;
        if (p2 > 0) {
            ++x;
            p2 += 2L * (ry2 * x - rx2 * y) + rx2;
        } else {
            p2 += 2L * ry2 * (2L * x + 1) + 2L * rx2;
        }
    }
}

void fill_ellipse(Canvas16& c, int cx, int cy, int rx, int ry, uint16_t col) {
    if (rx < 0 || ry < 0) return;
    for (int y = -ry; y <= ry; ++y) {
        const long num = 1L * rx * rx * (ry * ry - y * y);
        if (num < 0) continue;
        long hx = 0;
        while (1L * (hx + 1) * (hx + 1) * ry * ry <= num) ++hx;
        hline(c, static_cast<int>(cx - hx), static_cast<int>(cx + hx), cy + y, col);
    }
}

namespace {
inline int round_down(float v) {
    int i = static_cast<int>(v);
    if (v < i) --i;  // floor for negatives
    return i;
}
}  // namespace

void fill_polygon(Canvas16& c, const Point* pts, int n, uint16_t col) {
    if (n < 3) return;
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; ++i) {
        ymin = std::min(ymin, pts[i].y);
        ymax = std::max(ymax, pts[i].y);
    }
    float xs[64];  // ponytail: fixed cap; raise to dynamic if a widget needs >32 edges
    for (int y = ymin; y <= ymax; ++y) {
        const float cy = y + 0.5f;  // sample at pixel centres
        int k = 0;
        for (int i = 0; i < n; ++i) {
            const Point& a = pts[i];
            const Point& b = pts[(i + 1) % n];
            if ((a.y <= cy && b.y > cy) || (b.y <= cy && a.y > cy)) {
                const float t = (cy - a.y) / (b.y - a.y);
                if (k < 64) xs[k++] = a.x + t * (b.x - a.x);
            }
        }
        std::sort(xs, xs + k);
        for (int j = 0; j + 1 < k; j += 2) {
            const int x0 = round_down(xs[j] + 0.5f);
            const int x1 = round_down(xs[j + 1] + 0.5f);
            hline(c, x0, x1 - 1, y, col);
        }
    }
}

}  // namespace nb
