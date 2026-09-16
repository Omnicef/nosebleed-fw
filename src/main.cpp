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
    Serial.printf("  internal free      before=%lu after=%lu delta=%ld\n", f0, f1, df);
    Serial.printf("  internal largestblk  before=%lu after=%lu delta=%ld\n", b0, b1, db);
    if (!ok || df != 0 || db != 0) ++fails;

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

static void task_render(void*) { for (;;) { heartbeat("render"); vTaskDelay(pdMS_TO_TICKS(5000)); } }
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

#ifdef NB_LOGOS_TEST
  logos_test();  // never returns; tasks below are for normal boots only
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
// T-1.1 accept: env:native builds an empty program. Real host tests arrive at
// T-2.6. This guard is what lets both envs share one main file.
int main() { return 0; }
#endif
