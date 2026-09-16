// SPDX-License-Identifier: GPL-3.0-only

#ifdef ARDUINO

#include "panel.h"

#include <Arduino.h>  // delay()
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

namespace nb {
namespace panel {

// SEENGREAT RGB Matrix Adapter Board (E) V2.x — proven on hardware by the
// RED/GREEN/BLUE word card, T-2.8. NOT the silkscreen: the adapter's actual
// wiring transposes G<->B on both halves (G1 is GPIO17, B1 is GPIO8; same
// for G2/B2), while the silkscreen claims 18/8/17. R and the address/scan
// lines match the silkscreen. The library's S3 defaults are wrong for this
// board in every field (SPIKE_RESULTS T-0.2); V1.x is a different map.
//              R1 G1 B1 R2 G2 B2  A  B  C  D E LAT OE CLK
static const HUB75_I2S_CFG::i2s_pins kPinsV2 =
    {18, 17, 8, 16, 15, 1, 7, 48, 6, 47, 2, 21, 4, 5};

static MatrixPanel_I2S_DMA* g_panel = nullptr;

bool init(const config::HardwareSetting& hw) {
    if (g_panel) return true;
    // Positional order per HUB75_I2S_CFG ctor (header line ~319):
    // w, h, chain, pins, driver, line_decoder, double_buff, i2sspeed,
    // latch_blanking, clkphase, min_refresh, depth_bits.
    HUB75_I2S_CFG cfg(hw.cols, hw.rows, hw.chain_length, kPinsV2,
                      HUB75_I2S_CFG::SHIFTREG, HUB75_I2S_CFG::TYPE138,
                      hw.double_buff != 0,
                      hw.i2sspeed ? HUB75_I2S_CFG::HZ_16M : HUB75_I2S_CFG::HZ_8M,
                      hw.latch_blanking, hw.clkphase != 0, 60,
                      PIXEL_COLOR_DEPTH_BITS_DEFAULT);
    g_panel = new MatrixPanel_I2S_DMA(cfg);
    if (!g_panel->begin()) {
        delete g_panel;
        g_panel = nullptr;
        return false;
    }
    set_brightness(hw.brightness);
    return true;
}

void set_brightness(uint8_t pct) {
    if (!g_panel) return;
    // Config brightness is 0-100 (Python parity); the driver takes 0-255.
    g_panel->setBrightness(static_cast<uint8_t>(pct > 100 ? 255 : pct * 51 / 20));
}

int width() { return g_panel ? g_panel->width() : 0; }
int height() { return g_panel ? g_panel->height() : 0; }
int refresh_rate() { return g_panel ? g_panel->calculated_refresh_rate : 0; }

void blit(const Canvas16& c, int dx, int dy) {
    if (!g_panel) return;
    // Whole-panel push: every panel cell is written every frame, uncovered
    // areas black — a clipped push leaves stale columns behind during the
    // splash traverse (the smear I chased at T-2.8 re-test). Canvas16::get
    // is bounds-checked and reads black off-canvas. Per-pixel drawPixel is
    // fine for splash/bring-up; Phase 7 replaces this with a row memcpy.
    const int pw = width(), ph = height();
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            g_panel->drawPixel(x, y, c.get(x - dx, y - dy));
    g_panel->flipDMABuffer();
}

void splash(const Canvas16& c, uint32_t hold_ms) {
    const int pw = width(), ph = height();
    const int oy = (ph - c.h) / 2;
    const uint32_t frame_ms = 1000 / 30;
    for (;;) {
        if (c.w <= pw) {  // fits: hold in place, forever
            blit(c, (pw - c.w) / 2, oy);
            delay(hold_ms);
            continue;
        }
        delay(hold_ms);
        for (int x = 0; x <= c.w - pw; x++) {  // one traverse...
            blit(c, -x, oy);
            delay(frame_ms);
        }
        delay(hold_ms);  // ...then hold, then loop
    }
}

}  // namespace panel
}  // namespace nb

#endif  // ARDUINO
