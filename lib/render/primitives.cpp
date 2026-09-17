// SPDX-License-Identifier: GPL-3.0-only

#include "primitives.h"

#include <algorithm>
#include <cmath>

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
    if (rx == 2 && ry == 2) {  // Pillow's exact 5x5 outline used by out dots.
        hline(c, cx - 1, cx + 1, cy - 2, col);
        hline(c, cx - 1, cx + 1, cy + 2, col);
        for (int y = cy - 1; y <= cy + 1; ++y) {
            point(c, cx - 2, y, col);
            point(c, cx + 2, y, col);
        }
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

constexpr int kMaxEdges = 64;

struct Edge {
    int x0, y0;
    int xmin, ymin, xmax, ymax;
    float dx;
};

inline int round_up(float v) {
    return v >= 0.0f ? static_cast<int>(std::floor(v + 0.5f))
                     : -static_cast<int>(std::floor(std::fabs(v) + 0.5f));
}

inline int round_down(float v) {
    return v >= 0.0f ? static_cast<int>(std::ceil(v - 0.5f))
                     : -static_cast<int>(std::ceil(std::fabs(v) - 0.5f));
}

inline void add_edge(Edge& e, int x0, int y0, int x1, int y1) {
    if (x0 <= x1) {
        e.xmin = x0;
        e.xmax = x1;
    } else {
        e.xmin = x1;
        e.xmax = x0;
    }
    if (y0 <= y1) {
        e.ymin = y0;
        e.ymax = y1;
    } else {
        e.ymin = y1;
        e.ymax = y0;
    }
    if (y0 == y1) {
        e.dx = 0.0f;
    } else {
        e.dx = static_cast<float>(x1 - x0) / static_cast<float>(y1 - y0);
    }
    e.x0 = x0;
    e.y0 = y0;
}

}  // namespace

void fill_polygon(Canvas16& c, const Point* pts, int n, uint16_t col) {
    if (n < 3 || n > kMaxEdges) return;

    Edge edges[kMaxEdges];
    Edge* edge_table[kMaxEdges];
    float xx[2 * kMaxEdges];
    int n_edges = 0;

    for (int i = 0; i < n - 1; ++i) {
        const int x0 = pts[i].x, y0 = pts[i].y;
        const int x1 = pts[i + 1].x, y1 = pts[i + 1].y;

        if (y0 == y1 && i != 0 && y0 == pts[i - 1].y && n_edges > 0) {
            Edge& last = edges[n_edges - 1];
            if (x1 > x0 && x0 > pts[i - 1].x) {
                last.xmax = x1;
                continue;
            }
            if (x1 < x0 && x0 < pts[i - 1].x) {
                last.xmin = x1;
                continue;
            }
        }

        add_edge(edges[n_edges++], x0, y0, x1, y1);
    }

    if (pts[n - 1].x != pts[0].x || pts[n - 1].y != pts[0].y) {
        add_edge(edges[n_edges++], pts[n - 1].x, pts[n - 1].y, pts[0].x, pts[0].y);
    }

    int ymin = c.h - 1;
    int ymax = 0;
    int edge_count = 0;

    for (int i = 0; i < n_edges; ++i) {
        Edge& e = edges[i];
        if (ymin > e.ymin) ymin = e.ymin;
        if (ymax < e.ymax) ymax = e.ymax;
        if (e.ymin == e.ymax) {
            if (e.xmin <= e.xmax) hline(c, e.xmin, e.xmax, e.ymin, col);
            continue;
        }
        edge_table[edge_count++] = &e;
    }

    if (ymin < 0) ymin = 0;
    if (ymax > c.h) ymax = c.h;

    for (; ymin <= ymax; ++ymin) {
        int j = 0;
        for (int i = 0; i < edge_count; ++i) {
            Edge* cur = edge_table[i];
            if (ymin < cur->ymin || ymin > cur->ymax) continue;

            xx[j++] = (ymin - cur->y0) * cur->dx + cur->x0;

            if (ymin == cur->ymax && ymin < ymax) {
                xx[j] = xx[j - 1];
                ++j;
            } else if ((ymin == cur->ymin || ymin == cur->ymax) && cur->dx != 0.0f) {
                for (int k = 0; k < i; ++k) {
                    Edge* other = edge_table[k];
                    if ((ymin != other->ymin && ymin != other->ymax) ||
                        other->dx == 0.0f)
                        continue;

                    const float other_x =
                        (ymin - other->y0) * other->dx + other->x0;
                    if (std::round(xx[j - 1]) == std::round(other_x)) {
                        const int offset = ymin == cur->ymax ? -1 : 1;
                        const float adj_x =
                            (ymin + offset - cur->y0) * cur->dx + cur->x0;
                        if (ymin + offset >= other->ymin &&
                            ymin + offset <= other->ymax) {
                            const float adj_other =
                                (ymin + offset - other->y0) * other->dx +
                                other->x0;
                            if (xx[j - 1] > adj_x + 1.0f &&
                                xx[j - 1] > adj_other + 1.0f) {
                                const float mx =
                                    adj_x > adj_other ? adj_x : adj_other;
                                xx[j - 1] = std::round(mx) + 1.0f;
                            } else if (xx[j - 1] < adj_x - 1.0f &&
                                       xx[j - 1] < adj_other - 1.0f) {
                                const float mn =
                                    adj_x < adj_other ? adj_x : adj_other;
                                xx[j - 1] = std::round(mn) - 1.0f;
                            }
                            break;
                        }
                    }
                }
            }
        }

        std::sort(xx, xx + j);
        for (int i = 1; i < j; i += 2) {
            const int x0 = round_up(xx[i - 1]);
            const int x1 = round_down(xx[i]);
            if (x0 <= x1) hline(c, x0, x1, ymin, col);
        }
    }
}

}  // namespace nb
