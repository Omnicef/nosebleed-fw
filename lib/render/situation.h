// SPDX-License-Identifier: GPL-3.0-only
//
// T-6.6 — league situation indicators. Layout copied from
// Marquee marquee/render/situation/{diamond,indicators}.py.

#pragma once

#include <cstdio>
#include <cstring>

#include "../data/game.h"
#include "canvas.h"
#include "font.h"
#include "font_data.h"
#include "primitives.h"

namespace nb {
namespace render {

constexpr uint16_t SIT_COLOR_DIM = 0x8C71;    // rgb565(140, 140, 140)
constexpr uint16_t SIT_COLOR_ACCENT = 0xFE80;  // rgb565(255, 210, 0)
constexpr uint16_t SIT_COLOR_ARROW = 0xD69A;  // rgb565(210, 210, 210)
constexpr uint16_t SIT_COLOR_INNING = 0x94B2;  // rgb565(150, 150, 150)
constexpr uint16_t SIT_COLOR_COUNT = 0xFFFF;

inline void stroke_polygon(Canvas16& c, const Point* pts, int n, uint16_t col) {
    for (int i = 0; i < n; ++i) {
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % n];
        line(c, a.x, a.y, b.x, b.y, col);
    }
}

inline void draw_diamond(Canvas16& c, int x0, int y0, int width, int height,
                         const data::Situation& sit, uint16_t accent) {
    const int cx = x0 + width / 2;
    const int y_top = y0 + height * 14 / 32;
    const int y_mid = y0 + height * 20 / 32;
    const int spread = width * 5 / 64;
    const int dx = spread < 1 ? 1 : spread;

    const Point second[4] = {{cx, y_top - 2}, {cx + 2, y_top}, {cx, y_top + 2}, {cx - 2, y_top}};
    const Point first[4] = {{cx + dx, y_mid - 2}, {cx + dx + 2, y_mid}, {cx + dx, y_mid + 2}, {cx + dx - 2, y_mid}};
    const Point third[4] = {{cx - dx, y_mid - 2}, {cx - dx + 2, y_mid}, {cx - dx, y_mid + 2}, {cx - dx - 2, y_mid}};

    const Point* outer[3] = {second, first, third};
    const bool occupied[3] = {sit.on_second != 0, sit.on_first != 0, sit.on_third != 0};

    for (const Point* base : outer) stroke_polygon(c, base, 4, SIT_COLOR_DIM);

    for (int i = 0; i < 3; ++i) {
        if (!occupied[i]) continue;
        const Point* o = outer[i];
        Point inner[4] = {{o[0].x, o[0].y + 1}, {o[1].x - 1, o[1].y}, {o[2].x, o[2].y - 1}, {o[3].x + 1, o[3].y}};
        fill_polygon(c, inner, 4, accent);
    }
}

inline void draw_outs(Canvas16& c, int cx, int y, const data::Situation& sit) {
    if (sit.outs == data::kNoInt) return;
    const int xs[2] = {cx - 5, cx + 2};
    for (int i = 0; i < 2; ++i) {
        ellipse(c, xs[i], y, 2, 2, SIT_COLOR_DIM);
        if (i < static_cast<int>(sit.outs)) fill_rect(c, xs[i] - 1, y - 1, 3, 3, SIT_COLOR_ACCENT);
    }
}

inline void draw_inning(Canvas16& c, int x, int y, int16_t inning, int inning_top) {
    if (inning_top < 0 && inning == data::kNoInt) return;
    const int tri_cx = x + 5 / 2;
    const int tri_y = y + (8 - 3) / 2;
    if (inning_top == 1) {
        const Point tri[3] = {{tri_cx, tri_y}, {tri_cx - 2, tri_y + 3}, {tri_cx + 2, tri_y + 3}};
        fill_polygon(c, tri, 3, SIT_COLOR_ARROW);
    } else if (inning_top == 0) {
        const Point tri[3] = {{tri_cx - 2, tri_y}, {tri_cx + 2, tri_y}, {tri_cx, tri_y + 3}};
        fill_polygon(c, tri, 3, SIT_COLOR_ARROW);
    }

    if (inning == data::kNoInt) return;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d", static_cast<int>(inning));
    draw_text(c, FONT_SPLEEN_5X8, x + 7, y, buf, SIT_COLOR_INNING);
}

inline void draw_count(Canvas16& c, int x_right, int y, const data::Situation& sit) {
    if (sit.balls == data::kNoInt || sit.strikes == data::kNoInt) return;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d-%d", static_cast<int>(sit.balls), static_cast<int>(sit.strikes));
    const int cw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(buf)));
    draw_text(c, FONT_SPLEEN_5X8, x_right - cw, y, buf, SIT_COLOR_COUNT);
}

inline int inning_top_from_status(const char* status) {
    char low[24];
    size_t i = 0;
    for (; i + 1 < sizeof low && status[i] != '\0'; ++i) {
        char ch = status[i];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        low[i] = ch;
    }
    low[i] = '\0';
    if (std::strstr(low, "top") != nullptr) return 1;
    if (std::strstr(low, "bot") != nullptr || std::strstr(low, "bottom") != nullptr) return 0;
    return -1;
}

inline void draw_mlb_indicator(Canvas16& c, const data::Game& game) {
    const data::Situation& sit = game.situation;
    constexpr int cx = 32;
    draw_diamond(c, 0, 0, c.w, c.h, sit, SIT_COLOR_ACCENT);
    draw_outs(c, cx, c.h * 26 / 32, sit);

    const int info_y = c.h * 24 / 32;
    draw_inning(c, 2, info_y, game.period, inning_top_from_status(game.status_display));
    draw_count(c, c.w - 1, info_y, sit);
}

inline void draw_indicator(Canvas16& c, const data::Game& game, const char* league) {
    if (game.status != data::kStatusIn || game.has_situation == 0) return;
    if (league != nullptr && std::strcmp(league, "mlb") == 0) draw_mlb_indicator(c, game);
}

}  // namespace render
}  // namespace nb
