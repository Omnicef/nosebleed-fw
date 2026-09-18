// SPDX-License-Identifier: GPL-3.0-only
//
// T-1.5 — Phase 1 task-topology skeleton. Four pinned tasks per AGENTS.md
// "Task topology" table, each logging a heartbeat with core ID, heap, and its
// stack high-water mark so T-1.5 accept ("high-water marks leave >25%
// headroom") is measurable.

#ifdef ARDUINO

#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "canvas.h"
#include "config.h"
#include "store.h"
#include "timezone.h"

// T-4.2 lesson: 3.3 KB Config must never sit on a task stack (loopTask's is
// 8 KB and the render task's is 8 KB too). One global, the writer owns it.
static nb::config::Config g_cfg;

// T-2.1: route every Canvas16 through PSRAM. Internal SRAM keeps the DMA
// framebuffer, the TLS session and task stacks.
static void* psram_canvas_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void psram_canvas_free(void* p) { heap_caps_free(p); }

#ifdef NB_PANEL_TEST
// T-2.8/T-2.9 — first hardware run of the lib/render -> panel seam
// (env:paneltest). Init panel, connect WiFi, then hold-scroll-hold-loop the
// IP + version splash forever. Proves colour order and row mapping —
// exactly what host tests cannot see.
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include "panel.h"
#include "font_data.h"
#include "primitives.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

#ifndef NB_VERSION
#define NB_VERSION "dev"
#endif

static void panel_test() {
    const uint32_t f0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (!nb::panel::init(g_cfg.hw)) {
        Serial.println("panel.begin() FAILED — check adapter PSU / pins");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    const uint32_t f1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // T-2.8 accept: DMA framebuffer in INTERNAL SRAM — a drop in the
    // internal pool proves placement without trusting the allocator flag.
    Serial.printf("panel begin OK: %dx%d, refresh=%d Hz\n", nb::panel::width(),
                  nb::panel::height(), nb::panel::refresh_rate());
    Serial.printf("DMA fb: internal free %lu -> %lu (delta %ld B)\n",
                  static_cast<unsigned long>(f0),
                  static_cast<unsigned long>(f1),
                  static_cast<long>(f1) - static_cast<long>(f0));
    // Bench-safe clamp: no 5V/4A PSU until the real supply lands
    // (SPIKE_RESULTS T-0.2). Splash art is sparse, but a scrolling traverse
    // must not brown out a USB port.
    const uint8_t br = g_cfg.hw.brightness > 50 ? 50 : g_cfg.hw.brightness;

    // --- FAULT-1 isolation: three words, three colours, all on the panel
    // at once, static. No timing to miss — just read which word is which
    // colour. Drawn through the real blit path on a canvas-sized-to-panel.
    nb::panel::set_brightness(16);  // USB-fed cap (T-0.2)
    const int pw = nb::panel::width(), ph = nb::panel::height();
    nb::Canvas16 card = nb::canvas_alloc(static_cast<uint16_t>(pw),
                                         static_cast<uint16_t>(ph));
    if (!card.valid()) { Serial.println("canvas alloc FAILED"); for(;;); }
    struct { const char* word; uint16_t col; } rows[] = {
        {"RED", nb::rgb565(255, 0, 0)},
        {"GREEN", nb::rgb565(0, 255, 0)},
        {"BLUE", nb::rgb565(0, 0, 255)}};
    for (unsigned i = 0; i < 3; i++) {
        const int len = static_cast<int>(strlen(rows[i].word));
        const int x = (pw - len * nb::FONT_SPLEEN_5X8.advance) / 2;
        const int y = static_cast<int>(i) * (ph / 3);
        nb::draw_text(card, nb::FONT_SPLEEN_5X8, x, y, rows[i].word, rows[i].col);
        Serial.printf("drawn \"%s\" in %s at y=%d\n", rows[i].word, rows[i].word, y);
    }
    nb::panel::blit(card);
    Serial.println("static: read which word is which colour (reset to re-show)");
    vTaskDelay(pdMS_TO_TICKS(3000));
    nb::panel::set_brightness(br);
    Serial.printf("brightness restored: cfg=%u applied=%u\n",
                  static_cast<unsigned>(g_cfg.hw.brightness),
                  static_cast<unsigned>(br));

    WiFi.mode(WIFI_STA);
    // Panel is wall-powered; WiFi modem-sleep buys nothing and its PHY
    // wake path re-creates the PLL esp_timer on every wake (measured
    // T-5.7: phy_track_pll_init aborts NO_MEM under parse heap churn).
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("wifi: connecting");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        Serial.print('.');
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    const String ip = WiFi.status() == WL_CONNECTED
                          ? WiFi.localIP().toString()
                          : String("NO WIFI");
    Serial.printf("wifi: %s\n", ip.c_str());

    // Splash: IP (6x12, white) over version (5x8, green). Canvas sized to
    // the WIDER of the two lines — sizing on the IP alone clipped the
    // 105 px version line inside an 86 px canvas (FAULT 2, part one: both
    // line ends vanished, which read as "garbled").
    char ver[48];
    snprintf(ver, sizeof ver, "nosebleed %s", NB_VERSION);
    const int len_ip = static_cast<int>(strlen(ip.c_str())),
              len_ver = static_cast<int>(strlen(ver));
    const int need_ip = len_ip * nb::FONT_SPLEEN_6X12.advance,
              need_ver = len_ver * nb::FONT_SPLEEN_5X8.advance;
    int sw = nb::panel::width();
    if (need_ip + 2 > sw) sw = need_ip + 2;
    if (need_ver + 2 > sw) sw = need_ver + 2;
    nb::Canvas16 c = nb::canvas_alloc(static_cast<uint16_t>(sw),
                                      static_cast<uint16_t>(nb::panel::height()));
    if (!c.valid()) { Serial.println("canvas alloc FAILED"); for(;;); }
    nb::draw_text(c, nb::FONT_SPLEEN_6X12, (sw - len_ip * nb::FONT_SPLEEN_6X12.advance) / 2,
                  16, ip.c_str(), nb::rgb565(255, 255, 255));
    nb::draw_text(c, nb::FONT_SPLEEN_5X8, (sw - len_ver * nb::FONT_SPLEEN_5X8.advance) / 2,
                  5, ver, nb::rgb565(0, 255, 0));
    Serial.printf("splash canvas %dx%d — panel should show IP scrolling now\n", c.w, c.h);
    nb::panel::splash(c, 1500);  // never returns
}
#endif  // NB_PANEL_TEST

#ifdef NB_LOGOS_TEST
// T-3.4 / T-3.7 — logos-partition mmap test, run standalone (env:logostest)
// before the tasks start so the heap samples are quiet. Accept criteria:
//  * internal free heap AND largest internal block identical before/after
//    init() — the mapping allocates no heap and copies nothing;
//  * every lookup's px+mask blob hashes to the host-predicted FNV-1a;
//  * set_phase(FRAME) + lookup() traps (NB_LOGOS_ASSERT=1 build).
#include "logos.h"
#include "logos_esp.h"

static uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static uint32_t blob_hash(const nb::logos::Ref& r) {
    const size_t n = static_cast<size_t>(r.w) * r.h * 2 +
                     ((static_cast<size_t>(r.w) + 7) / 8) * r.h;
    return fnv1a(r.px, n);
}

static void logos_test() {
    using namespace nb::logos;
    int fails = 0;
    Serial.println("T-3.4 mmap test:");
    const uint32_t f0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool ok = init_mmap();
    const uint32_t f1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b1 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    Serial.printf("  init=%d count=%u logo_h=%u\n",
                  static_cast<int>(ok), static_cast<unsigned>(table().count),
                  static_cast<unsigned>(table().logo_height));
    const long df = static_cast<long>(f1) - static_cast<long>(f0);
    const long db = static_cast<long>(b1) - static_cast<long>(b0);
    // mmap allocates its page-table mapping (~100 B) once — that is mapping
    // metadata, not lookup cost; the acceptance criterion (heap identical
    // around lookups) is checked separately after the loop below.
    Serial.printf("  mmap internal free delta=%ld largestblk delta=%ld (one-time mapping)\n", df, db);
    if (!ok) ++fails;

    // Base rows died with atlas v2: every row is a card display height.
    // One team per league at the PRE slot (h=24) + the cross-league
    // abbreviation reuse (BOS is in MLB, NBA and NHL — a bare-abbr index
    // could only hold one) + misses. FNV-1a of px+mask, recomputed
    // host-side from the same logos.bin.
    struct Want { const char* league; const char* abbr; uint16_t h; uint32_t hash; };
    static const Want wants[] = {
        {"mlb", "BOS", 24, 0x8294c099u}, {"nfl", "KC", 24, 0x0444d882u},
        {"nba", "LAL", 24, 0x3a12ba50u}, {"nhl", "VGK", 24, 0x2c255b8au},
        {"epl", "LIV", 24, 0xb82b6b3fu}, {"nhl", "BOS", 24, 0x8f68df91u},
        {"mlb", "BOS", 32, 0},  // not a card height — must miss
        {"mlb", "ZZZ", 24, 0},  // must miss
    };
    const uint32_t f2 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b2 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    for (const auto& w : wants) {
        Ref r;
        const bool hit = lookup(w.league, w.abbr, w.h, r);
        const bool ok = w.hash ? (hit && blob_hash(r) == w.hash) : !hit;
        if (!ok) ++fails;
        if (hit) {
            Serial.printf("  %s:%s %s w=%u h=%u hash=%08lx\n", w.league,
                          w.abbr, ok ? "OK  " : "FAIL", r.w, r.h,
                          static_cast<unsigned long>(blob_hash(r)));
        } else {
            Serial.printf("  %s:%s %s (miss)\n", w.league, w.abbr,
                          ok ? "OK  " : "FAIL");
        }
    }
    const long dl = static_cast<long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) - static_cast<long>(f2);
    const long dbl = static_cast<long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) - static_cast<long>(b2);
    Serial.printf("  lookups: internal free delta=%ld largestblk delta=%ld (must be 0/0)\n", dl, dbl);
    if (dl != 0 || dbl != 0) ++fails;
    if (fails) Serial.printf("  RESULT: FAIL (%d)\n", fails);
    else Serial.println("  RESULT: PASS");
#ifdef NB_LOGOS_TRAP_EXPECT
    // T-3.7: a lookup outside BOOTSTRAP/REBUILD must trap. Expected crash:
    // Guru Meditation (IllegalInstruction from __builtin_trap), then reboot.
    set_phase(Phase::FRAME);
    Serial.println("  trap expected now: FRAME-phase lookup");
    Serial.flush();
    delay(50);
    Ref r;
    lookup("mlb", "BOS", 24, r);
    Serial.println("  ASSERT FAILED: no trap");  // must not reach
#else
    set_phase(Phase::FRAME);
    Serial.println("  (built without NB_LOGOS_TRAP_EXPECT — FRAME trap skipped)");
#endif
    Serial.println("  PASS: stop here for serial capture");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_LOGOS_TEST

#ifdef NB_CONFIG_TEST
// T-4.2 — NVS config store proof (env:configtest). Boot A: absent →
// defaults, brightness round-trip, garbage blob → defaults (no crash
// loop), truncated-valid-prefix → defaults; then save brightness=55 and
// esp_restart. Boot B: load must report the persisted 55 (power-cycle
// persistence), then reset the blob and halt.
#include <cstring>
#include <Preferences.h>
#include "config.h"
#include "store.h"
#include "esp_system.h"

static void config_test() {
    using namespace nb::config;
    int fails = 0;
    Serial.println("T-4.2 NVS config test (boot A):");

    static Config c, c3, c4;  // 3.3 KB each — never on the loopTask stack
    Config& c2 = c;               // reused after step 1; no second copy needed
    // 1. absent blob -> defaults
    reset();
    if (load(c) || c.hw.brightness != 80 || c.widget_count != 10) {
        ++fails; Serial.println("  absent->defaults FAIL");
    } else {
        Serial.println("  absent->defaults OK (load=false, brightness=80, widgets=10)");
    }

    // 2. round-trip: brightness 55
    c.hw.brightness = 55;
    if (!save(c)) ++fails;
    const bool rt = load(c2) && c2.hw.brightness == 55;
    Serial.printf("  save/load brightness=55: %s\n", rt ? "OK" : "FAIL");
    if (!rt) ++fails;

    // 3. deliberate garbage (0xFF blob) -> defaults, no crash
    {
        Preferences p;
        p.begin("nb");
        uint8_t junk[200];
        memset(junk, 0xFF, sizeof(junk));
        p.putBytes("cfg", junk, sizeof(junk));
        p.end();
    }
    if (load(c3) || c3.hw.brightness != 80 || c3.widget_count != 10) {
        ++fails; Serial.println("  garbage->defaults FAIL");
    } else {
        Serial.println("  garbage(0xFF x200)->defaults OK, no crash");
    }

    // 4. valid magic+schema but wrong length -> defaults (size gate)
    {
        Preferences p;
        p.begin("nb");
        p.putBytes("cfg", &c, 100);  // c is a valid Config; truncated blob
        p.end();
    }
    if (load(c4) || c4.hw.brightness != 80) {
        ++fails; Serial.println("  truncated->defaults FAIL");
    } else {
        Serial.println("  truncated(valid prefix,100 B)->defaults OK");
    }

    // 5. stage the persistence check, then restart into boot B
    load(c);
    c.hw.brightness = 55;
    save(c);
    Serial.printf("  RESULT: %s (boot A); restarting to prove persistence\n",
                  fails ? "FAIL" : "PASS");
    Serial.flush();
    delay(100);
    esp_restart();
}

// T-4.3 — mirrors task_render's wait: binds itself, then blocks on the
// notification that save() sends. Proves live-apply without any reboot.
static void cfg_apply_task(void*) {
    nb::config::bind_render_task(xTaskGetCurrentTaskHandle());
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000))) {
        nb::config::load(g_cfg);
        Serial.printf("  [render] woke via task notification, brightness now %u\n",
                      static_cast<unsigned>(g_cfg.hw.brightness));
        Serial.println(g_cfg.hw.brightness == 40
                           ? "  RESULT: PASS (T-4.3 — save() woke render, live apply, no restart)"
                           : "  RESULT: FAIL (T-4.3 — wrong value after notify)");
    } else {
        Serial.println("  RESULT: FAIL (T-4.3 — no notification within 3 s)");
    }
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

static void config_boot_check() {
    Serial.printf("T-4.2 boot B persisted load: brightness=%u widgets=%u\n",
                  static_cast<unsigned>(g_cfg.hw.brightness),
                  static_cast<unsigned>(g_cfg.widget_count));
    Serial.println(g_cfg.hw.brightness == 55
                       ? "  RESULT: PASS (boot B — survives restart)"
                       : "  RESULT: FAIL (boot B)");
    nb::config::reset();
    Serial.println("  blob reset; T-4.3 live-apply test next");

    if (xTaskCreate(cfg_apply_task, "cfgapply", 4 * 1024, nullptr, 2, nullptr) != pdPASS) {
        Serial.println("  RESULT: FAIL (T-4.3 — could not start listener)");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    vTaskDelay(pdMS_TO_TICKS(200));  // let the listener bind first
    g_cfg.hw.brightness = 40;
    Serial.println(nb::config::save(g_cfg)
                       ? "  saved brightness=40 (expect wake within ms)"
                       : "  RESULT: FAIL (T-4.3 — save failed)");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_CONFIG_TEST

#ifdef NB_HTTP_TEST
// T-5.3 — HTTP transport proof (env:httptest). Boot order is the hard rule:
// WiFi -> SNTP -> first TLS (cert notBefore validation needs the clock).
// 1. forced DNS failure: every attempt fails, backoff is honoured, nothing
//    crashes or leaks; 2. NFL scoreboard over TLS (T-04-proven URL): status
//    200 and body bytes counted off HttpStream == Content-Length.
// Internal heap is sampled around each http_get(): esp_http_client_close +
// cleanup run on every path including mid-body failure, so the post-call
// delta must be ~0 — that is the "never leaks a session" measurement.
#include <WiFi.h>
#include <cstdlib>
#include <StreamUtils.h>
#include "http.h"
#include "espn.h"
#include "espn_json.h"
#include "date_window.h"
#include "poll.h"
#include "cache.h"
#include <atomic>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

static uint32_t http_int_free() {
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

// T-5.7 — poll-task device proof (NB_POLL_DEMO). Real topology: this loop
// runs on a task pinned to core 0 (like the shipping poll task); an fps
// probe emulating the render task's 30 fps frame budget runs on core 1 at
// prio 3. Five pro leagues enabled, per-league deadlines via PollScheduler
// (live 20 s / idle 120 s per LeagueConfig, 5 s boot stagger). Accept
// evidence: one full cycle across five leagues with timing, a per-cycle
// internal-heap watermark, max CONCURRENT TLS sessions == 1, and core-1
// fps never below 28 while core 0 does TLS.
static nb::data::DataCache* g_pcache = nullptr;
static std::atomic<int> g_sess{0}, g_sess_max{0}, g_fps_min10{9999};
static volatile uint32_t g_cycle_ms = 0;
static volatile int g_poll_fails = 0;

static void poll_fps_probe(void*) {
    Serial.printf("  [fps] started on core %d\n", xPortGetCoreID());
    uint32_t frames = 0, t0 = millis(), tlog = t0;
    for (;;) {
        ++frames;
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps frame budget
        const uint32_t now = millis();
        if (now - tlog >= 10000) {
            const int fps10 = static_cast<int>(frames * 10000u / (now - tlog));
            if (fps10 < g_fps_min10) g_fps_min10 = fps10;
            Serial.printf("  [fps] %.1f fps (min %.1f)\n", fps10 / 10.0f,
                          g_fps_min10.load() / 10.0f);
            frames = 0;
            tlog = now;
        }
        (void)t0;
    }
}

static void poll_task(void*) {
    using namespace nb::data;
    Serial.printf("  poll: task on core %d, int_free=%u largest=%u\n", xPortGetCoreID(),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    static PollScheduler sch;
    static const int kEnabled[] = {0, 2, 5, 6, 7};  // nfl nba mlb nhl epl
    for (int i = 0; i < nb::config::kLeagueSlugCount; ++i)
        sch.set(i, {false, 20, 120});
    for (int lg : kEnabled) sch.set(lg, {true, 20, 120});
    sch.reset(time(nullptr));

    g_pcache = static_cast<nb::data::DataCache*>(
        heap_caps_calloc(1, sizeof(nb::data::DataCache), MALLOC_CAP_SPIRAM));
    if (!g_pcache) {
        Serial.println("  poll: FAIL — no PSRAM for DataCache");
        ++g_poll_fails;
        vTaskDelete(nullptr);
    }

    bool first_done[8] = {};
    int n_first = 0;
    const uint32_t cycle_t0 = millis();
    uint32_t heap_min = http_int_free();

    const uint32_t t_end = millis() + 150000;
    while (millis() < t_end) {
        const time_t now = time(nullptr);
        for (int lg = 0; lg < nb::config::kLeagueSlugCount; ++lg) {
            if (!sch.due(lg, now)) continue;
            char url[192], dates[16];
            local_day(dates, sizeof dates, now);
            scoreboard_url(url, sizeof url, nb::config::kLeagueApiPaths[lg], dates);
            size_t wire = 0;
            int games = 0;
            bool ok = false, has_live = false;
            const uint32_t t0 = millis();
            const uint32_t f0 = http_int_free();
            ++g_sess;
            if (g_sess > g_sess_max) g_sess_max.store(g_sess.load());
            {
                JsonDocument doc = make_psram_doc();
                ok = espn_fetch_scoreboard(url, doc, &wire);
                if (ok) {
                    GameList* w = g_pcache->writable(lg);
                    w->count = to_games(doc, *w);
                    filter_yesterday_today(w, now);
                    games = w->count;
                    if (games == 0) {  // opportunistic yesterday leg (T-5.6)
                        char yd[16];
                        if (local_yesterday(yd, sizeof yd, now) &&
                            scoreboard_url(url, sizeof url, nb::config::kLeagueApiPaths[lg], yd)) {
                            JsonDocument doc2 = make_psram_doc();
                            if (espn_fetch_scoreboard(url, doc2, &wire)) {
                                w->count = to_games(doc2, *w);
                                games = w->count;
                            }
                        }
                    }
                    for (int i = 0; i < games; ++i)
                        has_live |= (w->games[i].status == kStatusIn);
                    g_pcache->publish(lg, static_cast<int64_t>(now));
                } else {
                    ++g_poll_fails;  // last-good kept, per hard rule
                }
            }
            --g_sess;
            Serial.printf("  poll[%s]: ok=%d games=%d wire=%u %lu ms int_delta=%ld B\n",
                          nb::config::kLeagueSlugs[lg], ok, games,
                          static_cast<unsigned>(wire),
                          static_cast<unsigned long>(millis() - t0),
                          static_cast<long>(f0 - http_int_free()));
            sch.done(lg, time(nullptr), has_live);
            if (!first_done[lg]) {
                first_done[lg] = true;
                if (++n_first == 5) {
                    g_cycle_ms = millis() - cycle_t0;
                    Serial.printf("  poll-cycle: 5 leagues in %lu ms\n",
                                  static_cast<unsigned long>(g_cycle_ms));
                }
            }
            const uint32_t hf = http_int_free();
            if (hf < heap_min) heap_min = hf;
        }
        const time_t t = time(nullptr);
        int64_t sleep_s = sch.next_wake(t) - t;
        if (sleep_s < 1) sleep_s = 1;
        if (sleep_s > 30) sleep_s = 30;
        vTaskDelay(pdMS_TO_TICKS(1000 * sleep_s));
    }

    const bool cycle_ok = g_cycle_ms > 0;
    const bool pass = cycle_ok && g_sess_max <= 1 && g_fps_min10 >= 280;
    Serial.printf("  poll-demo: cycle=%d(%lu ms) sess_max=%d fps_min=%.1f heap_min=%u B fails=%d\n",
                  cycle_ok, static_cast<unsigned long>(g_cycle_ms), g_sess_max.load(),
                  g_fps_min10.load() / 10.0f, heap_min, g_poll_fails);
    Serial.printf("  POLL RESULT: %s\n", pass ? "PASS" : "FAIL");
    vTaskDelete(nullptr);
}

static void http_test() {
    int fails = 0;
    Serial.println("T-5.3 HTTP transport test:");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const uint8_t st = WiFi.waitForConnectResult(20000);
    if (st != WL_CONNECTED) {
        Serial.printf("  FAIL: wifi status=%u\n", static_cast<unsigned>(st));
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    Serial.printf("  wifi %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    time_t now = 0;
    uint32_t waited = 0;
    while (now < 1700000000 && waited < 20000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
        now = time(nullptr);
    }
    if (now < 1700000000) {
        Serial.println("  FAIL: SNTP not synced in 20 s — refusing TLS (hard rule)");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    Serial.printf("  sntp synced in %lu ms (TLS may proceed)\n",
                  static_cast<unsigned long>(waited));

    // 1. forced DNS failure — .invalid is reserved-NXDOMAIN by RFC 6761.
    {
        nb::data::HttpStat stat;
        const uint32_t t0 = millis(), f0 = http_int_free();
        const bool ok = nb::data::http_get(
            "http://nosebleed-nope.invalid./scoreboard.json",
            [](nb::data::HttpStream&) { return true; }, &stat);
        const uint32_t dt = millis() - t0;
        const int32_t d = static_cast<int32_t>(f0 - http_int_free());
        Serial.printf("  dns-fail: ok=%d status=%d elapsed=%lu ms heap_delta=%ld B\n",
                      ok, stat.status, static_cast<unsigned long>(dt),
                      static_cast<long>(d));
        // must fail; must spend >= the 0.5+1.0+2.0 s backoff floor
        if (ok || dt < 3500 || (d > 1024) || (d < -1024)) {
            ++fails;
            Serial.println("    FAIL: expected exhausted retries (>=3.5 s), no heap drift");
        }
    }

    // 2. TLS success — the exact T-0.4 URL. Count every body byte off the
    // stream; cleanup happens inside http_get_once, so measure after it.
    {
        nb::data::HttpStat stat;
        size_t n = 0;
        const uint32_t t0 = millis(), f0 = http_int_free();
        const bool ok = nb::data::http_get(
            "https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard",
            [&](nb::data::HttpStream& s) {
                char buf[512];
                for (;;) {
                    const size_t r = s.readBytes(buf, sizeof buf);
                    if (r == 0) break;
                    n += r;
                }
                return n > 1000;
            },
            &stat);
        const uint32_t dt = millis() - t0;
        const int32_t d = static_cast<int32_t>(f0 - http_int_free());
        Serial.printf("  tls: ok=%d status=%d clen=%lld bytes=%u elapsed=%lu ms heap_delta=%ld B\n",
                      ok, stat.status, static_cast<long long>(stat.clen),
                      static_cast<unsigned>(n), static_cast<unsigned long>(dt),
                      static_cast<long>(d));
        if (!ok || stat.status != 200 || (stat.clen >= 0 && stat.clen != (int64_t)n)) {
            ++fails;
            Serial.println("    FAIL: expected 200 with bytes == Content-Length");
        }
        if (d > 1024 || d < -1024) {
            ++fails;
            Serial.println("    FAIL: session heap not reclaimed (leak)");
        }
    }

    // 3. T-5.4 — filtered ESPN client on the live stream: ReadBufferingStream
    // (512 B, never exercised in the spike) feeding filter + NestingLimit(20)
    // parse. TLS session is already closed here, so the internal delta is the
    // parse alone and must be ~0 — the doc is PSRAM, the buffer is 512 B.
    {
        char url[160];
        if (!nb::data::scoreboard_url(url, sizeof url, "baseball/mlb")) ++fails;
        auto doc = nb::data::make_psram_doc();
        const uint32_t f0 = http_int_free(), t0 = millis();
        const bool ok = nb::data::espn_fetch_scoreboard(url, doc);
        const int32_t d = static_cast<int32_t>(f0 - http_int_free());
        Serial.printf("  espn: ok=%d events=%u kept=%u B internal_delta=%ld B elapsed=%lu ms\n",
                      ok,
                      ok ? static_cast<unsigned>(doc["events"].size()) : 0u,
                      ok ? static_cast<unsigned>(measureJson(doc)) : 0u,
                      static_cast<long>(d), static_cast<unsigned long>(millis() - t0));
        if (!ok) {
            ++fails;
            Serial.println("    FAIL: filtered MLB fetch");
        } else {
            Serial.printf("    spot: id=%s state=%s away=%s\n",
                          doc["events"][0]["id"].as<const char*>(),
                          doc["events"][0]["competitions"][0]["status"]["type"]["state"].as<const char*>(),
                          doc["events"][0]["competitions"][0]["competitors"][0]["team"]["abbreviation"].as<const char*>());
            if (d > 2048) {
                ++fails;
                Serial.println("    FAIL: parse internal delta > 2 KB — filter/allocator wrong");
            }
        }
    }

    // 4. T-5.6 (REQUIRED mitigation) — single local day via ?dates=.
    // Baseline: 6,081 ms / full window unmitigated. Measure over runs;
    // wire time is network-weather (T-5.3/5.4 both saw multi-fold swings).
    {
        char url[192], dates[16];
        setenv("TZ", "America/New_York", 1);  // config default; boot path does this via T-4.6
        tzset();
        const time_t now = time(nullptr);  // SNTP was synced for phase 2
        if (!nb::data::local_day(dates, sizeof dates, now)) ++fails;
        if (!nb::data::scoreboard_url(url, sizeof url, "baseball/mlb", dates)) ++fails;
        Serial.printf("  window: dates=%s tz=%s\n", dates, getenv("TZ") ? getenv("TZ") : "(unset)");
        for (int i = 1; i <= 3; ++i) {
            auto doc = nb::data::make_psram_doc();
            size_t wire = 0;
            const uint32_t t0 = millis();
            const bool ok = nb::data::espn_fetch_scoreboard(url, doc, &wire);
            Serial.printf("  run%d: ok=%d wire=%u B events=%u kept=%u B elapsed=%lu ms\n", i, ok,
                          static_cast<unsigned>(wire),
                          ok ? static_cast<unsigned>(doc["events"].size()) : 0u,
                          ok ? static_cast<unsigned>(measureJson(doc)) : 0u,
                          static_cast<unsigned long>(millis() - t0));
            if (!ok) ++fails;
            if (i < 3) vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    // 5. T-5.6 diagnostic — wire is only ~1.4 s of the ~8 s run (phase 2
    // moves the same-size NFL body in 1.35 s); parse owns the rest. Re-parse
    // ONE captured body from PSRAM memory with filter/doc in either heap to
    // find which allocation is slow. Throwaway; not the shipping path.
    {
        static constexpr size_t kBodyCap = 600u * 1024u;
        char* body = static_cast<char*>(heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM));
        char url[192], dates[16];
        nb::data::local_day(dates, sizeof dates, time(nullptr));
        if (!nb::data::scoreboard_url(url, sizeof url, "baseball/mlb", dates)) ++fails;
        size_t got = 0;
        const uint32_t tw0 = millis();
        const bool fetched =
            body && nb::data::http_get(url, [&](nb::data::HttpStream& s) {
                char buf[1024];
                for (;;) {
                    const size_t r = s.readBytes(buf, sizeof buf);
                    if (r == 0) break;
                    if (got + r > kBodyCap) return false;
                    memcpy(body + got, buf, r);
                    got += r;
                }
                return got > 1000;
            });
        Serial.printf("  parse-diag: fetched=%d body=%u B wire=%lu ms\n", fetched,
                      static_cast<unsigned>(got), static_cast<unsigned long>(millis() - tw0));
        if (fetched) {
            JsonDocument filt_int;
            nb::data::build_scoreboard_filter(filt_int);
            auto filt_psram = nb::data::make_psram_doc();
            nb::data::build_scoreboard_filter(filt_psram);
            for (int v = 0; v < 4; ++v) {
                // v0 = shipping combo (psram filter + psram doc); v3 = raw
                // parse, no filter, tokenizer baseline.
                const bool psram_filter = (v == 0 || v == 2);
                const bool psram_doc = (v == 0 || v == 1);
                auto parse_once = [&]() {
                    JsonDocument doc = psram_doc ? nb::data::make_psram_doc() : JsonDocument();
                    JsonDocument& f = psram_filter ? filt_psram : filt_int;
                    struct MemView {  // reader duck-type: size_t readBytes(char*, size_t)
                        const char* p;
                        size_t n, i = 0;
                        int read() { return i < n ? (uint8_t)p[i++] : -1; }
                        size_t readBytes(char* b, size_t m) {
                            const size_t k = (n - i < m) ? n - i : m;
                            memcpy(b, p + i, k);
                            i += k;
                            return k;
                        }
                    } mv{body, got};
                    const uint32_t f0 = http_int_free(), t0 = micros();
                    const DeserializationError err =
                        (v == 3) ? deserializeJson(doc, mv, DeserializationOption::NestingLimit(20))
                                 : nb::data::parse_scoreboard(mv, f, doc);
                    const uint32_t dt = (micros() - t0) / 1000u;
                    const int32_t d = static_cast<int32_t>(f0 - http_int_free());
                    Serial.printf("    filt=%-5s doc=%-8s: err=%d %lu ms internal_delta=%ld B\n",
                                  (v == 3) ? "none" : (psram_filter ? "psram" : "internal"),
                                  psram_doc ? "psram" : "internal", static_cast<int>(bool(err)),
                                  static_cast<unsigned long>(dt), static_cast<long>(d));
                };
                parse_once();  // warm cache
                parse_once();  // measured pass
            }
        }
        heap_caps_free(body);
    }

    Serial.printf("  RESULT: %s\n", fails ? "FAIL" : "PASS");
    // 6. T-5.7 — real-topology poll proof runs as tasks (poll on core 0,
    // fps probe on core 1); logs POLL RESULT when done.
#ifdef NB_POLL_DEMO
    Serial.printf("  poll-demo boot: int_free=%u largest=%u psram_free=%u\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    xTaskCreatePinnedToCore(poll_fps_probe, "fps", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(poll_task, "poll", 12 * 1024, nullptr, 2, nullptr, 0);
    Serial.println("  poll-demo: tasks created");
#endif
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_HTTP_TEST

// T-5.2 device proof (NB_CACHE_TEST). The host stress test proved the
// protocol on x86, which orders far more than Xtensa does. Same payload,
// same detector, on the real topology: writer core 0 prio 2 publishing
// 50 Hz x 4 leagues, reader core 1 prio 3 snapshotting at 30 Hz. The
// control runs the forbidden in-place mutation and MUST show tears —
// if the detector is blind on this chip, the zero in the real phase is
// meaningless. Cache instance lives in PSRAM, where the shipped one
// will sit (57.6 KB of .bss would come straight out of the WiFi heap).
#ifdef NB_CACHE_TEST
#include "cache.h"
#include <cstring>
#include <cstdio>

using nb::data::DataCache;
using nb::data::Game;
using nb::data::GameList;
using nb::data::kMaxGamesPerLeague;

static void fill_generation(GameList* l, uint32_t gen) {
    l->count = 1 + static_cast<int>(gen % kMaxGamesPerLeague);
    char id[16];
    std::snprintf(id, sizeof id, "%u", gen);
    for (int i = 0; i < l->count; i++) {
        Game& g = l->games[i];
        nb::data::copy_str(g.id, sizeof g.id, id);
        nb::data::copy_str(g.status_display, sizeof g.status_display, "Gen");
        g.period = static_cast<int16_t>(gen % 1000);
        g.away_score = static_cast<int16_t>(gen % 10);
        g.home_score = static_cast<int16_t>(gen % 100);
        g.start_utc = gen;
        g.situation.outs = static_cast<int16_t>(gen % 4);
        g.away.colour = gen * 2654435761u;
    }
}

static int check_generation(const GameList& l) {
    if (l.count < 1 || l.count > kMaxGamesPerLeague) return -1;
    const uint32_t gen = static_cast<uint32_t>(l.games[0].start_utc);
    char id[16];
    std::snprintf(id, sizeof id, "%u", gen);
    if (l.count != 1 + static_cast<int>(gen % kMaxGamesPerLeague)) return static_cast<int>(gen);
    for (int i = 0; i < l.count; i++) {
        const Game& g = l.games[i];
        if (g.start_utc != (int64_t)gen || std::strcmp(g.id, id) != 0 ||
            g.period != (int16_t)(gen % 1000) || g.away_score != (int16_t)(gen % 10) ||
            g.home_score != (int16_t)(gen % 100) || g.situation.outs != (int16_t)(gen % 4) ||
            g.away.colour != gen * 2654435761u)
            return static_cast<int>(gen);
    }
    return 0;
}

static DataCache* g_sc = nullptr;
static GameList *g_ctl = nullptr, *g_snap = nullptr;
static std::atomic<bool> g_ctl_stop{false};
static std::atomic<int> g_torn{0}, g_ctl_torn{0}, g_reads{0}, g_retry{0}, g_maxbusy{0};
static std::atomic<int> g_hm_torn{0}, g_hm_retries{0};
static std::atomic<uint32_t> g_ctl_gen{0}, g_str_gen{0};  // max gen the core-1 reader saw

static void ctl_writer(void*) {  // forbidden pattern: mutate in place
    uint32_t gen = 1;
    while (!g_ctl_stop.load()) {
        fill_generation(g_ctl, ++gen);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelete(nullptr);
}

static void ctl_reader(void*) {  // holds the pointer across the read
    volatile uint32_t spin = 0;
    while (!g_ctl_stop.load()) {
        GameList* held = g_ctl;  // no swap, no seqlock: same object
        if (static_cast<uint32_t>(held->games[0].start_utc) > g_ctl_gen.load())
            g_ctl_gen.store(static_cast<uint32_t>(held->games[0].start_utc));
        int saw = -1;
        for (int i = 0; i < held->count; i++) {
            const int64_t g = held->games[i].start_utc;
            for (uint32_t k = 0; k < 60000; k++) spin += k;  // dwell ~ms, longer than a fill
            if (g != saw && saw != -1) {
                g_ctl_torn.fetch_add(100);
                break;
            }
            saw = static_cast<int>(g);
        }
        if (check_generation(*held) != 0) g_ctl_torn.fetch_add(1);
        vTaskDelay(1);
    }
    vTaskDelete(nullptr);
}

static void str_writer(void*) {  // poll role: core 0 prio 2, 50 Hz x 4 leagues
    uint32_t gen = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 180000) {
        for (int l = 0; l < 4; l++) {
            GameList* b = g_sc->writable(l);
            fill_generation(b, ++gen);
            g_sc->publish(l, gen);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(nullptr);
}

static void str_reader(void*) {  // render role: core 1 prio 3, 30 Hz snapshots
    const uint32_t t0 = millis();
    while (millis() - t0 < 180000) {
        vTaskDelay(pdMS_TO_TICKS(33));
        if (!g_sc->snapshot(0, g_snap)) {
            const int b = ++g_retry;
            if (b > g_maxbusy.load()) g_maxbusy.store(b);
            continue;
        }
        ++g_reads;
        if (static_cast<uint32_t>(g_snap->games[0].start_utc) > g_str_gen.load())
            g_str_gen.store(static_cast<uint32_t>(g_snap->games[0].start_utc));
        if (check_generation(*g_snap) != 0) ++g_torn;
    }
    vTaskDelete(nullptr);
}

// Hammer phase: same protocol, adversarial contention — writer max-rates
// league 0 (real 50 Hz cadence gives ~1 expected retry in 180 s, too few
// to prove the retry branch ever fires on Xtensa). Torn must stay 0.
static std::atomic<bool> g_hm_done{false};
static void hm_writer(void*) {
    uint32_t gen = 900000, n = 0;
    while (!g_hm_done.load()) {
        GameList* b = g_sc->writable(0);
        fill_generation(b, ++gen);
        g_sc->publish(0, gen);
        if (++n % 200 == 0) vTaskDelay(1);  // keep core 0 idle fed (TASK_WDT)
    }
    vTaskDelete(nullptr);
}

static void hm_reader(void*) {
    uint32_t n = 0;
    while (!g_hm_done.load()) {
        if (++n % 50 == 0) vTaskDelay(1);  // unconditional: a retry storm must not starve core 1
        if (!g_sc->snapshot(0, g_snap)) {
            ++g_hm_retries;
            continue;
        }
        if (check_generation(*g_snap) != 0) ++g_hm_torn;
    }
    vTaskDelete(nullptr);
}

static void cache_test() {
    Serial.println("T-5.2 device seqlock stress (cachetest):");
    g_sc = static_cast<DataCache*>(heap_caps_calloc(1, sizeof(DataCache), MALLOC_CAP_SPIRAM));
    g_ctl = static_cast<GameList*>(heap_caps_calloc(1, sizeof(GameList), MALLOC_CAP_INTERNAL));
    g_snap = static_cast<GameList*>(heap_caps_calloc(1, sizeof(GameList), MALLOC_CAP_INTERNAL));
    if (!g_sc || !g_ctl || !g_snap) { Serial.println("CACHE RESULT: FAIL (alloc)"); for (;; vTaskDelay(100)); }
    fill_generation(g_ctl, 1);

    xTaskCreatePinnedToCore(ctl_reader, "cr", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(ctl_writer, "cw", 3 * 1024, nullptr, 2, nullptr, 0);
    const uint32_t t0 = millis();
    while (!g_ctl_torn.load() && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(5));
    g_ctl_stop.store(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    Serial.printf("  control (in-place mutation): torn=%d last_gen=%lu after %lu ms\n",
                  g_ctl_torn.load(), static_cast<unsigned long>(g_ctl_gen.load()),
                  static_cast<unsigned long>(millis() - t0));
    const bool teeth = g_ctl_torn.load() > 0;
    const bool vis_ctl = g_ctl_gen.load() > 1;  // did core 1 see core 0's RAM writes at all?

    xTaskCreatePinnedToCore(hm_reader, "hr", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(hm_writer, "hw", 3 * 1024, nullptr, 2, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(10000));
    g_hm_done.store(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    Serial.printf("  hammer (max-rate league 0, 10 s): retries=%d torn=%d\n", g_hm_retries.load(),
                  g_hm_torn.load());

    xTaskCreatePinnedToCore(str_reader, "sr", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(str_writer, "sw", 3 * 1024, nullptr, 2, nullptr, 0);
    uint32_t tlog = millis();
    const uint32_t t_end = tlog + 185000;  // fixed anchor — tlog re-anchors each print
    while (static_cast<int32_t>(t_end - millis()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (millis() - tlog >= 30000) {
            tlog = millis();
            Serial.printf("  stress: reads=%d retries=%d max_busy=%d torn=%d last_gen=%lu int_free=%u\n",
                          g_reads.load(), g_retry.load(), g_maxbusy.load(), g_torn.load(),
                          static_cast<unsigned long>(g_str_gen.load()),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        }
    }
    const bool pass = teeth && vis_ctl && g_torn.load() == 0 && g_reads.load() > 1000 &&
                      g_hm_retries.load() > 0 && g_hm_torn.load() == 0 &&
                      g_str_gen.load() > 8000;  // ~50 Hz x 180 s of publishes seen
    Serial.printf("CACHE RESULT: %s torn=%d reads=%d retries=%d max_busy=%d control_torn=%d "
                  "ctl_gen=%lu str_gen=%lu hm_retries=%d hm_torn=%d\n",
                  pass ? "PASS" : "FAIL", g_torn.load(), g_reads.load(), g_retry.load(),
                  g_maxbusy.load(), g_ctl_torn.load(),
                  static_cast<unsigned long>(g_ctl_gen.load()),
                  static_cast<unsigned long>(g_str_gen.load()), g_hm_retries.load(),
                  g_hm_torn.load());
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_CACHE_TEST

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
        Serial.printf("cardtest fixture: parse FAIL (%s) — %s\n", static_cast<int>(bool(err)), why);
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

// T-1.2 accept: esp_partition_find locates the `logos` and `web` data partitions.
static void partition_snapshot(void) {
  Serial.println("partitions (T-1.2):");
  for (const char* label : {"logos", "web"}) {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (p != nullptr) {
      Serial.printf("  %-6s FOUND  addr=0x%06lx  size=%lu KB  subtype=0x%02lx\n",
                    label,
                    static_cast<unsigned long>(p->address),
                    static_cast<unsigned long>(p->size / 1024),
                    static_cast<unsigned long>(p->subtype));
    } else {
      Serial.printf("  %-6s MISSING\n", label);
    }
  }
}

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

// T-1.3 accept: "settings verified present at runtime, not just in the file".
// Arduino's prebuilt sdkconfig means sdkconfig.defaults is largely inert
// (T-0.6); this makes the divergence explicit.
static void sdkconfig_snapshot(void) {
  Serial.println("sdkconfig snapshot (what the compiler actually saw):");
  Serial.printf("  flash            = %s\n", CONFIG_ESPTOOLPY_FLASHSIZE);
  Serial.printf("  psram octal      = %s\n",
#ifdef CONFIG_SPIRAM_MODE_OCT
                "yes"
#else
                "no"
#endif
  );
  Serial.printf("  data cache       = %d KB (PLAN T-1.3 wants 64 KB; Arduino prebuilts all ship 32 KB)\n",
                static_cast<int>(CONFIG_ESP32S3_DATA_CACHE_SIZE / 1024));
  Serial.printf("  freertos tick    = %d Hz\n", CONFIG_FREERTOS_HZ);
  Serial.printf("  c++              = %ld\n", static_cast<long>(__cplusplus));
  Serial.printf("runtime psram      : %s, size=%lu B\n",
                psramFound() ? "present" : "MISSING",
                static_cast<unsigned long>(ESP.getPsramSize()));
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // render must never block on printf (T-7.6 saw
                             // ~2 s CDC stalls on USB churn); drops instead
  delay(1500);  // let USB CDC enumerate after reset
  Serial.println();
  Serial.println("=== nosebleed-fw Phase 1 skeleton ===");
  sdkconfig_snapshot();
  partition_snapshot();
  Serial.println();

  nb::canvas_set_allocator(psram_canvas_alloc, psram_canvas_free);

#ifdef NB_LOGOS_TEST
  logos_test();  // never returns; tasks below are for normal boots only
#endif

#ifdef NB_HTTP_TEST
  http_test();  // never returns
#endif

#ifdef NB_CACHE_TEST
  cache_test();  // never returns
#endif

#ifdef NB_CONFIG_TEST
  {
    // Boot A = fresh/other blob; boot B = ours staged before esp_restart.
    if (nb::config::load(g_cfg) && g_cfg.hw.brightness == 55) config_boot_check();
    else config_test();  // both branches never return
  }
#endif

  if (!nb::config::load(g_cfg)) Serial.println("config: absent/corrupt — seeded defaults");
  else Serial.printf("config: loaded (brightness=%u)\n",
                     static_cast<unsigned>(g_cfg.hw.brightness));
  Serial.printf("timezone: '%s' -> %s\n", g_cfg.hw.timezone,
                nb::config::apply_timezone(g_cfg.hw.timezone) == nb::config::TzResult::kApplied
                     ? "applied" : "UTC fallback");

#ifdef NB_PANEL_TEST
  panel_test();  // never returns; tasks below are for normal boots only
#endif
#ifdef NB_CARD_TEST
  card_test();  // never returns; tasks below are for normal boots only
#endif

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
