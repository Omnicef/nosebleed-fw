// SPDX-License-Identifier: GPL-3.0-only
//
// Shared boot prologue for the ARDUINO builds. Splitting the seven test mains
// out of main.cpp (see the "Split test envs" WORKLOG entry) meant the pieces
// every build used — the diagnostic banner, the partition/heap snapshots, the
// PSRAM canvas allocator and the one global Config — needed one home that both
// normal boot (src/main.cpp) and each src/envs/<env>_test.cpp could reach.
//
// Exactly ONE of those translation units is compiled per build: esp32s3 selects
// main.cpp and excludes this directory (build_src_filter); each test env
// includes exactly one env file and excludes main.cpp. So the `static`
// definitions below land once in whichever binary is being built — never twice,
// no ODR worry.
#pragma once
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

// T-1.3: the sdkconfig values that actually reached the compiler diverge from
// what sdkconfig.defaults asks for on the Arduino core (T-0.6); this makes the
// divergence explicit.
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

// Everything setup() did before it dispatched to a test entry or the normal-boot
// path. Byte-for-byte the old pre-dispatch sequence, so the split is
// behaviour-preserving.
static void boot_prologue() {
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
}

// The normal-boot config load that panel/card tests need before they touch
// g_cfg.hw. The logos/http/cache/config tests run before this (config_test does
// its own reset/load), matching the old setup() dispatch order.
static void boot_load_config() {
  if (!nb::config::load(g_cfg)) Serial.println("config: absent/corrupt — seeded defaults");
  else Serial.printf("config: loaded (brightness=%u)\n",
                     static_cast<unsigned>(g_cfg.hw.brightness));
  Serial.printf("timezone: '%s' -> %s\n", g_cfg.hw.timezone,
                nb::config::apply_timezone(g_cfg.hw.timezone) == nb::config::TzResult::kApplied
                     ? "applied" : "UTC fallback");
}

#endif  // ARDUINO
