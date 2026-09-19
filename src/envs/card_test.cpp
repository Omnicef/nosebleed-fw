// SPDX-License-Identifier: GPL-3.0-only
//
// env:card — T-6.x/T-7.x all card states on the panel. Moved verbatim out of main.cpp
// (Split test envs). Selected by build_src_filter, not by this #ifdef, but
// the guard is kept so the moved text is byte-identical and a stray compile
// of this file under another env stays inert.

#include "common.h"

#ifdef NB_CARD_TEST
// Hardware card seam proof: real data -> existing lib/render cards ->
// mmap'd flash logos -> panel::blit. No phase-7 strip/scroll yet; this only
// cycles the four card states for eyeball/serial confirmation.
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "cache.h"
#include "cardtest_fixture.h"
#include "date_window.h"
#include "espn.h"
#include "espn_json.h"
#include "game_card.h"
#include "logos_esp.h"
#include "panel.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

using nb::config::kLeagueApiPaths;
using nb::config::kLeagueSlugCount;
using nb::config::kLeagueSlugs;
using nb::data::copy_str;
using nb::data::DataCache;
using nb::data::Game;
using nb::data::GameList;
using nb::data::kNoInt;
using nb::data::kStatusIn;
using nb::data::kStatusPost;
using nb::data::kStatusPre;

struct Card {
    Game g;
    const char* league = "";
    const char* label = "";
    const char* src = "missing";
    bool show_situation = false;
};

static GameList* g_card_snaps = nullptr;
static GameList* g_card_fixture = nullptr;

static nb::LogoArt cardtest_logo(void*, const char* league, const char* abbr, int h) {
    nb::logos::Ref r;
    if (league == nullptr || abbr == nullptr || !nb::logos::lookup(league, abbr, static_cast<uint16_t>(h), r))
        return nb::LogoArt{nullptr, nullptr, 0, 0};
    return nb::LogoArt{reinterpret_cast<const uint16_t*>(r.px), r.mask, static_cast<int>(r.w),
                       static_cast<int>(r.h)};
}

static const nb::render::LogoResolver kCardResolver = {nullptr, cardtest_logo};

static bool cardtest_wifi_time() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("cardtest wifi: connecting");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        Serial.print('.');
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(" NO WIFI");
        return false;
    }
    Serial.printf(" %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    const time_t t0 = time(nullptr);
    uint32_t waited = 0;
    while (time(nullptr) < 1700000000 && waited < 20000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    const bool ok = time(nullptr) >= 1700000000;
    Serial.printf("cardtest sntp: %s (%lu ms)\n", ok ? "synced" : "FAILED",
                  static_cast<unsigned long>(time(nullptr) >= 1700000000 ? waited : time(nullptr) - t0));
    return ok;
}

static int cardtest_poll(DataCache* cache) {
    static const int kEnabled[] = {0, 2, 5, 6, 7};  // nfl nba mlb nhl epl
    int total = 0;
    for (int lg : kEnabled) {
        char dates[16], url[192];
        const time_t now = time(nullptr);
        size_t wire = 0;
        if (!nb::data::local_day(dates, sizeof dates, now) ||
            !nb::data::scoreboard_url(url, sizeof url, kLeagueApiPaths[lg], dates)) {
            Serial.printf("cardtest poll[%s]: URL FAIL\n", kLeagueSlugs[lg]);
            continue;
        }
        GameList* w = cache->writable(lg);
        JsonDocument doc = nb::data::make_psram_doc();
        if (!nb::data::espn_fetch_scoreboard(url, doc, &wire)) {
            Serial.printf("cardtest poll[%s]: FETCH FAIL\n", kLeagueSlugs[lg]);
            continue;
        }
        w->count = nb::data::to_games(doc, *w);
        nb::data::filter_yesterday_today(w, now);
        if (w->count == 0) {
            char yd[16];
            if (nb::data::local_yesterday(yd, sizeof yd, now) &&
                nb::data::scoreboard_url(url, sizeof url, kLeagueApiPaths[lg], yd)) {
                JsonDocument doc2 = nb::data::make_psram_doc();
                if (nb::data::espn_fetch_scoreboard(url, doc2, &wire))
                    w->count = nb::data::to_games(doc2, *w);
            }
        }
        cache->publish(lg, static_cast<int64_t>(now));
        if (cache->snapshot(lg, &g_card_snaps[lg])) total += g_card_snaps[lg].count;
        Serial.printf("cardtest poll[%s]: games=%d wire=%u B\n", kLeagueSlugs[lg], g_card_snaps[lg].count,
                      static_cast<unsigned>(wire));
    }
    return total;
}

static bool cardtest_load_fixture(const char* why) {
    if (g_card_fixture == nullptr) {
        g_card_fixture = static_cast<GameList*>(heap_caps_calloc(1, sizeof(GameList), MALLOC_CAP_SPIRAM));
        if (g_card_fixture == nullptr) {
            Serial.println("cardtest fixture: FAIL no PSRAM for GameList");
            return false;
        }
    }
    JsonDocument filter = nb::data::make_psram_doc();
    nb::data::build_scoreboard_filter(filter);
    JsonDocument doc = nb::data::make_psram_doc();
    const char* text = kCardTestFixture;
    const DeserializationError err = nb::data::parse_scoreboard(text, filter, doc);
    if (err) {
        Serial.printf("cardtest fixture: parse FAIL (%s) — %s\n", err.c_str(), why);
        return false;
    }
    g_card_fixture->count = nb::data::to_games(doc, *g_card_fixture);
    Serial.printf("cardtest fixture: loaded %d games (%s)\n", g_card_fixture->count, why);
    return g_card_fixture->count > 0;
}

static void clear_situation(Game& g) {
    g.has_situation = 0;
    std::memset(&g.situation, 0, sizeof g.situation);
}

static void fake_mlb_situation(Game& g) {
    g.status = kStatusIn;
    g.has_situation = 1;
    std::memset(&g.situation, 0, sizeof g.situation);
    g.situation.on_first = 1;
    g.situation.on_second = 0;
    g.situation.on_third = 1;
    g.situation.balls = 2;
    g.situation.strikes = 1;
    g.situation.outs = 1;
    g.situation.down = kNoInt;
    g.situation.distance = kNoInt;
    g.situation.yard_line = kNoInt;
    g.situation.possession[0] = '\0';
    if (g.period == kNoInt) g.period = 5;
    copy_str(g.status_display, sizeof g.status_display, "Top 5th");
}

static void derive_pre(Game& g, int64_t now) {
    g.status = kStatusPre;
    g.away_score = g.home_score = kNoInt;
    g.start_utc = now + 3600;
    g.period = kNoInt;
    g.clock[0] = '\0';
    copy_str(g.status_display, sizeof g.status_display, "");
    clear_situation(g);
}

static void derive_post(Game& g, int64_t now) {
    g.status = kStatusPost;
    g.start_utc = now;
    if (g.away_score == kNoInt) g.away_score = 0;
    if (g.home_score == kNoInt) g.home_score = 0;
    if (g.period == kNoInt) g.period = 9;
    g.clock[0] = '\0';
    copy_str(g.status_display, sizeof g.status_display, "Final");
    clear_situation(g);
}

static void derive_generic(Game& g) {
    g.status = kStatusIn;
    if (g.away_score == kNoInt) g.away_score = 0;
    if (g.home_score == kNoInt) g.home_score = 0;
    if (g.period == kNoInt) g.period = 5;
    copy_str(g.clock, sizeof g.clock, "0:00");
    copy_str(g.status_display, sizeof g.status_display, "Top 5th");
    clear_situation(g);
}

static void score_text(int16_t score, char* out, size_t cap) {
    if (score == kNoInt) {
        copy_str(out, cap, "-");
        return;
    }
    std::snprintf(out, cap, "%d", static_cast<int>(score));
}

static void assign_card(Card& c, const Game& g, const char* league, const char* src, bool situation = false) {
    c.g = g;
    c.league = league ? league : "";
    c.src = src;
    c.show_situation = situation;
}

static constexpr int kCardCount = 11;

static const Game* find_live(const char* league) {
    if (g_card_snaps == nullptr) return nullptr;
    for (int lg = 0; lg < DataCache::kLeagues; ++lg) {
        if (std::strcmp(kLeagueSlugs[lg], league) != 0) continue;
        const GameList& list = g_card_snaps[lg];
        for (int i = 0; i < list.count; ++i) {
            if (list.games[i].status == kStatusIn) return &list.games[i];
        }
        break;
    }
    return nullptr;
}

static void assign_generic_card(Card& c, const Game& g, const char* league, const char* src) {
    (void)league;
    assign_card(c, g, "mlb", src);
    copy_str(c.g.away.abbr, sizeof c.g.away.abbr, "LAL");
    copy_str(c.g.home.abbr, sizeof c.g.home.abbr, "BOS");
    c.g.away.colour = 0x552583;
    c.g.home.colour = 0x007a33;
    derive_generic(c.g);
}

static void set_nfl_no_situation(Game& g) {
    g.status = kStatusIn;
    g.has_situation = 0;
    std::memset(&g.situation, 0, sizeof g.situation);
    if (g.period == kNoInt) g.period = 2;
    copy_str(g.clock, sizeof g.clock, "12:34");
    copy_str(g.status_display, sizeof g.status_display, "Q2");
}

static void set_nfl_possession(Game& g, bool home, bool redzone) {
    g.status = kStatusIn;
    g.has_situation = 1;
    std::memset(&g.situation, 0, sizeof g.situation);
    g.situation.balls = g.situation.strikes = g.situation.outs = kNoInt;
    g.situation.down = redzone ? 2 : 1;
    g.situation.distance = redzone ? 7 : 10;
    g.situation.yard_line = redzone ? 85 : 25;
    g.situation.is_red_zone = redzone ? 1 : 0;
    copy_str(g.situation.possession, sizeof g.situation.possession, home ? g.home.id : g.away.id);
    if (g.period == kNoInt) g.period = 2;
    copy_str(g.clock, sizeof g.clock, "12:34");
    copy_str(g.status_display, sizeof g.status_display, "Q2");
}

static void fake_nfl_game(Game& g, const Game& base, int64_t now) {
    g = base;
    copy_str(g.id, sizeof g.id, "cardtest-nfl");
    g.status = kStatusIn;
    copy_str(g.away.id, sizeof g.away.id, "12");
    copy_str(g.home.id, sizeof g.home.id, "34");
    copy_str(g.away.abbr, sizeof g.away.abbr, "KC");
    copy_str(g.home.abbr, sizeof g.home.abbr, "LAR");
    g.away.colour = 0xe4393c;
    g.home.colour = 0x003594;
    g.away_score = 7;
    g.home_score = 10;
    g.start_utc = now;
    g.period = 2;
    copy_str(g.clock, sizeof g.clock, "12:34");
    copy_str(g.status_display, sizeof g.status_display, "Q2");
    clear_situation(g);
}

static void fake_periodclock_game(Game& g, const Game& base, int64_t now, const char* away, const char* home,
                                   uint32_t away_colour, uint32_t home_colour, int16_t away_score,
                                   int16_t home_score, int16_t period, const char* clock,
                                   const char* status_display) {
    g = base;
    copy_str(g.id, sizeof g.id, "cardtest-pc");
    g.status = kStatusIn;
    copy_str(g.away.id, sizeof g.away.id, away);
    copy_str(g.home.id, sizeof g.home.id, home);
    copy_str(g.away.abbr, sizeof g.away.abbr, away);
    copy_str(g.home.abbr, sizeof g.home.abbr, home);
    g.away.colour = away_colour;
    g.home.colour = home_colour;
    g.away_score = away_score;
    g.home_score = home_score;
    g.start_utc = now;
    g.period = period;
    copy_str(g.clock, sizeof g.clock, clock);
    copy_str(g.status_display, sizeof g.status_display, status_display);
    clear_situation(g);
}

static void fill_periodclock_cards(Card cards[kCardCount], const Game* first, int64_t now) {
    Game base{};
    if (first != nullptr) {
        base = *first;
    } else if (g_card_fixture != nullptr && g_card_fixture->count > 0) {
        base = g_card_fixture->games[0];
    } else {
        return;
    }

    const Game* live = find_live("nhl");
    if (live != nullptr) {
        assign_card(cards[8], *live, "nhl", "cache");
    } else {
        assign_card(cards[8], base, "nhl", "derived");
        fake_periodclock_game(cards[8].g, base, now, "LAL", "VGK", 0xe4393c, 0x22a7de, 3, 2, 3, "14:22", "3rd");
    }
    cards[8].show_situation = false;

    live = find_live("nba");
    if (live != nullptr) {
        assign_card(cards[9], *live, "nba", "cache");
    } else {
        assign_card(cards[9], base, "nba", "derived");
        fake_periodclock_game(cards[9].g, base, now, "BOS", "LAL", 0x22a7de, 0xe4393c, 101, 98, 5, "2:30", "OT");
    }
    cards[9].show_situation = false;

    live = find_live("epl");
    if (live != nullptr) {
        assign_card(cards[10], *live, "epl", "cache");
    } else {
        assign_card(cards[10], base, "epl", "derived");
        fake_periodclock_game(cards[10].g, base, now, "ARS", "CHE", 0xe4393c, 0x22a7de, 1, 0, 2, "67'", "67'");
    }
    cards[10].show_situation = false;
}

static void fill_nfl_gridiron_cards(Card cards[kCardCount], const Game* first, int64_t now) {
    Game base{};
    const char* src = nullptr;
    const Game* nfl_live = find_live("nfl");

    if (nfl_live != nullptr) {
        base = *nfl_live;
        src = "cache-derived";
    } else if (g_card_fixture != nullptr && g_card_fixture->count > 0) {
        fake_nfl_game(base, g_card_fixture->games[0], now);
        src = "fixture-derived";
    } else if (first != nullptr) {
        fake_nfl_game(base, *first, now);
        src = "cache-derived";
    } else {
        return;
    }

    assign_card(cards[4], base, "nfl", src);
    set_nfl_possession(cards[4].g, false, false);
    cards[4].show_situation = true;

    assign_card(cards[5], base, "nfl", src);
    set_nfl_possession(cards[5].g, true, false);
    cards[5].show_situation = true;

    assign_card(cards[6], base, "nfl", src);
    set_nfl_no_situation(cards[6].g);
    cards[6].show_situation = false;

    assign_card(cards[7], base, "nfl", src);
    set_nfl_possession(cards[7].g, false, true);
    cards[7].show_situation = true;
}

static void build_cards(Card cards[kCardCount]) {
    static const char* kLabels[kCardCount] = {
        "PRE", "FINAL", "LIVE generic (MLB no-sit)", "MLB live diamond",
        "NFL gridiron away", "NFL gridiron home", "NFL gridiron no-situation",
        "NFL gridiron redzone", "NHL period-clock", "NBA period-clock",
        "Soccer period-clock",
    };
    for (int i = 0; i < kCardCount; ++i) cards[i].label = kLabels[i];

    const Game* first = nullptr;
    const char* first_league = nullptr;
    bool got[4] = {false, false, false, false};

    if (g_card_snaps != nullptr) {
        for (int lg = 0; lg < DataCache::kLeagues; ++lg) {
            const GameList& list = g_card_snaps[lg];
            const char* league = kLeagueSlugs[lg];
            if (list.count > 0 && first == nullptr) {
                first = &list.games[0];
                first_league = league;
            }
            for (int i = 0; i < list.count; ++i) {
                const Game& g = list.games[i];
                if (!got[0] && g.status == kStatusPre) {
                    assign_card(cards[0], g, league, "cache");
                    got[0] = true;
                }
                if (!got[1] && g.status == kStatusPost) {
                    assign_card(cards[1], g, league, "cache");
                    got[1] = true;
                }
                if (!got[2] && g.status == kStatusIn && std::strcmp(league, "mlb") == 0 && !g.has_situation) {
                    assign_card(cards[2], g, league, "cache");
                    got[2] = true;
                }
                if (!got[3] && std::strcmp(league, "mlb") == 0 && g.status == kStatusIn && g.has_situation) {
                    assign_card(cards[3], g, league, "cache", true);
                    got[3] = true;
                }
            }
        }
    }

    const int64_t raw_now = static_cast<int64_t>(time(nullptr));
    const int64_t now = raw_now > 1000000000 ? raw_now : 1777000000;

    if (first != nullptr) {
        if (!got[0]) {
            assign_card(cards[0], *first, first_league, "cache-derived");
            derive_pre(cards[0].g, now);
        }
        if (!got[1]) {
            assign_card(cards[1], *first, first_league, "cache-derived");
            derive_post(cards[1].g, now);
        }
        if (!got[2]) {
            assign_generic_card(cards[2], *first, first_league, "cache-derived");
        }
    }

    if (cardtest_load_fixture("fill missing cards")) {
        for (int i = 0; i < 4; ++i) {
            if (got[i]) continue;
            if (i == 2) {
                assign_generic_card(cards[i], g_card_fixture->games[0], "nba", "fixture");
            } else {
                assign_card(cards[i], g_card_fixture->games[0], "mlb", "fixture");
                if (i == 0) derive_pre(cards[i].g, now);
                else if (i == 1) derive_post(cards[i].g, now);
                else cards[i].show_situation = true;
            }
            got[i] = true;
        }
    }

    if (first != nullptr) {
        for (int i = 0; i < 4; ++i) {
            if (got[i]) continue;
            if (i == 2) {
                assign_generic_card(cards[i], *first, first_league, "cache-derived");
            } else {
                assign_card(cards[i], *first, i == 3 ? "mlb" : first_league, "cache-derived");
                if (i == 0) derive_pre(cards[i].g, now);
                else if (i == 1) derive_post(cards[i].g, now);
                else fake_mlb_situation(cards[i].g);
            }
            got[i] = true;
        }
    }

    fill_nfl_gridiron_cards(cards, first, now);
    fill_periodclock_cards(cards, first, now);
}

static void draw_card(const Card& card, nb::Canvas16& c) {
    nb::logos::set_phase(nb::logos::Phase::REBUILD);
    if (card.g.status == kStatusPre) {
        nb::render::render_game_card_pre(c, card.g, card.league, kCardResolver,
                                         nb::render::system_local_time, nullptr);
    } else if (card.g.status == kStatusPost) {
        nb::render::render_game_card_post(c, card.g, card.league, kCardResolver,
                                          nb::render::system_local_time, nullptr,
                                          static_cast<int64_t>(time(nullptr)));
    } else {
        nb::render::render_game_card_live(c, card.g, card.league, kCardResolver, card.show_situation);
    }
}

static void card_test() {
    if (!nb::panel::init(g_cfg.hw)) {
        Serial.println("cardtest: panel.begin() FAILED — check adapter PSU / pins");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    const uint8_t br = g_cfg.hw.brightness > 50 ? 50 : g_cfg.hw.brightness;
    nb::panel::set_brightness(br);
    Serial.printf("cardtest: panel %dx%d, brightness=%u\n", nb::panel::width(), nb::panel::height(),
                  static_cast<unsigned>(br));

    const bool logos_ok = nb::logos::init_mmap();
    Serial.printf("cardtest logos: %s count=%u\n", logos_ok ? "mmap OK" : "MISSED, using abbrev fallback",
                  static_cast<unsigned>(nb::logos::table().count));

    int cache_games = 0;
    const bool time_ok = cardtest_wifi_time();
    if (time_ok) {
        g_card_snaps = static_cast<GameList*>(
            heap_caps_calloc(DataCache::kLeagues, sizeof(GameList), MALLOC_CAP_SPIRAM));
        DataCache* cache = static_cast<DataCache*>(
            heap_caps_calloc(1, sizeof(DataCache), MALLOC_CAP_SPIRAM));
        if (g_card_snaps != nullptr && cache != nullptr) cache_games = cardtest_poll(cache);
        else Serial.println("cardtest poll: alloc FAIL");
    }
    Serial.printf("cardtest source: %s (cache games=%d)\n",
                  cache_games > 0 ? "cache" : "fixture", cache_games);

    Card cards[kCardCount] = {};
    build_cards(cards);
    for (int i = 0; i < kCardCount; ++i) {
        if (cards[i].league == nullptr || cards[i].league[0] == '\0') {
            Serial.printf("cardtest: card '%s' has no game — fallback fixture missing\n", cards[i].label);
            for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
    for (int i = 0; i < kCardCount; ++i)
        Serial.printf("cardtest map: %s src=%s\n", cards[i].label, cards[i].src);

    const int pw = nb::panel::width(), ph = nb::panel::height();
    nb::Canvas16 card = nb::canvas_alloc(nb::render::CARD_W, static_cast<uint16_t>(ph));
    if (!card.valid()) {
        Serial.println("cardtest: canvas alloc FAIL");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    (void)pw;

    for (;;) {
        for (int i = 0; i < kCardCount; ++i) {
            const Card& c = cards[i];
            char as[8], hs[8];
            score_text(c.g.away_score, as, sizeof as);
            score_text(c.g.home_score, hs, sizeof hs);
            Serial.printf("cardtest card: %s | %s:%s @ %s | %s-%s | src=%s | sit=%u poss=%s\n",
                          c.label, c.league, c.g.away.abbr, c.g.home.abbr, as, hs, c.src,
                          static_cast<unsigned>(c.show_situation), c.g.situation.possession);
            const uint32_t end = millis() + 4000;
            while (static_cast<int32_t>(end - millis()) > 0) {
                draw_card(c, card);
                nb::panel::blit(card);
                vTaskDelay(pdMS_TO_TICKS(33));
            }
        }
    }
}
#endif  // NB_CARD_TEST

void setup() {
    boot_prologue();
    boot_load_config();  // card_test reads g_cfg.hw
    card_test();  // never returns
}
void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }
