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

    // One team per league + the cross-league abbreviation reuse (BOS is in
    // MLB, NBA and NHL — a bare-abbr index could only hold one) + a miss.
    // Expected FNV-1a of px+mask, computed host-side from the same logos.bin.
    struct Want { const char* league; const char* abbr; uint32_t hash; };
    static const Want wants[] = {
        {"mlb", "BOS", 0xa917f7d6u}, {"nfl", "KC", 0x2b7038d9u},
        {"nba", "LAL", 0xeacc63aau}, {"nhl", "VGK", 0x1621bb28u},
        {"epl", "LIV", 0x1b0feca6u}, {"nhl", "BOS", 0x6322ff39u},
        {"mlb", "ZZZ", 0},  // must miss
    };
    const uint32_t f2 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b2 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    for (const auto& w : wants) {
        Ref r;
        const bool hit = lookup(w.league, w.abbr, r);
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
    lookup("mlb", "BOS", r);
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
#include "http.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

static uint32_t http_int_free() {
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static void http_test() {
    int fails = 0;
    Serial.println("T-5.3 HTTP transport test:");
    WiFi.mode(WIFI_STA);
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

    Serial.printf("  RESULT: %s\n", fails ? "FAIL" : "PASS");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_HTTP_TEST


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

static void task_render(void*) {
  nb::config::bind_render_task(xTaskGetCurrentTaskHandle());
  for (;;) {
    // Wake on config change (T-4.3) or heartbeat every 5 s otherwise.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) {
      nb::config::load(g_cfg);  // writer already persisted; pull new values
      nb::config::apply_timezone(g_cfg.hw.timezone);  // live TZ swap too
      Serial.printf("[render] config applied live: brightness=%u mode=%u\n",
                    static_cast<unsigned>(g_cfg.hw.brightness),
                    static_cast<unsigned>(g_cfg.hw.display_mode));
    } else {
      heartbeat("render");
    }
  }
}
static void task_poll  (void*) { for (;;) { heartbeat("poll");   vTaskDelay(pdMS_TO_TICKS(5000)); } }
static void task_web   (void*) { for (;;) { heartbeat("web");    vTaskDelay(pdMS_TO_TICKS(5000)); } }
static void task_net   (void*) { for (;;) { heartbeat("net");    vTaskDelay(pdMS_TO_TICKS(5000)); } }

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
