// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.1 — data structs. POD mirrors of Marquee's dataclasses
// (marquee/data/models.py) with fixed char[] and sentinel-optionals, so the
// whole Game[] set is statically sized and copyable with one memcpy.
//
// Python Optional[int] -> int16_t with kNoInt (INT16_MIN) sentinel.
// Python's per-Game `league` and Team `logo_url` are dropped on purpose:
// games live in a per-league cache slot (the slot IS the league tag), and
// logos come from the build-time atlas (league+abbr key), never a URL.
// No Arduino/ESP includes — host-compilable like lib/render.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nb {
namespace data {

constexpr int16_t kNoInt = INT16_MIN;  // Python None for optional ints

constexpr int kMaxGamesPerLeague = 16;  // single-day ESPN slates fit well under this

// status.type.state. ESPN emits only these three; the parser maps any
// other/missing value to kPre (Marquee norm_game's default).
enum Status : uint8_t { kStatusPre = 0, kStatusIn = 1, kStatusPost = 2 };

struct Team {
    char id[16];        // ESPN numeric id as string
    char name[32];      // displayName
    char abbr[6];       // abbreviation
    uint32_t colour;    // 0xRRGGBB from ESPN's 6-hex `color`; default white
};

// Marquee Situation. Absent ints are kNoInt; absent possession is "".
struct Situation {
    uint8_t on_first, on_second, on_third, is_red_zone;
    int16_t balls, strikes, outs;
    int16_t down, distance, yard_line;
    char possession[16];  // team id of possessing team, "" = absent
};

struct Game {
    char id[16];
    uint8_t status;              // Status
    char status_display[24];     // shortDetail ("Top 3rd", "Final", "7:05 PM ET")
    int16_t period;              // kNoInt when absent
    char clock[8];               // displayClock ("0:42", "67:00"); "" absent
    Team away, home;
    int16_t away_score, home_score;  // kNoInt pre-game / absent
    int64_t start_utc;               // epoch seconds UTC (Python start_time)
    Situation situation;
    uint8_t has_situation;           // Python `situation is None`
};

// One league's complete snapshot. The cache publishes whole GameLists by
// pointer swap (T-5.2) — a fetched list must never be mutated in place.
struct GameList {
    int count;
    int64_t fetched_utc;  // publish time (epoch s); 0 = never
    Game games[kMaxGamesPerLeague];
};

// Bounded, truncating copy into fixed char fields (never leaves unterminated).
inline void copy_str(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

}  // namespace data
}  // namespace nb
