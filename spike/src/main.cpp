// SPDX-License-Identifier: GPL-3.0-only
// Phase 0 / T-0.3 -- WiFi + SNTP spike (Wokwi). Throwaway code in spike/.
//
// Boot order is a HARD rule: WiFi -> SNTP -> TLS. SNTP must land before the
// first TLS handshake, or cert notBefore validation fails against a zero
// clock. No TLS here -- this task stops once the wall clock is correct.
//
// The three heap readings are the real post-WiFi ceiling the AGENTS.md memory
// budget hangs on ("~320 KB usable after WiFi" was a projection, not a
// measurement). T-0.1 baseline with WiFi down: 346072 B free.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// Wokwi's single virtual AP: open network, empty password, channel 6 skips the
// scan. A hardware build overrides these three via its own defines (secrets.h
// or build_flags); do not hardcode a real SSID here.
#define WIFI_SSID    "Wokwi-GUEST"
#define WIFI_PASS    ""
#define WIFI_CHANNEL 6

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== nosebleed T-0.3 WiFi + SNTP spike ===");

  Serial.printf("heap[1:pre-wifi] : %u bytes\n", (unsigned)ESP.getFreeHeap());

  Serial.printf("WiFi.begin(\"%s\", ch %d)...\n", WIFI_SSID, (int)WIFI_CHANNEL);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  uint8_t st = WiFi.waitForConnectResult(20000);
  if (st != WL_CONNECTED) {
    Serial.printf("WiFi FAILED (status=%u). Re-run -- suspect Wokwi gateway.\n",
                  (unsigned)st);
    for (;;) delay(1000);
  }
  IPAddress ip = WiFi.localIP();
  Serial.printf("WiFi connected  : %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);

  Serial.printf("heap[2:post-wifi]: %u bytes\n", (unsigned)ESP.getFreeHeap());

  Serial.println("SNTP sync (UTC) : pool.ntp.org, time.nist.gov ...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = 0;
  uint32_t waited = 0;
  while (now < 1700000000 && waited < 20000) {  // < 2023-11 => not synced yet
    delay(100);
    waited += 100;
    now = time(nullptr);
  }
  if (now < 1700000000) {
    Serial.println("SNTP FAILED: no time within 20 s. Re-run -- suspect Wokwi gateway.");
    for (;;) delay(1000);
  }
  struct tm t;
  localtime_r(&now, &t);
  char buf[24];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &t);
  Serial.printf("SNTP synced     : %s UTC (epoch %lu, waited %lu ms)\n",
                buf, (unsigned long)now, (unsigned long)waited);

  Serial.printf("heap[3:post-sntp]: %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println("T-0.3 done: WiFi -> SNTP ok, clock set, no TLS attempted.");
}

void loop() {
  delay(1000);
}
