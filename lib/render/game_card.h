// SPDX-License-Identifier: GPL-3.0-only
//
// T-6.3+ — game card composition. Layout constants and branches copied from
// Marquee marquee/render/widgets/game_strip.py. Logo lookup is injected so
// lib/render stays host-testable and framework-free.

#pragma once

#include <cstdio>
#include <cstring>

#include "canvas.h"
#include "../data/game.h"
#include "font.h"
#include "font_data.h"
#include "logo.h"
#include "primitives.h"

#include "card_producer.h"
#include "local_time.h"
#include "situation.h"

namespace nb {
namespace render {

constexpr int LOGO_H_LIVE = 13;
constexpr int LOGO_H_PRE = 24;
constexpr int LOGO_H_POST = 19;
constexpr int NFL_LOGO_H = 12;
constexpr int LOGO_H_BLEED = 30;
constexpr int FIELD_STRIP_H = 8;

constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_DIM = 0x6B6D;    // rgb565(110, 110, 110)
constexpr uint16_t COLOR_OUTLINE = 0x0000;

struct LogoResolver {
    void* ctx;
    LogoArt (*lookup)(void* ctx, const char* league, const char* abbr, int h);
};

inline void clear_card(Canvas16& c) {
    fill_rect(c, 0, 0, c.w, c.h, 0);
}

inline int abbr3(const char* src, char* dst) {
    int n = 0;
    for (; n < 3 && src[n] != '\0'; ++n) dst[n] = src[n];
    dst[n] = '\0';
    return n;
}

inline uint16_t team_colour(const data::Team& t) {
    return rgb565(static_cast<uint8_t>((t.colour >> 16) & 0xFF),
                  static_cast<uint8_t>((t.colour >> 8) & 0xFF),
                  static_cast<uint8_t>(t.colour & 0xFF));
}

inline int score_str(int16_t score, char* dst, size_t cap) {
    if (score == data::kNoInt) return std::snprintf(dst, cap, "-");
    return std::snprintf(dst, cap, "%d", static_cast<int>(score));
}

inline int paste_logo(Canvas16& c, int x, int y, int logo_h, const data::Team& team,
                      const char* league, const LogoResolver& logos, bool right_align) {
    LogoArt art{nullptr, nullptr, 0, 0};
    if (logos.lookup != nullptr && team.abbr[0] != '\0')
        art = logos.lookup(logos.ctx, league, team.abbr, logo_h);

    if (art.px != nullptr && art.mask != nullptr && art.w > 0 && art.h > 0) {
        const int ox = right_align ? x - art.w : x;
        const int oy = y + (logo_h > art.h ? (logo_h - art.h) / 2 : 0);
        blit_logo(c, ox, oy, art);
        return art.w;
    }

    char abbr[4];
    const int len = abbr3(team.abbr, abbr);
    const int width = text_width(FONT_SPLEEN_5X8, len) + 1;  // Python adds 1 for the fallback box.
    const int ox = right_align ? x - width : x;
    const int oy = y + (logo_h > 8 ? (logo_h - 8) / 2 : 0);
    draw_text(c, FONT_SPLEEN_5X8, ox, oy, abbr, team_colour(team));
    return width;
}

inline void draw_centered(Canvas16& c, const Font& font, int y, const char* text, uint16_t color) {
    int x = (CARD_W - text_width(font, static_cast<int>(std::strlen(text)))) / 2;
    if (x < 0) x = 0;
    draw_text(c, font, x, y, text, color);
}

inline void start_time_str(const data::Game& game, LocalTimeFn local, void* ctx,
                           char* dst, size_t cap) {
    LocalTime t{};
    if (local == nullptr || !local(ctx, game.start_utc, t)) {
        std::snprintf(dst, cap, "?");
        return;
    }
    const int hour12 = (t.hour % 12) == 0 ? 12 : (t.hour % 12);
    std::snprintf(dst, cap, "%d:%02d", hour12, t.minute);
}

inline void render_game_card_pre(Canvas16& out, const data::Game& game, const char* league,
                                const LogoResolver& logos, LocalTimeFn local, void* local_ctx) {
    clear_card(out);

    paste_logo(out, 0, 1, LOGO_H_PRE, game.away, league, logos, false);
    paste_logo(out, CARD_W, 1, LOGO_H_PRE, game.home, league, logos, true);

    draw_centered(out, FONT_SPLEEN_5X8, 14, "VS", COLOR_WHITE);

    char ts[16];
    start_time_str(game, local, local_ctx, ts, sizeof ts);
    draw_centered(out, FONT_SPLEEN_5X8, 25, ts, COLOR_DIM);
}

void render_game_card_post(Canvas16& out, const data::Game& game, const char* league,
                           const LogoResolver& logos, LocalTimeFn local, void* local_ctx,
                           int64_t now_utc) {
    clear_card(out);

    paste_logo(out, 0, 1, LOGO_H_POST, game.away, league, logos, false);
    paste_logo(out, CARD_W, 1, LOGO_H_POST, game.home, league, logos, true);

    char away_score[8], home_score[8], score[24];
    score_str(game.away_score, away_score, sizeof away_score);
    score_str(game.home_score, home_score, sizeof home_score);
    std::snprintf(score, sizeof score, "%s-%s", away_score, home_score);
    draw_centered(out, FONT_SPLEEN_6X12, 11, score, COLOR_WHITE);

    LocalTime start{}, today{};
    const bool has_start = local != nullptr && local(local_ctx, game.start_utc, start);
    const bool has_now = local != nullptr && local(local_ctx, now_utc, today);
    const bool prev_day = has_start && has_now &&
        (start.year < today.year ||
         (start.year == today.year && (start.month < today.month ||
          (start.month == today.month && start.day < today.day))));

    char status[16];
    if (prev_day) {
        std::snprintf(status, sizeof status, "%d/%d", start.month, start.day);
    } else if (game.period != data::kNoInt && game.period > 9) {
        std::snprintf(status, sizeof status, "F/%d", static_cast<int>(game.period));
    } else {
        std::snprintf(status, sizeof status, "FINAL");
    }
    draw_centered(out, FONT_SPLEEN_5X8, 22, status, COLOR_DIM);
}

void render_game_card_live(Canvas16& out, const data::Game& game, const char* league,
                           const LogoResolver& logos, bool show_situation = true) {
    clear_card(out);

    constexpr int score_y = (LOGO_H_LIVE > 12) ? (LOGO_H_LIVE - 12) / 2 : 0;

    const int away_w = paste_logo(out, 0, 0, LOGO_H_LIVE, game.away, league, logos, false);
    char away_score[8];
    score_str(game.away_score, away_score, sizeof away_score);
    draw_text(out, FONT_SPLEEN_6X12, away_w + 2, score_y, away_score, COLOR_WHITE);

    const int home_w = paste_logo(out, CARD_W, 0, LOGO_H_LIVE, game.home, league, logos, true);
    char home_score[8];
    score_str(game.home_score, home_score, sizeof home_score);
    const int home_sw = text_width(FONT_SPLEEN_6X12, static_cast<int>(std::strlen(home_score)));
    draw_text(out, FONT_SPLEEN_6X12, CARD_W - home_w - 2 - home_sw, score_y, home_score, COLOR_WHITE);
    if (show_situation) draw_indicator(out, game, league);
}

}  // namespace render
}  // namespace nb
