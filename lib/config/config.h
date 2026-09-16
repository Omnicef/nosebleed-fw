// SPDX-License-Identifier: GPL-3.0-only
//
// T-4.1 — config structs. POD mirrors of the Marquee SQLModel tables
// (HardwareSetting, WidgetConfig, LeagueConfig, Favorite) plus a schema
// version, packed into one fixed-size blob that T-4.2 stores in NVS.
//
// Dead Pi fields dropped per AGENTS.md ("Config fields that no longer
// exist"): hardware_mapping, gpio_slowdown, pwm_bits, pwm_dither_bits,
// pixel_mapper_config, led_rgb_sequence. ESP32 panel-timing fields added:
// lsbMsbTransitionBit, clkphase, latch_blanking, i2sspeed, double_buff.
// options_json becomes a typed `league` field — it only ever held
// {"league": ...} (see Marquee/marquee/config/defaults.py).
//
// No Arduino/ESP includes: this header compiles for host tests too.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nb {
namespace config {

constexpr uint32_t kMagic = 0x4E424346;  // "NBCF" little-endian
constexpr uint16_t kSchemaVersion = 1;   // fresh device, no SQLite history

constexpr size_t kLeagueIdLen = 26;   // "womens-college-basketball" + NUL
constexpr size_t kWidgetIdLen = 40;   // "scoreboard_womens-college-basketball" + NUL
constexpr size_t kTzLen = 32;         // "America/Argentina/Buenos_Aires" + NUL

constexpr int kMaxWidgets = 16;       // seeds 10 (splash + clock + 8 scoreboards)
constexpr int kMaxLeagues = 12;       // 8 league slugs today
constexpr int kMaxFavorites = 16;

enum DisplayMode : uint8_t { kDisplayScroll = 0, kDisplayStatic = 1 };

// Marquee LEAGUE_SLUGS (data/models.py) — dict insertion order preserved.
constexpr int kLeagueSlugCount = 8;
constexpr const char* kLeagueSlugs[kLeagueSlugCount] = {
    "nfl", "college-football", "nba", "mens-college-basketball",
    "womens-college-basketball", "mlb", "nhl", "epl",
};

// Panel geometry/wiring + boot + behaviour. Marquee HardwareSetting
// (singleton row) minus Pi fields plus HUB75-DMA timing fields.
struct HardwareSetting {
    uint16_t rows;              // panel scan rows, default 32
    uint16_t cols;              // panel width, default 64
    uint16_t chain_length;      // panels in series
    uint8_t parallel;           // parallel chains (HUB75_I2S_CFG.parallel)
    uint8_t brightness;         // 0-100 %
    uint16_t boot_splash_s;     // boot splash duration, 0 = disabled
    uint8_t preemption_enabled;
    uint16_t preemption_dwell_s;
    char timezone[kTzLen];      // IANA name; "" = UTC (see T-4.6)
    uint8_t display_mode;       // DisplayMode; applies live
    float scroll_speed;         // px/s, scroll mode
    uint8_t card_gap;           // blank px between cards
    uint8_t clock_24h;          // 0 = 12h + AM/PM
    // ESP32 panel timing — map onto HUB75_I2S_CFG at panel bring-up.
    uint8_t lsb_msb_transition_bit;
    uint8_t clkphase;
    uint8_t latch_blanking;
    uint8_t i2sspeed;
    uint8_t double_buff;
    uint8_t reserved;           // keep size stable when fields are added
};

struct WidgetConfig {
    char id[kWidgetIdLen];      // stable slug, e.g. "clock", "scoreboard_mlb"
    char type[16];              // "boot_splash" | "clock" | "scoreboard" | ...
    uint16_t order;             // carousel order (drag-to-reorder)
    uint8_t enabled;
    float dwell_s;              // static-mode dwell
    char league[kLeagueIdLen];  // scoreboard widgets only; "" otherwise
};

struct LeagueConfig {
    char id[kLeagueIdLen];      // league slug
    uint8_t enabled;
    uint16_t poll_interval_live;  // seconds
    uint16_t poll_interval_idle;  // seconds
};

struct Favorite {
    char league[kLeagueIdLen];
    char team_id[12];
    char team_name[40];
    char team_abbr[6];
    uint8_t priority;
};

// Whole config as one NVS blob (T-4.2). kMagic/kSchemaVersion gate the
// corrupted-blob fallback; *_count bound the used rows of each array.
struct Config {
    uint32_t magic;
    uint16_t schema;
    HardwareSetting hw;
    uint16_t widget_count;
    WidgetConfig widgets[kMaxWidgets];
    uint16_t league_count;
    LeagueConfig leagues[kMaxLeagues];
    uint16_t favorite_count;
    Favorite favorites[kMaxFavorites];
};

// The nvs partition is 0x5000 (20 KB) after the T-1.2 overlap fix and NVS
// blobs are size-capped well below that; keep the whole store under 4 KB
// (PLAN T-4.1 accept).
static_assert(sizeof(Config) < 4096, "config blob must stay under 4 KB");

// Bounds a loaded (or supplied) blob may be trusted: magic, schema and the
// three counts in range. Everything else is fixed-size POD. Pure — the
// device store and the host corruption test share it.
bool validate(const Config& c);

// Seed Marquee defaults (config/defaults.py): splash + clock + one
// scoreboard per league (MLB first, rest sorted; only MLB enabled),
// LeagueConfig row per slug (only MLB enabled).
void set_defaults(Config& c);

}  // namespace config
}  // namespace nb
