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

inline bool is_football_league(const char* league) {
    return league != nullptr &&
           (std::strcmp(league, "nfl") == 0 || std::strcmp(league, "college-football") == 0);
}

inline void nfl_ordinal_str(int value, char* dst, size_t cap) {
    switch (value) {
        case 1: std::snprintf(dst, cap, "1ST"); return;
        case 2: std::snprintf(dst, cap, "2ND"); return;
        case 3: std::snprintf(dst, cap, "3RD"); return;
        case 4: std::snprintf(dst, cap, "4TH"); return;
        default: std::snprintf(dst, cap, "%dTH", value); return;
    }
}

inline void nfl_quarter_str(const data::Game& game, char* dst, size_t cap) {
    if (game.period == data::kNoInt) {
        dst[0] = '\0';
        return;
    }
    const int period = static_cast<int>(game.period);
    if (period <= 4) {
        nfl_ordinal_str(period, dst, cap);
    } else if (period == 5) {
        std::snprintf(dst, cap, "OT");
    } else {
        std::snprintf(dst, cap, "%dOT", period - 4);
    }
}

inline void nfl_down_distance_str(const data::Situation& sit, char* dst, size_t cap) {
    if (sit.down == data::kNoInt) {
        dst[0] = '\0';
        return;
    }
    char down[8];
    nfl_ordinal_str(static_cast<int>(sit.down), down, sizeof down);
    if (sit.distance == data::kNoInt) {
        std::snprintf(dst, cap, "%s", down);
    } else {
        std::snprintf(dst, cap, "%s&%d", down, static_cast<int>(sit.distance));
    }
}

inline void draw_possession_football(Canvas16& c, int x, int y) {
    // Pillow draw.ellipse([x, y, x + 5, y + 3], ...) for this exact 6x4 box.
    hline(c, x + 1, x + 4, y, COLOR_FOOTBALL_BROWN);
    fill_rect(c, x, y + 1, 6, 2, COLOR_FOOTBALL_BROWN);
    hline(c, x + 1, x + 4, y + 3, COLOR_FOOTBALL_BROWN);
    line(c, x + 2, y + 1, x + 2, y + 2, COLOR_WHITE);
}

inline bool is_periodclock_league(const char* league) {
    if (league == nullptr) return false;
    return std::strcmp(league, "nhl") == 0 || std::strcmp(league, "nba") == 0 ||
           std::strcmp(league, "mens-college-basketball") == 0 ||
           std::strcmp(league, "womens-college-basketball") == 0 ||
           std::strcmp(league, "epl") == 0 || std::strcmp(league, "eng.1") == 0 ||
           std::strcmp(league, "usa.1") == 0 || std::strcmp(league, "esp.1") == 0 ||
           std::strcmp(league, "ger.1") == 0 || std::strcmp(league, "ita.1") == 0 ||
           std::strcmp(league, "fra.1") == 0 || std::strcmp(league, "mex.1") == 0;
}

inline bool is_nba_periodclock_league(const char* league) {
    return league != nullptr &&
           (std::strcmp(league, "nba") == 0 || std::strcmp(league, "mens-college-basketball") == 0 ||
            std::strcmp(league, "womens-college-basketball") == 0);
}

inline char ascii_lower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

inline bool contains_ci(const char* s, const char* needle_lower) {
    if (s == nullptr || needle_lower == nullptr) return false;
    for (; *s != '\0'; ++s) {
        const char* h = s;
        const char* n = needle_lower;
        while (*h != '\0' && *n != '\0' && ascii_lower(*h) == *n) {
            ++h;
            ++n;
        }
        if (*n == '\0') return true;
    }
    return false;
}

inline void ascii_strip(const char* src, char* dst, size_t cap) {
    if (src == nullptr || dst == nullptr || cap == 0) {
        if (dst != nullptr && cap > 0) dst[0] = '\0';
        return;
    }
    while (*src != '\0' && (*src == ' ' || (*src >= '\t' && *src <= '\r'))) ++src;
    size_t len = std::strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || (src[len - 1] >= '\t' && src[len - 1] <= '\r'))) --len;
    const size_t copy = len < cap - 1 ? len : cap - 1;
    std::memcpy(dst, src, copy);
    dst[copy] = '\0';
}

inline void nhl_period_str(int16_t period, char* dst, size_t cap) {
    if (period == data::kNoInt) {
        dst[0] = '\0';
        return;
    }
    switch (period) {
        case 1: std::snprintf(dst, cap, "P1"); return;
        case 2: std::snprintf(dst, cap, "P2"); return;
        case 3: std::snprintf(dst, cap, "P3"); return;
        case 4: std::snprintf(dst, cap, "OT"); return;
        case 5: std::snprintf(dst, cap, "SO"); return;
        default: std::snprintf(dst, cap, "OT%d", static_cast<int>(period) - 3); return;
    }
}

inline void nba_period_str(int16_t period, char* dst, size_t cap) {
    if (period == data::kNoInt) {
        dst[0] = '\0';
        return;
    }
    const int p = static_cast<int>(period);
    if (p <= 4) {
        std::snprintf(dst, cap, "Q%d", p);
    } else if (p == 5) {
        std::snprintf(dst, cap, "OT");
    } else {
        std::snprintf(dst, cap, "%dOT", p - 4);
    }
}

inline void soccer_period_str(int16_t period, const char* status_display, char* dst, size_t cap) {
    if (contains_ci(status_display, "halftime") || contains_ci(status_display, "half time")) {
        std::snprintf(dst, cap, "HT");
    } else if (period == 1) {
        std::snprintf(dst, cap, "1ST");
    } else if (period == 2) {
        std::snprintf(dst, cap, "2ND");
    } else if (period != data::kNoInt) {
        std::snprintf(dst, cap, "P%d", static_cast<int>(period));
    } else {
        dst[0] = '\0';
    }
}

inline void soccer_clock_str(const data::Game& game, char* dst, size_t cap) {
    char clock[16];
    ascii_strip(game.clock, clock, sizeof clock);
    if (clock[0] != '\0') {
        const size_t len = std::strlen(clock);
        if (clock[len - 1] == '\'') {
            std::snprintf(dst, cap, "%s", clock);
        } else {
            std::snprintf(dst, cap, "%s'", clock);
        }
        return;
    }

    char digits[4] = {};
    int n = 0;
    for (const char* p = game.status_display; *p != '\0' && n < 3; ++p) {
        if (*p >= '0' && *p <= '9') digits[n++] = *p;
    }
    if (n == 0) {
        dst[0] = '\0';
    } else {
        std::snprintf(dst, cap, "%s'", digits);
    }
}

inline void format_period_clock(const data::Game& game, const char* league, char* period,
                                size_t period_cap, char* clock, size_t clock_cap) {
    period[0] = '\0';
    clock[0] = '\0';
    if (league != nullptr && std::strcmp(league, "nhl") == 0) {
        nhl_period_str(game.period, period, period_cap);
        std::snprintf(clock, clock_cap, "%s", game.clock);
        return;
    }
    if (is_nba_periodclock_league(league)) {
        nba_period_str(game.period, period, period_cap);
        std::snprintf(clock, clock_cap, "%s", game.clock);
        return;
    }
    soccer_period_str(game.period, game.status_display, period, period_cap);
    soccer_clock_str(game, clock, clock_cap);
}

inline void paste_logo_bleed(Canvas16& out, bool left_side, int logo_h, const data::Team& team,
                             const char* league, const LogoResolver& logos) {
    const int logo_y = (static_cast<int>(out.h) - logo_h) / 2;

    LogoArt art{nullptr, nullptr, 0, 0};
    if (logos.lookup != nullptr && team.abbr[0] != '\0') {
        art = logos.lookup(logos.ctx, league, team.abbr, logo_h);
    }

    if (art.px != nullptr && art.mask != nullptr && art.w > 0 && art.h > 0) {
        const int paste_x =
            left_side ? -(art.w / 4) : CARD_W - art.w + (art.w / 4);
        blit_logo(out, paste_x, logo_y, art);
        return;
    }

    char abbr[4];
    const int len = abbr3(team.abbr, abbr);
    const int text_y = logo_y + (logo_h > 8 ? (logo_h - 8) / 2 : 0);
    const int tw = text_width(FONT_SPLEEN_5X8, len);
    const int x = left_side ? 1 : CARD_W - tw - 1;
    draw_text(out, FONT_SPLEEN_5X8, x, text_y, abbr, team_colour(team));
}

void render_game_card_live_periodclock(Canvas16& out, const data::Game& game, const char* league,
                                       const LogoResolver& logos) {
    clear_card(out);

    paste_logo_bleed(out, true, LOGO_H_BLEED, game.away, league, logos);
    paste_logo_bleed(out, false, LOGO_H_BLEED, game.home, league, logos);

    char away_score[8], home_score[8], score[24];
    score_str(game.away_score, away_score, sizeof away_score);
    score_str(game.home_score, home_score, sizeof home_score);
    std::snprintf(score, sizeof score, "%s-%s", away_score, home_score);

    const int score_y = (static_cast<int>(out.h) / 4) - 4;
    const int score_sw = text_width(FONT_SPLEEN_6X12, static_cast<int>(std::strlen(score)));
    const int score_x = (CARD_W - score_sw) / 2;
    draw_text_outlined(out, FONT_SPLEEN_6X12, score_x < 0 ? 0 : score_x,
                       score_y > 0 ? score_y : 0, score, COLOR_WHITE, COLOR_OUTLINE);

    char period_str[16], clock_str[16];
    format_period_clock(game, league, period_str, sizeof period_str, clock_str, sizeof clock_str);

    if (period_str[0] != '\0') {
        const int pw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(period_str)));
        const int period_x = (CARD_W - pw) / 2;
        const int period_y = static_cast<int>(out.h) * 18 / 32;
        draw_text_outlined(out, FONT_SPLEEN_5X8, period_x < 0 ? 0 : period_x, period_y,
                           period_str, COLOR_DIM, COLOR_OUTLINE);
    }

    if (clock_str[0] != '\0') {
        const int cw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(clock_str)));
        const int clock_x = (CARD_W - cw) / 2;
        const int clock_y = static_cast<int>(out.h) * 24 / 32;
        draw_text_outlined(out, FONT_SPLEEN_5X8, clock_x < 0 ? 0 : clock_x, clock_y,
                           clock_str, COLOR_WHITE, COLOR_OUTLINE);
    }
}

void render_game_card_live_nfl(Canvas16& out, const data::Game& game, const char* league,
                               const LogoResolver& logos, bool show_situation = true) {
    clear_card(out);

    const bool has_sit = show_situation && game.has_situation != 0;
    const data::Situation& sit = game.situation;

    const int score_y_away = NFL_LOGO_H > 8 ? (NFL_LOGO_H - 8) / 2 : 0;

    const int away_lw = paste_logo(out, 0, 0, NFL_LOGO_H, game.away, league, logos, false);
    char away_score[8];
    score_str(game.away_score, away_score, sizeof away_score);
    draw_text(out, FONT_SPLEEN_5X8, away_lw + 2, score_y_away, away_score, COLOR_WHITE);
    const int away_sw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(away_score)));
    if (has_sit && std::strcmp(sit.possession, game.away.id) == 0) {
        draw_possession_football(out, away_lw + 2 + away_sw + 1, score_y_away);
    }

    const int score_y_home = NFL_LOGO_H + score_y_away;
    const int home_lw = paste_logo(out, 0, NFL_LOGO_H, NFL_LOGO_H, game.home, league, logos, false);
    char home_score[8];
    score_str(game.home_score, home_score, sizeof home_score);
    draw_text(out, FONT_SPLEEN_5X8, home_lw + 2, score_y_home, home_score, COLOR_WHITE);
    const int home_sw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(home_score)));
    if (has_sit && std::strcmp(sit.possession, game.home.id) == 0) {
        draw_possession_football(out, home_lw + 2 + home_sw + 1, score_y_home);
    }

    const int rx = CARD_W - 1;
    int ry = 0;

    char quarter[8];
    nfl_quarter_str(game, quarter, sizeof quarter);
    if (quarter[0] != '\0') {
        const int qw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(quarter)));
        draw_text(out, FONT_SPLEEN_5X8, rx - qw, ry, quarter, COLOR_DIM);
    }
    ry += 8;

    if (game.clock[0] != '\0') {
        const int cw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(game.clock)));
        draw_text(out, FONT_SPLEEN_5X8, rx - cw, ry, game.clock, COLOR_WHITE);
    }
    ry += 8;

    if (has_sit) {
        char down_distance[24];
        nfl_down_distance_str(sit, down_distance, sizeof down_distance);
        if (down_distance[0] != '\0') {
            const int dw = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(down_distance)));
            draw_text(out, FONT_SPLEEN_5X8, rx - dw, ry, down_distance, COLOR_WHITE);
        }

        const int field_y = static_cast<int>(out.h) - FIELD_STRIP_H;
        if (field_y >= 0) draw_gridiron(out, 0, field_y, CARD_W, FIELD_STRIP_H, sit);
    }
}

void render_game_card_live(Canvas16& out, const data::Game& game, const char* league,
                           const LogoResolver& logos, bool show_situation = true) {
    if (is_football_league(league)) {
        render_game_card_live_nfl(out, game, league, logos, show_situation);
        return;
    }
    if (is_periodclock_league(league)) {
        render_game_card_live_periodclock(out, game, league, logos);
        return;
    }

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
