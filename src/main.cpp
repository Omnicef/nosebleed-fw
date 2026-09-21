// SPDX-License-Identifier: GPL-3.0-only
//
// T-1.5 — Phase 1 task-topology skeleton. Four pinned tasks per AGENTS.md
// "Task topology" table, each logging a heartbeat with core ID, heap, and its
// stack high-water mark so T-1.5 accept ("high-water marks leave >25%
// headroom") is measurable.

#ifdef ARDUINO
#include "envs/common.h"  // boot prologue, g_cfg, PSRAM allocators, snapshots

static void heartbeat(const char* name) {
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  // uxTaskGetStackHighWaterMark returns the minimum remaining stack in words
  // since the task started; on xtensa a word is 4 bytes.
  UBaseType_t hw_words = uxTaskGetStackHighWaterMark(h);
  Serial.printf("[%s] core=%d heap=%lu stack_min_free=%lu B\n",
                name,
                static_cast<int>(xPortGetCoreID()),
                static_cast<unsigned long>(esp_get_free_heap_size()),
                static_cast<unsigned long>(hw_words * sizeof(portSTACK_TYPE)));
}

// ---------------------------------------------------------------------------
// Normal boot — Phase 7 wiring. Producers (clock + one scoreboard per league)
// live in a carousel-ordered vector; the poll task (core 0) fetches, rebuilds
// the mega-strip into StripHolder::back() on a cards_key change, and commits;
// the render task (core 1) only reads front()/generation() and blits a window
// (T-7.2/T-7.3). T-7.3's full-repaint rule is upheld by panel::blit's own
// full-panel write (T-2.8 fix) — never touch lib/panel/panel.cpp.
// UNVERIFIED: the stale-column behaviour of this path on the real DMA panel
// (host-proven only via blit_window; no HUB75 simulator exists, PLAN §4).
// ---------------------------------------------------------------------------
#include <WiFi.h>
#include <atomic>
#include <cstring>
#include <ctime>
#include "esp_system.h"  // esp_restart (T-9.6 factory-reset reboot)
#include "esp_ota_ops.h"   // T-9.4 rollback validation
#include "esp_app_format.h"  // esp_app_desc_t (boot-guard image id)
#include "cache.h"
#include "clock_widget.h"
#include "weather_widget.h"
#include "date_window.h"
#include "espn.h"
#include "espn_json.h"
#include "font.h"
#include "game.h"
#include "info_cache.h"
#include "local_time.h"
#include "logos_esp.h"
#include "logos_ota.h"
#include "net.h"
#include "panel.h"
#include "poll.h"
#include "scoreboard_widget.h"
#include "scroll.h"
#include "strip.h"
#include "ticker_json.h"
#include "weather_json.h"
#include "web.h"

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

static nb::data::DataCache* g_cache = nullptr;
static nb::data::InfoCache g_info;  // weather + ticker (T-10.x); small, static
static nb::render::StripHolder g_holder;
static nb::render::StripBuilder g_builder;  // writer: poll task only
static nb::render::ClockWidget* g_clock = nullptr;
static nb::render::ScoreboardWidget* g_sb[kLeagueSlugCount] = {};
static nb::render::WeatherWidget* g_weather = nullptr;
static bool g_wen[nb::config::kMaxWidgets] = {};  // stable enabled flags for widgets[]
static uint32_t g_strip_key = 0;

static nb::LogoArt boot_logo(void*, const char* league, const char* abbr, int h) {
    nb::logos::Ref r;
    if (league == nullptr || abbr == nullptr ||
        !nb::logos::lookup(league, abbr, static_cast<uint16_t>(h), r))
        return nb::LogoArt{nullptr, nullptr, 0, 0};
    return nb::LogoArt{reinterpret_cast<const uint16_t*>(r.px), r.mask,
                       static_cast<int>(r.w), static_cast<int>(r.h)};
}
static const nb::render::LogoResolver kBootResolver = {nullptr, boot_logo};

static int league_of_slug(const char* slug) {
    for (int i = 0; i < kLeagueSlugCount; ++i)
        if (std::strcmp(kLeagueSlugs[i], slug) == 0) return i;
    return -1;
}

static void boot_apply_favorites(const nb::config::Config& cfg);

// T-10.3 — is a carousel row of this type switched on? Also gates the POLL
// side: a configured-but-hidden info widget must not keep fetching.
static bool widget_row_enabled(const nb::config::Config& cfg, const char* type) {
    for (int i = 0; i < cfg.widget_count; ++i)
        if (std::strcmp(cfg.widgets[i].type, type) == 0) return cfg.widgets[i].enabled != 0;
    return false;
}

// Producers are constructed once (one clock, one scoreboard per known
// league) and ordered per pass by boot_order_producers(). T-8.5: the strip
// order IS the carousel widget order — /api/widgets/reorder needs no
// restart, only a strip-key change.
// +5 headroom: weather now, news/stocks/crypto tickers at T-10.4.
static constexpr int kMaxProducers = kLeagueSlugCount + 5;
static nb::render::CardProducer* g_prod[kMaxProducers];
static int g_prod_n = 0;

static void boot_create_producers() {
    for (int i = 0; i < g_cfg.widget_count; ++i) {
        const auto& w = g_cfg.widgets[i];
        if (std::strcmp(w.type, "clock") == 0 && g_clock == nullptr) {
            g_clock = new nb::render::ClockWidget(nb::render::system_local_time, nullptr,
                                                  g_cfg.hw.clock_24h != 0);
        } else if (std::strcmp(w.type, "scoreboard") == 0) {
            const int lg = league_of_slug(w.league);
            if (lg >= 0 && g_sb[lg] == nullptr)
                g_sb[lg] = new nb::render::ScoreboardWidget(g_cache, lg, kLeagueSlugs[lg],
                                                            &g_wen[i], kBootResolver,
                                                            nb::render::system_local_time, nullptr);
        } else if (std::strcmp(w.type, "weather") == 0 && g_weather == nullptr) {
            g_weather = new nb::render::WeatherWidget(&g_info, &g_wen[i]);
        }
    }
}

// Carousel order = widgets sorted by `order` (insertion sort, <=16 rows).
// Also refreshes the stable enabled flags the producers point into.
static void boot_order_producers(const nb::config::Config& cfg) {
    uint8_t idx[nb::config::kMaxWidgets];
    int n = 0;
    for (int i = 0; i < cfg.widget_count && n < nb::config::kMaxWidgets; ++i) idx[n++] = i;
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && cfg.widgets[idx[b]].order < cfg.widgets[idx[b - 1]].order; --b) {
            const uint8_t t = idx[b];
            idx[b] = idx[b - 1];
            idx[b - 1] = t;
        }
    for (int i = 0; i < nb::config::kMaxWidgets; ++i) g_wen[i] = false;
    int out = 0;
    for (int j = 0; j < n && out < kMaxProducers; ++j) {
        const auto& w = cfg.widgets[idx[j]];
        const int wi = idx[j];
        g_wen[wi] = w.enabled != 0;
        if (std::strcmp(w.type, "clock") == 0 && g_clock != nullptr) {
            g_prod[out++] = g_clock;
        } else if (std::strcmp(w.type, "scoreboard") == 0) {
            const int lg = league_of_slug(w.league);
            if (lg >= 0 && g_sb[lg] != nullptr) g_prod[out++] = g_sb[lg];
        } else if (std::strcmp(w.type, "weather") == 0 && g_weather != nullptr) {
            g_prod[out++] = g_weather;
        }
    }
    g_prod_n = out;
}

// Favourite team ids per league (T-7.5 preemption input). Pointers into the
// caller's config — the poll task's copy is stable for the whole pass.
static void boot_apply_favorites(const nb::config::Config& cfg) {
    static const char* ids[nb::data::kMaxGamesPerLeague];
    for (int lg = 0; lg < kLeagueSlugCount; ++lg) {
        if (g_sb[lg] == nullptr) continue;
        int c = 0;
        for (int f = 0; f < cfg.favorite_count && c < nb::data::kMaxGamesPerLeague; ++f)
            if (league_of_slug(cfg.favorites[f].league) == lg) ids[c++] = cfg.favorites[f].team_id;
        g_sb[lg]->set_priority_team_ids(ids, c);
    }
}

static int boot_producers(nb::render::CardProducer* ps[kMaxProducers]) {
    for (int i = 0; i < g_prod_n; ++i) ps[i] = g_prod[i];
    return g_prod_n;
}

static uint32_t boot_strip_key(nb::render::CardProducer* const* ps, int n, int64_t now,
                               const nb::config::Config& cfg) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) {
        if (!ps[i]->is_visible(now)) continue;
        uint32_t k = ps[i]->cards_key(now);
        for (int b = 0; b < 4; ++b) { h ^= (k >> (8 * b)) & 0xff; h *= 16777619u; }
        h ^= 0xa5;  // order matters; separator
    }
    h = h * 31u + cfg.hw.card_gap + cfg.hw.display_mode * 256u +
        cfg.hw.preemption_enabled * 512u;
    for (int f = 0; f < cfg.favorite_count; ++f)  // T-8.4: favourites reorder the strip
        for (const char* p = cfg.favorites[f].team_id; *p != '\0'; ++p) {
            h ^= static_cast<uint8_t>(*p);
            h *= 16777619u;
        }
    return h;
}

static void boot_rebuild_strip(int64_t now, const nb::config::Config& cfg) {
    nb::render::CardProducer* ps[kMaxProducers];
    const int n = boot_producers(ps);
    int order[kMaxProducers];
    const int on = nb::render::order_producers(ps, n, now, cfg.hw.preemption_enabled != 0, order);
    nb::logos::set_phase(nb::logos::Phase::REBUILD);
    const int placed = g_builder.build(*g_holder.back(), ps, order, on, cfg.hw.card_gap,
                                        nb::panel::height(), now);
    nb::logos::set_phase(nb::logos::Phase::FRAME);
    if (placed > 0) g_holder.commit();  // 0 = nothing visible / OOM: keep last good
#ifdef NB_D2_TRACE
    if (placed > 0)
        Serial.printf("[d2] %lu commit gen=%lu cards=%d\n", static_cast<unsigned long>(millis()),
                      static_cast<unsigned long>(g_holder.generation()), placed);
#endif
    const nb::render::Strip* f = g_holder.front();
    Serial.printf("[strip] rebuilt: %d cards%s, w=%d, pages=%d\n", placed,
                  g_builder.truncated() ? ", PRODUCERS TRUNCATED" : "",
                  f != nullptr ? f->canvas.w : 0, f != nullptr ? f->page_count : 0);
}

// T-8.5: static paging dwell comes from the first enabled widget that has
// one. ponytail: Strip pages carry no per-card widget attribution, so
// per-widget dwell is stored but page-agnostic; wire it per-page if the
// strip ever records page->widget.
static int render_dwell_s() {
    for (int i = 0; i < g_cfg.widget_count; ++i)
        if (g_cfg.widgets[i].enabled != 0 && g_cfg.widgets[i].dwell_s >= 1.0f)
            return g_cfg.widgets[i].dwell_s > 120.0f ? 120 : static_cast<int>(g_cfg.widgets[i].dwell_s);
    return 5;
}

// T-8.6: POST /api/system/show-ip arms an 8 s scrolling splash on the
// render task (the panel is render's property — the web side only flags it).
// T-9.1: the splash is now message-driven — the net task arms the AP SSID
// ("join this network") and the new IP after provisioning. Text is written
// before the deadline store (release/acquire pair); render rebuilds the
// canvas only when the deadline value changes.
static std::atomic<uint32_t> g_ip_deadline{0};
static char g_splash_msg[48] = "";
static void arm_splash(const char* msg) {
    snprintf(g_splash_msg, sizeof(g_splash_msg), "%s", msg);
    uint32_t dl = millis() + 8000;
    if (dl == 0) dl = 1;  // 0 means "no splash"
    g_ip_deadline.store(dl, std::memory_order_release);
}
static void web_show_ip() { arm_splash(WiFi.localIP().toString().c_str()); }

// T-9.6 — factory reset, physical path. The DevKitC-1 "BOOT" button (GPIO0)
// is the only user button the board has and the panel pin map (T-2.8) leaves
// it free. 5 s hold while RUNNING (loop() below) clears NVS and reboots —
// for a device whose network config is what's broken. The boot-time strap
// meaning of GPIO0 (download mode) is untouched: the loop only counts holds
// after the app is up. Web path: POST /api/system/factory-reset?confirm=1
// (web.cpp) calls arm_reboot() after its response is queued — handlers run
// on async_tcp and cannot block to flush, so loop() does the restart.
static constexpr int kResetBtn = 0;  // GPIO0 = BOOT button
static std::atomic<uint32_t> g_reboot_at{0};
static const char* g_reboot_why = "";  // set before arming; loop() prints it on the way out
static void arm_reboot(const char* why) {
    g_reboot_why = why;
    uint32_t dl = millis() + 700;
    if (dl == 0) dl = 1;  // 0 means "not armed"
    g_reboot_at.store(dl, std::memory_order_release);
}
static void do_reboot(const char* why) {
    Serial.printf("[reset] %s — rebooting\n", why);
    Serial.flush();
    esp_restart();
}

static void task_render(void*) {
    nb::config::bind_render_task(xTaskGetCurrentTaskHandle());
    const int pw = nb::panel::width(), ph = nb::panel::height();
    nb::Canvas16 black = nb::canvas_alloc(static_cast<uint16_t>(pw), static_cast<uint16_t>(ph));
    double scroll = 0.0;
    uint32_t seen_gen = 0, last_ms = millis(), hb = last_ms;
    nb::render::PageState pst;
    // T-7.6 instrumentation: fps + worst frame gap per 10 s window. Any key
    // over serial forces a 200 ms stall — the panel must HITCH, not blank
    // (DMA refresh is autonomous).
    uint32_t frames = 0, fps_t = last_ms;
    uint32_t worst_gap = 0, next_wait = 1;
    for (;;) {
        if (Serial.available() > 0) {
            while (Serial.available()) Serial.read();
            Serial.println("[render] forced 200 ms stall — panel must hitch, not blank");
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // 33 ms frame budget minus last frame's BUSY time (excluding the
        // idle wait). Subtracting the whole period instead undershoots and
        // ran at 55 fps — measured, not theorised.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next_wait))) {
            nb::config::load(g_cfg);
            nb::config::apply_timezone(g_cfg.hw.timezone);
            nb::panel::set_brightness(g_cfg.hw.brightness);
            g_strip_key = 0;  // force rebuild on next poll pass
        }
        const uint32_t t_busy = millis();
        // T-8.6 show-ip splash: 8 s scrolling IP, replacing the strip for the
        // window (wrap-safe deadline math). Built once per trigger.
        const uint32_t ipdl = g_ip_deadline.load(std::memory_order_acquire);
        if (ipdl != 0 && static_cast<int32_t>(ipdl - millis()) <= 0)
            g_ip_deadline.store(0, std::memory_order_relaxed);
        bool ip_shown = false;
        if (ipdl != 0) {
            static nb::Canvas16 ip_c;
            static uint32_t ip_dl_built = 0;
            static double ip_scroll = 0;
            static uint32_t ip_t = 0;
            if (ip_dl_built != ipdl) {
                nb::canvas_free(ip_c);
                const int tw = nb::text_width(nb::FONT_SPLEEN_6X12,
                                              static_cast<int>(strlen(g_splash_msg)));
                ip_c = nb::canvas_alloc(static_cast<uint16_t>(tw + pw), static_cast<uint16_t>(ph));
                if (ip_c.valid())
                    nb::draw_text_outlined(ip_c, nb::FONT_SPLEEN_6X12, pw,
                                             (ph - nb::FONT_SPLEEN_6X12.box_h) / 2, g_splash_msg,
                                             nb::rgb565(255, 196, 0), 0);
                ip_dl_built = ipdl;
                ip_scroll = 0;
            }
            ip_t = ip_t == 0 ? millis() : ip_t;
            ip_scroll += 20.0 * (millis() - ip_t) / 1000.0;
            ip_t = millis();
            const int w0 = nb::render::scroll_window_x(ip_c.w, pw, static_cast<int32_t>(ip_scroll));
            nb::panel::blit(ip_c, -w0, 0);  // invalid canvas blits black (bounds-checked)
            ip_shown = true;
        }
        const nb::render::Strip* s = ip_shown ? nullptr : g_holder.front();
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (s == nullptr || !s->canvas.valid()) {
            if (!ip_shown) nb::panel::blit(black);
        } else {
            const uint32_t g = g_holder.generation();
            if (g != seen_gen) {
                seen_gen = g;
#ifdef NB_D2_TRACE
                Serial.printf("[d2] %lu gen=%lu visible w=%d pages=%d\n",
                              static_cast<unsigned long>(millis()), static_cast<unsigned long>(g), s->canvas.w, s->page_count);
#endif
                scroll = nb::render::scroll_rewrap(static_cast<int32_t>(scroll), s->canvas.w, pw);
            }
            int w0;
            if (g_cfg.hw.display_mode == nb::config::kDisplayStatic) {
#ifdef NB_D2_TRACE
                const int page_before = pst.page;
#endif
                w0 = nb::render::page_window_x(*s, pst, now, render_dwell_s());
#ifdef NB_D2_TRACE
                if (pst.page != page_before)
                    Serial.printf("[d2] %lu page %d->%d\n", static_cast<unsigned long>(millis()),
                                  page_before, pst.page);
#endif
            } else {
                const uint32_t ms = millis();
                scroll += static_cast<double>(g_cfg.hw.scroll_speed) * (ms - last_ms) / 1000.0;
                w0 = nb::render::scroll_window_x(s->canvas.w, pw, static_cast<int32_t>(scroll));
            }
            nb::panel::blit(s->canvas, -w0, 0);  // full-panel write (T-7.3 rule)
        }
        const uint32_t t_prev = last_ms;
        last_ms = millis();
        const uint32_t gap = last_ms - t_prev;
        if (gap > worst_gap) worst_gap = gap;
        const uint32_t work = last_ms - t_busy;
        next_wait = work < 33 ? 33 - work : 1;
        ++frames;
        if (last_ms - fps_t >= 10000) {
            Serial.printf("[render] %.1f fps worst-frame-gap=%lu ms heap=%lu\n",
                          frames * 1000.0f / (last_ms - fps_t), static_cast<unsigned long>(worst_gap),
                          static_cast<unsigned long>(esp_get_free_heap_size()));
            frames = 0;
            worst_gap = 0;
            fps_t = last_ms;
        }
        if (last_ms - hb >= 30000) { heartbeat("render"); hb = last_ms; }
    }
}

static void task_poll(void*) {
    using namespace nb::data;
    static PollScheduler sch;
    static InfoScheduler isch;  // T-10.x: weather + ticker, same task, same session rule
    while (time(nullptr) < 1700000000) vTaskDelay(pdMS_TO_TICKS(200));  // SNTP first (TLS)
    sch.reset(time(nullptr));
    isch.reset(time(nullptr));
    for (;;) {
        // T-8.5: the poll task owns its own config copy. The shared g_cfg is
        // the RENDER task's (reloaded on save-notify); poll reloads once per
        // pass here, so /api changes land on the next pass with no cross-task
        // mutation of g_cfg.
        static nb::config::Config pc;
        nb::config::load(pc);
        // D-3: the scheduler config is re-applied EVERY pass from pc — an
        // enable via /api must start polling without a reboot. set() touches
        // cfg_ only; the due_ schedule survives, so a mid-life enable is due
        // at once (stale due_ is in the past) and a disable just stops runs.
        for (int i = 0; i < kLeagueSlugCount; ++i) sch.set(i, {false, 20, 120});
        for (int i = 0; i < pc.league_count; ++i) {
            const int lg = league_of_slug(pc.leagues[i].id);
            if (lg >= 0)
                sch.set(lg, {pc.leagues[i].enabled != 0, pc.leagues[i].poll_interval_live,
                             pc.leagues[i].poll_interval_idle});
        }
        const time_t now = time(nullptr);
        for (int lg = 0; lg < kLeagueSlugCount; ++lg) {
            if (!sch.due(lg, now)) continue;
            char url[192], dates[16];
            size_t wire = 0;
            bool ok = false, has_live = false;
            local_day(dates, sizeof dates, now);
            scoreboard_url(url, sizeof url, kLeagueApiPaths[lg], dates);
            {
                JsonDocument doc = make_psram_doc();
                ok = espn_fetch_scoreboard(url, doc, &wire);
                if (ok) {
                    GameList* w = g_cache->writable(lg);
                    w->count = to_games(doc, *w);
                    filter_yesterday_today(w, now);
                    if (w->count == 0) {  // opportunistic yesterday leg (T-5.6)
                        char yd[16];
                        if (local_yesterday(yd, sizeof yd, now) &&
                            scoreboard_url(url, sizeof url, kLeagueApiPaths[lg], yd)) {
                            JsonDocument doc2 = make_psram_doc();
                            if (espn_fetch_scoreboard(url, doc2, &wire))
                                w->count = to_games(doc2, *w);
                        }
                    }
                    for (int i = 0; i < w->count; ++i) has_live |= (w->games[i].status == kStatusIn);
                    g_cache->publish(lg, static_cast<int64_t>(now));
                }
                // fetch failure: keep last good, scheduler retries per cadence
            }
            sch.done(lg, time(nullptr), has_live);
            Serial.printf("[poll] %s ok=%d wire=%u B\n", kLeagueSlugs[lg], ok,
                          static_cast<unsigned>(wire));
        }
        // T-10.1 — weather: own deadline slot on the same scheduler clock,
        // fetched only when the weather widget row is enabled (T-10.3: the
        // gate moved from "lat set" to the carousel switch — configuring a
        // location without showing the card must not keep polling).
        // 900 s cadence; nothing on a panel needs weather faster. Fetched
        // into a local first — a failed response never touches the slot,
        // so the last-good Weather keeps rendering.
        isch.set(info_src::kWeather, {widget_row_enabled(pc, "weather"), 900, 900});
        if (isch.due(info_src::kWeather, now)) {
            char wurl[256];
            bool wok = false;
            if (weather_url(wurl, sizeof wurl, pc.svc.lat, pc.svc.lon, pc.svc.imperial != 0)) {
                Weather w;
                if (weather_fetch(wurl, w)) {
                    *g_info.writable_weather() = w;
                    g_info.publish_weather(static_cast<int64_t>(now));
                    wok = true;
                }
            }
            isch.done(info_src::kWeather, time(nullptr), false);
            Serial.printf("[poll] weather ok=%d\n", wok);
        }
        // T-10.3 — ticker sources: one slot, one deadline, one fetch each.
        // A source's failure only ever skips its own publish, so a dead
        // news feed never takes the crypto board down with it. All three
        // run on this task, sequentially — still one TLS session at a time.
        nb::config::ApiKeys keys;
        nb::config::load_keys(keys);  // tiny NVS blob; poll task is the only reader
        struct { const char* widget; const char* param; } tsrc[] = {
            {"news", keys.gnews}, {"stocks", keys.finnhub}, {"crypto", ""}};
        for (int ti = 0; ti < 3; ++ti) {
            const int src = info_src::kNews + ti;
            const bool armed = widget_row_enabled(pc, tsrc[ti].widget) &&
                               (ti == 2 || tsrc[ti].param[0] != '\0');  // keyed srcs need keys
            isch.set(src, {armed, 900, 900});
            if (!isch.due(src, now)) continue;
            TickerList tl;
            bool tok = false;
            if (ti == 0) {  // news
                char url[256];
                if (news_url(url, sizeof url, pc.svc.news_category, pc.svc.news_country,
                             keys.gnews) &&
                    news_fetch(url, tl)) {
                    tok = true;
                }
            } else if (ti == 1) {  // stocks — per-symbol GETs inside
                if (pc.svc.stock_symbols[0] != '\0' &&
                    stocks_fetch(pc.svc.stock_symbols, keys.finnhub, tl)) {
                    tok = true;
                }
            } else {  // crypto — keyless, ids list is the gate
                char url[256];
                if (coin_url(url, sizeof url, pc.svc.crypto_ids) &&
                    coins_fetch(url, pc.svc.crypto_ids, tl)) {
                    tok = true;
                }
            }
            if (tok) {
                *g_info.writable_ticker(ti) = tl;
                g_info.publish_ticker(ti, static_cast<int64_t>(now));
            }
            isch.done(src, time(nullptr), false);
            Serial.printf("[poll] ticker%d ok=%d\n", ti, tok);
        }
        const int64_t now2 = time(nullptr);
        // T-9.5: logo atlas OTA — boot + daily, or on demand from
        // /api/logos/update. Runs here so fetches stay single-session.
        static int64_t last_logos = 0;
        if (now2 > 1700000000 && (nb::logos::ota::requested().exchange(false) ||
                                  last_logos == 0 || now2 - last_logos >= 86400)) {
            last_logos = now2;
            const int r = nb::logos::ota::check();
            Serial.printf("[logos] ota %d\n", r);
            if (r == 1) g_strip_key = 0;  // new art ⇒ rebuild
        }
        boot_order_producers(pc);  // T-8.5: carousel order/enabled apply on this pass
        boot_apply_favorites(pc);  // T-8.4: /api/favorites applies on this pass
        nb::render::CardProducer* ps[kMaxProducers];
        const int n = boot_producers(ps);
        const uint32_t key = boot_strip_key(ps, n, now2, pc);
        if (key != g_strip_key) {
            g_strip_key = key;
            boot_rebuild_strip(now2, pc);
        }
        int64_t wake = sch.next_wake(now2);
        const int64_t iwake = isch.next_wake(now2);  // T-10.1: info feeds share the sleep
        if (iwake < wake) wake = iwake;
        int64_t sleep_s = wake - now2;
        if (sleep_s < 1) sleep_s = 1;  // 1 s tick so per-minute clock keys rebuild promptly
        if (sleep_s > 30) sleep_s = 30;
        vTaskDelay(pdMS_TO_TICKS(1000 * sleep_s));
    }
}

static void task_web(void*) {
  while (!nb::net::link_ready()) {  // STA up or provisioning AP up (T-9.1)
    heartbeat("web");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  Serial.printf("[web] up (%s)\n",
                nb::net::ap_active() ? nb::net::ap_ssid() : WiFi.localIP().toString().c_str());
  if (!nb::web::init()) Serial.println("[web] init FAILED");
  for (;;) {
    heartbeat("web");
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

static void task_net(void*) {
    bool sntp = false;
    uint32_t hb = millis();
    nb::net::init();
    for (;;) {
        nb::net::tick();
        if (nb::net::take_edge_up()) {
            if (!sntp) {  // SNTP before any TLS (AGENTS boot-order rule)
                configTime(0, 0, "pool.ntp.org", "time.nist.gov");
                sntp = true;
            }
            const String ip = WiFi.localIP().toString();
            Serial.printf("[net] wifi %s\n", ip.c_str());
            arm_splash(ip.c_str());  // the footer of onboard.html promises this address
        }
        if (nb::net::take_edge_ap_up()) {
            Serial.printf("[net] AP %s open — join it to provision wifi\n", nb::net::ap_ssid());
            char msg[64];
            snprintf(msg, sizeof(msg), "Join AP \"%s\" to wifi-provision", nb::net::ap_ssid());
            arm_splash(msg);  // T-9.1: a headless device names the network it wants joined
        }
        if (millis() - hb >= 30000) { heartbeat("net"); hb = millis(); }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// T-9.4 crash-loop rollback. The IDF pending-state machine is NOT the gate
// here: measured on this board (2026-09-20), esp_ota_set_boot_partition()
// leaves the staged entry VALID at boot#1 (raw otadata dump), so nothing
// ever reaches PENDING_VERIFY→ABORTED — yet a hand-written NEW entry IS
// converted and de-selected correctly, and the bootloader always excludes
// INVALID/ABORTED entries (bootloader_common_ota_select_invalid). So the
// image guards itself: an RTC counter (survives panic/abort/WDT resets)
// armed on every boot and disarmed at the end of setup(); three
// consecutive boots of the SAME image without completing setup() call
// esp_ota_mark_app_invalid_rollback_and_reboot(), the bootloader excludes
// the INVALID entry and the other slot wins. The app-build id changes
// with the image, so a fresh flash can never inherit an armed counter.
struct NbBootGuard {
    uint32_t magic;
    uint32_t attempts;
    uint32_t image_id;  // FNV-1a of the running image's ELF sha256 (per-build)
};
constexpr uint32_t kGuardMagic = 0x4E424731;  // "NBG1"
RTC_NOINIT_ATTR NbBootGuard g_boot_guard;

inline uint32_t running_image_id() {
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(esp_ota_get_running_partition(), &desc) == ESP_OK) {
        uint32_t id = 2166136261u;
        for (unsigned i = 0; i < sizeof(desc.app_elf_sha256); i++) {
            id ^= desc.app_elf_sha256[i];
            id *= 16777619u;
        }
        return id;
    }
    return 0;
}

// True when this is the first boot of a DIFFERENT image — the new image
// always starts with a disarmed guard.
bool guard_new_image() {
    const uint32_t id = running_image_id();
    if (g_boot_guard.magic != kGuardMagic || g_boot_guard.image_id != id) {
        g_boot_guard.magic = kGuardMagic;
        g_boot_guard.image_id = id;
        g_boot_guard.attempts = 0;
        return true;
    }
    return false;
}

void setup() {
  boot_prologue();      // banner, sdkconfig/partition snapshots, PSRAM allocator
  esp_ota_img_states_t boot_state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t boot_state_err =
      esp_ota_get_state_partition(esp_ota_get_running_partition(), &boot_state);
  Serial.printf("[ota] running from %s, state=%u err=%s\n", esp_ota_get_running_partition()->label,
                static_cast<unsigned>(boot_state), esp_err_to_name(boot_state_err));
  guard_new_image();  // fresh image id resets the counter
  if (++g_boot_guard.attempts >= 3) {
      Serial.println("[ota] 3rd consecutive boot without completing setup — rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot();  // reboots into the other slot
  }
  boot_load_config();   // load g_cfg + apply timezone (panel/card tests did the same)

  // Normal boot bring-up (Phase 7 wiring). Brightness clamped to 50 % until a
  // 5 V / 4 A PSU is confirmed on the bench (AGENTS power rule); live config
  // changes in task_render apply the raw value.
  if (!nb::panel::init(g_cfg.hw)) {
    Serial.println("panel.begin() FAILED — check adapter PSU / pins");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
  }
  nb::panel::set_brightness(g_cfg.hw.brightness > 50 ? 50 : g_cfg.hw.brightness);
  Serial.printf("panel: %dx%d\n", nb::panel::width(), nb::panel::height());
  if (!nb::logos::init_mmap())
    Serial.println("logos: partition missed — abbreviation fallback");
  g_cache = static_cast<nb::data::DataCache*>(
      heap_caps_calloc(1, sizeof(nb::data::DataCache), MALLOC_CAP_SPIRAM));
  if (g_cache == nullptr) {
    Serial.println("FATAL: no PSRAM for DataCache");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
  }
  boot_create_producers();
  boot_order_producers(g_cfg);
  boot_apply_favorites(g_cfg);
  nb::logos::set_phase(nb::logos::Phase::FRAME);  // lookups only during rebuilds (T-3.7)
  if (!g_builder.init_scratch(nb::render::kMaxStripCards,
                              static_cast<uint16_t>(nb::panel::height())))
    Serial.println("strip scratch alloc FAILED");

  // Start the network stack before any server task can bind: LwIP's tcpip
  // thread is created here, and AsyncTCP's begin() asserts if it is absent.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  nb::web::set_preview_source(&g_holder);  // T-8.6 /preview
  nb::web::set_show_ip_hook(web_show_ip);
  nb::web::set_reboot_hook(arm_reboot);   // T-9.6 factory reset (web path)
  pinMode(kResetBtn, INPUT_PULLUP);       // T-9.6 factory reset (BOOT hold)

  // Exact cores / priorities / stacks from AGENTS.md. ESP-IDF's
  // xTaskCreatePinnedToCore takes the stack size in BYTES on this port.
  xTaskCreatePinnedToCore(task_render, "render",  8 * 1024, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(task_poll,   "poll",   12 * 1024, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(task_web,    "web",     8 * 1024, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(task_net,    "net",     4 * 1024, nullptr, 1, nullptr, 0);

  // T-9.4 — reaching the end of setup() is the image's self-test. Arm the
  // crash-loop guard counter (armed each boot at the top of setup; disarmed
  // here). Three consecutive boots without this line mark the running image
  // INVALID via esp_ota_mark_app_invalid_rollback_and_reboot(), which the
  // bootloader excludes from selection — the other slot wins. See
  // NbBootGuard above for why the PENDING_VERIFY state machine is not the
  // mechanism on this stack (measured 2026-09-20).
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* configured = esp_ota_get_boot_partition();
  if (running != configured)
      Serial.printf("[ota] bootloader fell back to %s after a failed update\n", running->label);
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
      ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
          Serial.printf("[ota] %s confirmed good, rollback cancelled\n", running->label);
  }
  g_boot_guard.attempts = 0;  // self-test passed
}

void loop() {
    // T-9.6 factory-reset chores, 100 ms tick (see kResetBtn above).
    static uint32_t held = 0;
    if (digitalRead(kResetBtn) == LOW) {
        if (++held >= 50) {  // 5 s continuous hold
            if (nb::config::reset())
                do_reboot("BOOT held 5 s: NVS cleared");
            else
                do_reboot("BOOT held 5 s: NVS clear FAILED (still wiping boot state)");
        }
    } else {
        held = 0;
    }
    const uint32_t dl = g_reboot_at.load(std::memory_order_acquire);
    if (dl != 0 && static_cast<int32_t>(millis() - dl) >= 0) do_reboot(g_reboot_why);
    vTaskDelay(pdMS_TO_TICKS(100));
}

#else
// T-2.10 — native live preview: the scrolling strip rendered through
// lib/render and painted to the terminal as ANSI 24-bit half-blocks (each
// cell = 1x2 px, so 64x32 is 64 cols x 16 text rows at roughly panel
// aspect). Run it as `.pio/build/native/program` after `pio run -e native`
// — NOT `-t exec`, whose console wrapper strips the escape codes. The
// RGBMatrixEmulator replacement: no ESP32 simulator can render HUB75
// (PLAN §4), so this is the unplugged card-design loop. Ctrl-C quits.
// Real panel verification stays on hardware.
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "canvas.h"
#include "font_data.h"
#include "primitives.h"

namespace {

volatile sig_atomic_t g_run = 1;
void on_signal(int) { g_run = 0; }

double now_s() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<double>(tv.tv_sec) + tv.tv_usec * 1e-6;
}

// Three placeholder cards in a mega-strip — stand-ins for Phase 6 producers.
nb::Canvas16 make_strip() {
    const int W = 272, H = 32;
    nb::Canvas16 s = nb::canvas_alloc(static_cast<uint16_t>(W),
                                      static_cast<uint16_t>(H));
    if (!s.valid()) return s;
    const uint16_t white = nb::rgb565(255, 255, 255),
                   green = nb::rgb565(0, 255, 0),
                   red = nb::rgb565(255, 0, 0),
                   blue = nb::rgb565(0, 0, 255),
                   yellow = nb::rgb565(255, 255, 0),
                   grey = nb::rgb565(64, 64, 64);

    // card 1: outlined title (the T-2.5 path)
    nb::draw_text_outlined(s, nb::FONT_SPLEEN_6X12, 8, 4, "NOSEBLEED", white, 0);
    nb::draw_text(s, nb::FONT_SPLEEN_5X8, 8, 21, "PHASE 2", green);

    // card 2: primitives — diamond (fill_polygon) + ellipse ring + bar
    const nb::Point diamond[4] = {{144, 3}, {164, 15}, {144, 27}, {124, 15}};
    nb::fill_polygon(s, diamond, 4, red);
    nb::ellipse(s, 184, 15, 10, 10, green);
    nb::fill_rect(s, 200, 12, 20, 6, blue);

    // card 3: colour bars + dense tom-thumb text (font legibility check)
    nb::fill_rect(s, 228, 2, 8, 4, red);
    nb::fill_rect(s, 237, 2, 8, 4, green);
    nb::fill_rect(s, 246, 2, 8, 4, blue);
    nb::fill_rect(s, 255, 2, 8, 4, yellow);
    nb::draw_text(s, nb::FONT_TOM_THUMB, 228, 12, "live preview", white);
    nb::draw_text(s, nb::FONT_TOM_THUMB, 228, 20, "30fps host", yellow);

    nb::vline(s, 108, 0, H - 1, grey);  // card gaps
    nb::vline(s, 220, 0, H - 1, grey);
    return s;
}

}  // namespace

int main() {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    nb::Canvas16 strip = make_strip();
    if (!strip.valid()) {
        std::fprintf(stderr, "strip alloc failed\n");
        return 1;
    }

    const int PW = 64, PH = 32;
    const int period = static_cast<int>(strip.w) + PW;  // black lead-in wrap
    const double speed = 30.0;                          // px/s, config later

    std::fputs("\x1b[2J\x1b[?25l"
               "nosebleed native preview (T-2.10) — Ctrl-C quits\n\x1b[H",
               stdout);

    double scroll = 0.0, t0 = now_s(), last = t0;
    while (g_run) {
        const double now = now_s();
        const double dt = now - last;
        last = now;
        scroll += speed * dt;
        const int w0 = static_cast<int>(scroll) % period - PW;  // window start

        std::fputs("\x1b[H", stdout);
        char cell[48];
        for (int y = 0; y < PH; y += 2) {
            std::fputs("\x1b[K", stdout);
            for (int x = 0; x < PW; x++) {
                uint8_t r1, g1, b1, r2, g2, b2;
                nb::unpack565(strip.get(x + w0, y), r1, g1, b1);
                nb::unpack565(strip.get(x + w0, y + 1), r2, g2, b2);
                std::snprintf(cell, sizeof cell,
                              "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80",
                              r1, g1, b1, r2, g2, b2);
                std::fputs(cell, stdout);
            }
            std::fputs("\x1b[0m\n", stdout);
        }
        std::fflush(stdout);
        const double frame = 1.0 / 30.0;
        const double spent = now_s() - now;
        if (spent < frame) usleep(static_cast<useconds_t>((frame - spent) * 1e6));
    }
    std::fputs("\x1b[0m\x1b[?25h\n", stdout);
    nb::canvas_free(strip);
    return 0;
}
#endif
