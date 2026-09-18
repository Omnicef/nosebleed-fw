// SPDX-License-Identifier: GPL-3.0-only
//
// env:panel — T-2.8/T-2.9 panel-seam proof. Moved verbatim out of main.cpp
// (Split test envs). Selected by build_src_filter, not by this #ifdef, but
// the guard is kept so the moved text is byte-identical and a stray compile
// of this file under another env stays inert.

#include "common.h"

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

void setup() {
    boot_prologue();
    boot_load_config();  // panel_test reads g_cfg.hw
    panel_test();        // never returns
}
void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }
