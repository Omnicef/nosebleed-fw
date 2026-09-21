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
constexpr uint16_t kSchemaVersion = 3;   // v3: ticker widget rows (Phase 10)

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

// ESPN scoreboard API path per league slug (Marquee data/models.py
// LEAGUE_SLUGS dict), same order as kLeagueSlugs.
constexpr const char* kLeagueApiPaths[kLeagueSlugCount] = {
    "football/nfl", "football/college-football", "basketball/nba",
    "basketball/mens-college-basketball", "basketball/womens-college-basketball",
    "baseball/mlb", "hockey/nhl", "soccer/eng.1",
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
    char type[16];              // "boot_splash" | "clock" | "scoreboard" | "weather" | ...
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

// Phase 10 — non-secret parameters for the info feeds (weather + ticker)
// and the display schedule. Deliberately NOT in HardwareSetting: none is
// panel-structural, all are live-applicable. Secret ticker keys are NOT
// here — they live in the ApiKeys blob beside Creds (never GET, never
// logged). Whole struct copied alongside the rest of Config.
struct Services {
    // Weather — Open-Meteo, keyless. lat/lon plain decimals (weather_url
    // validates before use); empty lat = weather disabled.
    char lat[12];
    char lon[12];
    uint8_t imperial;   // 0 = Celsius, 1 = Fahrenheit
    // Ticker sources — public query params only (GNews category/country,
    // comma-separated Finnhub symbols, CoinGecko ids). Keys are separate.
    char news_category[16];
    char news_country[3];
    char stock_symbols[48];
    char crypto_ids[48];
    // T-10.5 — quiet hours on the local wall clock (config timezone).
    // Window is [quiet_start, quiet_end); wraps when quiet_end <= start.
    uint8_t quiet_enabled;
    uint16_t quiet_start;  // minutes past local midnight
    uint16_t quiet_end;    // minutes past local midnight
    uint8_t quiet_brightness;  // brightness during quiet hours (0 = blank)
};

// Whole config as one NVS blob (T-4.2). kMagic/kSchemaVersion gate the
// corrupted-blob fallback; *_count bound the used rows of each array.
struct Config {
    uint32_t magic;
    uint16_t schema;
    HardwareSetting hw;
    Services svc;
    uint16_t widget_count;
    WidgetConfig widgets[kMaxWidgets];
    uint16_t league_count;
    LeagueConfig leagues[kMaxLeagues];
    uint16_t favorite_count;
    Favorite favorites[kMaxFavorites];
};

// T-9.2 — WiFi credentials. A SEPARATE NVS blob ("net"), never part of
// Config: nothing that serialises or GETs the config can return a
// password by construction. Fixed-size POD, IEEE 802.11 lengths
// (SSID 1–32, WPA passphrase 0 or 8–63). Device-only access; host code
// may validate but never logs these.
struct Creds {
    char ssid[33];
    char pass[65];
};

// 802.11 length bounds + no control characters. Pure — shared by the
// provisioning handler and any test. The reason the character guard is
// explicit: a form POST percent-decodes, so "%0A" can arrive as a real
// newline and would desync anything that prints the value.
bool creds_valid(const Creds& c);

// T-10.3 — ticker API keys. A SEPARATE NVS blob ("keys") with the exact
// Creds discipline: never part of Config, never serialised by any GET,
// never logged. The allowlist matters beyond tidiness — these strings are
// concatenated into https query strings, so anything outside
// [A-Za-z0-9._-] could re-shape the request (spaces, '&', '#', CR/LF).
// Empty string = unset (valid); fetch code treats unset as "source off".
struct ApiKeys {
    char gnews[64];
    char finnhub[64];
};

bool api_keys_valid(const ApiKeys& k);

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

// T-4.4 — restart-banner condition: true iff a panel-geometry / DMA-timing
// field CHANGED between the stored and the proposed setting. Marquee's
// STRUCTURAL_FIELDS ported to ESP32: Pi fields (hardware_mapping,
// gpio_slowdown, pwm_* , pixel_mapper_config) are gone; our lsbMsbTransitionBit/
// clkphase/latch_blanking/i2sspeed/double_buff are their equivalents.
// Compares values, never "was the key in the payload" — that Marquee fix
// is preserved by this being the only API (two structs in, one bool out).
bool hw_structural_changed(const HardwareSetting& old, const HardwareSetting& now);

}  // namespace config
}  // namespace nb
