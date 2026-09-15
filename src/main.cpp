// SPDX-License-Identifier: GPL-3.0-only
//
// T-1.5 — Phase 1 task-topology skeleton. Four pinned tasks per AGENTS.md
// "Task topology" table, each logging a heartbeat with core ID, heap, and its
// stack high-water mark so T-1.5 accept ("high-water marks leave >25%
// headroom") is measurable.

#ifdef ARDUINO

#include <Arduino.h>
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
