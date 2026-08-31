// SPDX-License-Identifier: GPL-3.0-only
// Phase 0 / T-0.4 -- TLS smoke test (Wokwi). Throwaway code in spike/.
//
// Boot order is a HARD rule: WiFi -> SNTP -> TLS. The clock must land before
// the first handshake, or cert notBefore validation fails against a zero clock.
//
// GETs the ESPN NFL scoreboard (small) over HTTPS with esp_http_client +
// esp_crt_bundle -- the Mozilla CA bundle baked in by menuconfig, NEVER a
// pinned cert (ESPN rotates, pinning schedules an outage).
//   Accept: HTTP 200, bytes read == Content-Length, no cert error.
// Logs free heap before / peak(during) / after to size the UNTUNED mbedTLS
// session; T-0.6 applies the tuning and re-measures the same numbers.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"

// Wokwi's single virtual AP: open network, empty password, channel 6 skips the
// scan. A hardware build overrides these three via its own defines (secrets.h
// or build_flags); do not hardcode a real SSID here.
#define WIFI_SSID    "Wokwi-GUEST"
#define WIFI_PASS    ""
#define WIFI_CHANNEL 6

// Exact scoreboard URL the Python builds: {base}/{football/nfl}/scoreboard.
#define NFL_URL "https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== nosebleed T-0.4 TLS smoke test (Wokwi) ===");

  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  uint8_t st = WiFi.waitForConnectResult(20000);
  if (st != WL_CONNECTED) {
    Serial.printf("WiFi FAILED (status=%u). Re-run -- suspect Wokwi gateway.\n", (unsigned)st);
    for (;;) delay(1000);
  }
  IPAddress ip = WiFi.localIP();
  Serial.printf("WiFi connected  : %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);

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
  char tbuf[24];
  strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &t);
  Serial.printf("SNTP synced     : %s UTC (waited %lu ms)\n", tbuf, (unsigned long)waited);

  // ---- T-0.4: one HTTPS GET, full cert validation, no pinned cert ----
  const uint32_t heap_pre = (uint32_t)ESP.getFreeHeap();
  Serial.printf("heap[pre-tls]    : %u bytes\n", heap_pre);

  const esp_http_client_config_t http_cfg = {
      .url = NFL_URL,
      .user_agent = "marquee-display/2.0",   // Python's UA. (Browser UA also 403s --
                                              // the block is the Wokwi gateway IP, not the UA.)
      .method = HTTP_METHOD_GET,
      .timeout_ms = 20000,
      .buffer_size = 512,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
  if (!client) {
    Serial.println("http init FAILED");
    for (;;) delay(1000);
  }
  const uint32_t h_init = (uint32_t)ESP.getFreeHeap();
  Serial.printf("heap[post-init]  : %u bytes\n", h_init);

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    Serial.printf("TLS open FAILED  : %s (0x%x). Re-run -- suspect Wokwi gateway.\n",
                  esp_err_to_name(err), (unsigned)err);
    esp_http_client_cleanup(client);
    for (;;) delay(1000);
  }
  const uint32_t h_open = (uint32_t)ESP.getFreeHeap();
  Serial.printf("heap[post-open]  : %u bytes  (handshake done)\n", h_open);

  err = esp_http_client_fetch_headers(client);
  if (err != ESP_OK) {
    Serial.printf("fetch_headers FAILED: %s (0x%x)\n", esp_err_to_name(err), (unsigned)err);
  }
  const int status = esp_http_client_get_status_code(client);
  const int64_t clen = esp_http_client_get_content_length(client);
  Serial.printf("HTTP status      : %d\n", status);
  Serial.printf("Content-Length   : %lld\n", (long long)clen);

  int64_t got = 0;
  uint32_t h_min_read = (uint32_t)ESP.getFreeHeap();
  char rbuf[512];
  for (;;) {
    int r = esp_http_client_read_response(client, rbuf, sizeof rbuf);
    if (r < 0) { Serial.printf("read FAILED      : %d\n", r); break; }
    if (r == 0) break;
    got += r;
    if (clen >= 0 && got >= clen) break;
    const uint32_t h = (uint32_t)ESP.getFreeHeap();
    if (h < h_min_read) h_min_read = h;
  }
  Serial.printf("bytes read       : %lld / %lld\n", (long long)got, (long long)clen);
  Serial.printf("heap[min-reads]  : %u bytes  (lowest during body read)\n", h_min_read);

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  const uint32_t heap_post = (uint32_t)ESP.getFreeHeap();
  Serial.printf("heap[post-cleanup]: %u bytes\n", heap_post);

  // No watermark-reset in this IDF, so peak-during is the min of the sampled
  // during-phases; min-since-boot is the authoritative global low (catches the
  // handshake dip even if it falls between samples).
  const uint32_t during = h_init < h_open ? h_init : h_open;
  const uint32_t peak = h_min_read < during ? h_min_read : during;
  const uint32_t gmin = esp_get_minimum_free_heap_size();
  Serial.printf("min-since-boot   : %u bytes\n", (unsigned)gmin);
  Serial.printf("peak mbedTLS drop: %u bytes (pre-tls %u -> lowest during %u)\n",
                (unsigned)(heap_pre - peak), heap_pre, peak);

  const bool ok = (status == 200) && (clen >= 0) && (got == clen);
  Serial.printf("T-0.4 %s: status=%d, bytes=%lld/%lld, no cert error (crt_bundle)\n",
                ok ? "PASS" : "FAIL", status, (long long)got, (long long)clen);
}

void loop() {
  delay(1000);
}
