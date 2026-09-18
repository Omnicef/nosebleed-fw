// SPDX-License-Identifier: GPL-3.0-only
//
// env:config — T-4.2/T-4.4 NVS config store. Moved verbatim out of main.cpp
// (Split test envs). Selected by build_src_filter, not by this #ifdef, but
// the guard is kept so the moved text is byte-identical and a stray compile
// of this file under another env stays inert.

#include "common.h"

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

void setup() {
    boot_prologue();
    // Boot A = fresh/other blob; boot B = ours staged before esp_restart.
    if (nb::config::load(g_cfg) && g_cfg.hw.brightness == 55) config_boot_check();
    else config_test();  // both branches never return
}
void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }
