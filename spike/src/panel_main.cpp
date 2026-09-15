// SPDX-License-Identifier: GPL-3.0-only
// T-0.2 panel hello-world throwaway. Not production. Solid fills + gradient +
// a moving stripe, to confirm wiring, level-shift, ghosting and refresh.
//
// Board: SEENGREAT RGB Matrix Adapter Board (E), SKU 250911, for
// ESP32-S3-DevKitC-1. Two hardware revisions exist with different GPIO maps
// (manufacturer wiki 186). Switch BOARD_V1 / BOARD_V2 below. Default V2.

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

//#define BOARD_V1
#ifndef BOARD_V1
#define BOARD_V2
#endif

static const HUB75_I2S_CFG::i2s_pins PINMAP =
#ifdef BOARD_V1
    // V1.x silkscreen: R1=37 G1=6 B1=36 R2=35 G2=5 B2=0 A=45 B=1 C=48 D=2 E=4 LAT=38 OE=21 CLK=47
    {37, 6, 36, 35, 5, 0, 45, 1, 48, 2, 4, 38, 21, 47};
#else
    // V2.x silkscreen: R1=18 G1=8 B1=17 R2=16 G2=1 B2=15 A=7 B=48 C=6 D=47 E=2 LAT=21 OE=4 CLK=5
    {18, 8, 17, 16, 1, 15, 7, 48, 6, 47, 2, 21, 4, 5};
#endif

// 64x32, 1/16 scan, single panel, board-specific pin map, plain shift-register
// driver, double-buffer OFF.
HUB75_I2S_CFG mx64x32(64, 32, 1, PINMAP);

MatrixPanel_I2S_DMA panel(mx64x32);

static void show(const char *name, uint16_t c, int ms) {
  panel.fillScreen(c);
  Serial.printf("%s\n", name);
  delay(ms);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.printf("free heap before begin: %u\n", (unsigned)ESP.getFreeHeap());
  if (!panel.begin()) {
    Serial.println("panel.begin() FAILED — check wiring/PSU");
    while (true) delay(1000);
  }

  // Achieved refresh, auto-derived by the lib from i2sspeed + depth +
  // min_refresh_rate. (lsbMsbTransitionBit is internal in v3.0.15.)
  Serial.printf("begin OK, calculated_refresh_rate = %d Hz\n", panel.calculated_refresh_rate);
  Serial.printf("free heap after begin: %u\n", (unsigned)ESP.getFreeHeap());

  // Deliberately dim: no 5V/4A PSU available on this bench. Full-white T-0.2
  // acceptance is deferred until the real supply arrives.
  panel.setBrightness(16);
}

void loop() {
  // Corner colours — spot dead columns / swapped channels.
  show("RED", panel.color565(255, 0, 0), 1200);
  show("GREEN", panel.color565(0, 255, 0), 1200);
  show("BLUE", panel.color565(0, 0, 255), 1200);
  show("WHITE", panel.color565(255, 255, 255), 1200);  // brown-out watch
  show("BLACK", panel.color565(0, 0, 0), 600);

  // Horizontal RGB->cyan gradient — spot stuck/off pixels and tearing.
  for (int x = 0; x < 64; x++) {
    uint8_t r = 255 - x * 4;
    uint8_t g = x * 4;
    uint8_t b = 128;
    panel.fillRect(x, 0, 1, 32, panel.color565(r, g, b));
  }
  Serial.println("gradient");
  delay(2000);

  // 8px stripe sweeping right — checks scroll smoothness / DMA refresh.
  for (int i = -8; i < 72; i++) {
    panel.fillScreen(panel.color565(0, 0, 0));
    panel.fillRect(i, 0, 8, 32, panel.color565(255, 255, 255));
    delay(40);
  }
}
