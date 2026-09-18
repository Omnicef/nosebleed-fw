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
#include "cache.h"
#include "clock_widget.h"
#include "date_window.h"
#include "espn.h"
#include "espn_json.h"
#include "game.h"
#include "local_time.h"
#include "logos_esp.h"
#include "panel.h"
#include "poll.h"
#include "scoreboard_widget.h"
#include "scroll.h"
#include "strip.h"

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
static nb::render::StripHolder g_holder;
static nb::render::StripBuilder g_builder;  // writer: poll task only
static nb::render::ClockWidget* g_clock = nullptr;
static nb::render::ScoreboardWidget* g_sb[kLeagueSlugCount] = {};
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

static void boot_apply_favorites();

// Carousel order = widgets sorted by `order`. Producers are built once.
static void boot_build_producers() {
    uint8_t idx[nb::config::kMaxWidgets];
    int n = 0;
    for (int i = 0; i < g_cfg.widget_count && n < nb::config::kMaxWidgets; ++i) idx[n++] = i;
    for (int a = 1; a < n; ++a)  // insertion sort; 16 elements
        for (int b = a; b > 0 && g_cfg.widgets[idx[b]].order < g_cfg.widgets[idx[b - 1]].order; --b) {
            const uint8_t t = idx[b];
            idx[b] = idx[b - 1];
            idx[b - 1] = t;
        }
    for (int j = 0; j < n; ++j) {
        const int wi = idx[j];
        const auto& w = g_cfg.widgets[wi];
        g_wen[wi] = w.enabled != 0;
        if (std::strcmp(w.type, "clock") == 0 && g_clock == nullptr) {
            g_clock = new nb::render::ClockWidget(nb::render::system_local_time, nullptr,
                                                  g_cfg.hw.clock_24h != 0);
        } else if (std::strcmp(w.type, "scoreboard") == 0) {
            const int lg = league_of_slug(w.league);
            if (lg >= 0 && g_sb[lg] == nullptr)
                g_sb[lg] = new nb::render::ScoreboardWidget(g_cache, lg, kLeagueSlugs[lg],
                                                            &g_wen[wi], kBootResolver,
                                                            nb::render::system_local_time, nullptr);
        }
    }
    boot_apply_favorites();
}

// Favourite team ids per league (T-7.5 preemption input). Pointers into the
// static g_cfg arrays — stable.
static void boot_apply_favorites() {
    static const char* ids[nb::data::kMaxGamesPerLeague];
    for (int lg = 0; lg < kLeagueSlugCount; ++lg) {
        if (g_sb[lg] == nullptr) continue;
        int c = 0;
        for (int f = 0; f < g_cfg.favorite_count && c < nb::data::kMaxGamesPerLeague; ++f)
            if (league_of_slug(g_cfg.favorites[f].league) == lg) ids[c++] = g_cfg.favorites[f].team_id;
        g_sb[lg]->set_priority_team_ids(ids, c);
    }
}

static int boot_producers(nb::render::CardProducer* ps[kLeagueSlugCount + 1]) {
    int n = 0;
    if (g_clock) ps[n++] = g_clock;
    for (int lg = 0; lg < kLeagueSlugCount; ++lg)
        if (g_sb[lg]) ps[n++] = g_sb[lg];
    return n;
}

static uint32_t boot_strip_key(nb::render::CardProducer* const* ps, int n, int64_t now) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) {
        if (!ps[i]->is_visible(now)) continue;
        uint32_t k = ps[i]->cards_key(now);
        for (int b = 0; b < 4; ++b) { h ^= (k >> (8 * b)) & 0xff; h *= 16777619u; }
        h ^= 0xa5;  // order matters; separator
    }
    h = h * 31u + g_cfg.hw.card_gap + g_cfg.hw.display_mode * 256u +
        g_cfg.hw.preemption_enabled * 512u;
    return h;
}

static void boot_rebuild_strip(int64_t now) {
    nb::render::CardProducer* ps[kLeagueSlugCount + 1];
    const int n = boot_producers(ps);
    int order[kLeagueSlugCount + 1];
    const int on = nb::render::order_producers(ps, n, now, g_cfg.hw.preemption_enabled != 0, order);
    nb::logos::set_phase(nb::logos::Phase::REBUILD);
    const int placed = g_builder.build(*g_holder.back(), ps, order, on, g_cfg.hw.card_gap,
                                        nb::panel::height(), now);
    nb::logos::set_phase(nb::logos::Phase::FRAME);
    if (placed > 0) g_holder.commit();  // 0 = nothing visible / OOM: keep last good
    const nb::render::Strip* f = g_holder.front();
    Serial.printf("[strip] rebuilt: %d cards%s, w=%d, pages=%d\n", placed,
                  g_builder.truncated() ? ", PRODUCERS TRUNCATED" : "",
                  f != nullptr ? f->canvas.w : 0, f != nullptr ? f->page_count : 0);
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
        const nb::render::Strip* s = g_holder.front();
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (s == nullptr || !s->canvas.valid()) {
            nb::panel::blit(black);
        } else {
            const uint32_t g = g_holder.generation();
            if (g != seen_gen) {
                seen_gen = g;
                scroll = nb::render::scroll_rewrap(static_cast<int32_t>(scroll), s->canvas.w, pw);
            }
            int w0;
            if (g_cfg.hw.display_mode == nb::config::kDisplayStatic) {
                w0 = nb::render::page_window_x(*s, pst, now, 5);
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
    for (int i = 0; i < nb::config::kLeagueSlugCount; ++i) sch.set(i, {false, 20, 120});
    for (int i = 0; i < g_cfg.league_count; ++i) {
        const int lg = league_of_slug(g_cfg.leagues[i].id);
        if (lg >= 0)
            sch.set(lg, {g_cfg.leagues[i].enabled != 0, g_cfg.leagues[i].poll_interval_live,
                         g_cfg.leagues[i].poll_interval_idle});
    }
    while (time(nullptr) < 1700000000) vTaskDelay(pdMS_TO_TICKS(200));  // SNTP first (TLS)
    sch.reset(time(nullptr));
    for (;;) {
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
        const int64_t now2 = time(nullptr);
        nb::render::CardProducer* ps[kLeagueSlugCount + 1];
        const int n = boot_producers(ps);
        const uint32_t key = boot_strip_key(ps, n, now2);
        if (key != g_strip_key) {
            g_strip_key = key;
            boot_rebuild_strip(now2);
        }
        int64_t sleep_s = sch.next_wake(now2) - now2;
        if (sleep_s < 1) sleep_s = 1;  // 1 s tick so per-minute clock keys rebuild promptly
        if (sleep_s > 30) sleep_s = 30;
        vTaskDelay(pdMS_TO_TICKS(1000 * sleep_s));
    }
}

static void task_web(void*) { for (;;) { heartbeat("web"); vTaskDelay(pdMS_TO_TICKS(5000)); } }

static void task_net(void*) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    bool sntp = false;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            if (WIFI_SSID[0] != '\0') {
                WiFi.begin(WIFI_SSID, WIFI_PASS);
                for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i)
                    vTaskDelay(pdMS_TO_TICKS(250));
            }
            if (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(30000));  // ponytail: fixed retry, exponential if routers ever flake
        } else {
            if (!sntp) {
                configTime(0, 0, "pool.ntp.org", "time.nist.gov");
                sntp = true;
                Serial.printf("[net] wifi %s\n", WiFi.localIP().toString().c_str());
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void setup() {
  boot_prologue();      // banner, sdkconfig/partition snapshots, PSRAM allocator
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
  boot_build_producers();
  nb::logos::set_phase(nb::logos::Phase::FRAME);  // lookups only during rebuilds (T-3.7)
  if (!g_builder.init_scratch(nb::render::kMaxStripCards,
                              static_cast<uint16_t>(nb::panel::height())))
    Serial.println("strip scratch alloc FAILED");

  // Exact cores / priorities / stacks from AGENTS.md. ESP-IDF's
  // xTaskCreatePinnedToCore takes the stack size in BYTES on this port.
  xTaskCreatePinnedToCore(task_render, "render",  8 * 1024, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(task_poll,   "poll",   12 * 1024, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(task_web,    "web",     8 * 1024, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(task_net,    "net",     4 * 1024, nullptr, 1, nullptr, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }

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
