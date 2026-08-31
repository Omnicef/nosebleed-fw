// SPDX-License-Identifier: GPL-3.0-only
// Phase 0 / T-0.1 — toolchain spike. Throwaway code in spike/.
//
// Goal: prove the pioarduino toolchain builds + flashes, and that the N16R8
// part reports 16 MB flash + 8 MB PSRAM on boot. The serial report below is
// the acceptance evidence; the on-board LED blink is secondary.

#include <Arduino.h>

// DevKitC-1 on-board WS2812 RGB LED (data-driven, not a plain digital LED).
static constexpr uint32_t LED_PIN = 48;

void setup() {
  Serial.begin(115200);
  delay(300);  // let the UART / monitor attach before the first line.
  Serial.println();
  Serial.println("=== nosebleed T-0.1 toolchain spike ===");
  Serial.printf("CPU freq        : %u MHz\n", (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("Flash chip size : %u bytes (%u MB)\n",
                (unsigned)ESP.getFlashChipSize(),
                (unsigned)(ESP.getFlashChipSize() / 1048576UL));
  Serial.printf("PSRAM size      : %u bytes (%u MB)\n",
                (unsigned)ESP.getPsramSize(),
                (unsigned)(ESP.getPsramSize() / 1048576UL));
  Serial.printf("PSRAM free      : %u bytes\n", (unsigned)ESP.getFreePsram());
  Serial.printf("Internal free   : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("psramFound      : %s\n", ESP.getPsramSize() ? "yes" : "NO");
  Serial.println("blink: on-board RGB on GPIO48 (red<->green)");
}

void loop() {
  static bool on = false;
  on = !on;
  neopixelWrite(LED_PIN, on ? 24 : 0, on ? 0 : 24, 0);
  delay(500);
}
