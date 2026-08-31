// SPDX-License-Identifier: GPL-3.0-only
// Phase 0 / T-0.5 -- THE GATE: filtered MLB fetch (memory half). Throwaway in spike/.
//
// Fetches the mlb_scoreboard.json (1.46 MB, 15 events) from a PUBLIC GIST
// (raw.githubusercontent.com -- valid cert in the Mozilla crt bundle, so this is
// real TLS + real esp_crt_bundle validation + a real 1.4 MB stream), NOT ESPN,
// which 403s the Wokwi Public Gateway egress IP (see PLAN.md T-0.5).
//
// Parses straight off the stream with an ArduinoJson v7 Filter, wrapped in
// ReadBufferingStream (512 B) -- never byte-at-a-time, never the whole response
// buffered. The parse doc lives in PSRAM (docs/arduinojson-v7.md §3/§5) so the
// internal-heap peak isolates the TLS session + HTTP + filter + read buffer.
//
// Boot order HARD rule: WiFi -> SNTP -> TLS.
//
// THE NUMBER: peak INTERNAL heap. esp_get_minimum_free_heap_size() is banned for
// this -- it tracks internal+PSRAM combined and 8 MB of PSRAM masks the internal
// dip. Sample heap_caps_get_free_size(MALLOC_CAP_INTERNAL) at the four phase
// boundaries instead. Elapsed time is IGNORED (Wokwi CPU is capped ~8 MHz; the
// timing half of the gate is deferred to hardware).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_heap_caps.h"
#include <ArduinoJson.h>
#include <StreamUtils.h>

// Wokwi's single virtual AP: open network, empty password, channel 6 skips the
// scan. A hardware build overrides these via its own defines; no real SSID here.
#define WIFI_SSID    "Wokwi-GUEST"
#define WIFI_PASS    ""
#define WIFI_CHANNEL 6

// TODO(replace): public gist raw URL for mlb_scoreboard.json (PLAN.md T-0.5).
#define GIST_URL "https://raw.githubusercontent.com/PLACEHOLDER/mlb_scoreboard.json"

// docs/arduinojson-v7.md §5 -- PSRAM allocator. v7 requires all three override
// methods, including reallocate(). Keeps the parse doc out of internal heap.
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* p) override {
    heap_caps_free(p);
  }
  void* reallocate(void* p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
  }
};

static uint32_t int_free() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

// The filter, verbatim from docs/arduinojson-v7.md §3. `true` = keep the field;
// a lone [0] element in an array filters EVERY element of that array.
static void build_filter(JsonDocument& f) {
  JsonObject ev = f["events"][0].to<JsonObject>();
  ev["id"] = true;
  ev["date"] = true;

  JsonObject comp = ev["competitions"][0].to<JsonObject>();
  JsonObject st = comp["status"].to<JsonObject>();
  st["period"] = true;
  st["displayClock"] = true;
  st["type"]["state"] = true;
  st["type"]["shortDetail"] = true;

  JsonObject c = comp["competitors"][0].to<JsonObject>();
  c["homeAway"] = true;
  c["score"] = true;
  JsonObject tm = c["team"].to<JsonObject>();
  tm["id"] = true;
  tm["displayName"] = true;
  tm["abbreviation"] = true;
  tm["color"] = true;

  comp["situation"] = true;  // small -- keep the whole subtree
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== nosebleed T-0.5 GATE (memory half): filtered MLB fetch ===");

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
  while (now < 1700000000 && waited < 20000) {
    delay(100);
    waited += 100;
    now = time(nullptr);
  }
  if (now < 1700000000) {
    Serial.println("SNTP FAILED: no time within 20 s. Re-run -- suspect Wokwi gateway.");
    for (;;) delay(1000);
  }
  Serial.printf("SNTP synced     : ok (waited %lu ms)\n", (unsigned long)waited);

  // ---- T-0.5: filtered streaming parse of the 1.46 MB MLB scoreboard ----
  const uint32_t heap_prefetch = int_free();
  Serial.printf("heap[prefetch]   : %u bytes internal free\n", heap_prefetch);

  JsonDocument filter;          // small, internal
  build_filter(filter);

  SpiRamAllocator psram_alloc;
  JsonDocument doc(&psram_alloc);  // parse doc in PSRAM -- internal heap stays clean

  WiFiClientSecure secure;       // uses esp_crt_bundle by default -- no pin, no insecure
  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(secure, GIST_URL)) {
    Serial.println("http.begin FAILED");
    for (;;) delay(1000);
  }

  const int code = http.GET();
  Serial.printf("HTTP status     : %d\n", code);
  const int64_t clen = http.getSize();
  Serial.printf("Content-Length  : %lld\n", (long long)clen);
  const uint32_t heap_posthandshake = int_free();
  Serial.printf("heap[post-handshake]: %u bytes internal free\n", heap_posthandshake);

  if (code != HTTP_CODE_OK) {
    Serial.println("GATE: could not fetch 200. Re-run -- suspect Wokwi gateway.");
    http.end();
    for (;;) delay(1000);
  }

  StreamUtils::ReadBufferingStream buffered(*http.getStreamPtr(), 512);
  DeserializationError err = deserializeJson(doc, buffered, DeserializationOption::Filter(filter));
  const uint32_t heap_postparse = int_free();
  Serial.printf("heap[post-parse]  : %u bytes internal free\n", heap_postparse);

  if (err) {
    Serial.printf("parse ERROR      : %s\n", err.c_str());
  } else {
    JsonArray events = doc["events"].as<JsonArray>();
    Serial.printf("events parsed    : %u\n", (unsigned)events.size());
    // Spot-check: first three events, team abbr + score + state.
    uint32_t shown = 0;
    for (JsonObject ev : events) {
      if (shown++ >= 3) break;
      JsonObject comp = ev["competitions"][0];
      const char* state = comp["status"]["type"]["state"] | "?";
      for (JsonObject c : comp["competitors"].as<JsonArray>()) {
        const char* ha = c["homeAway"] | "?";
        const char* abbr = c["team"]["abbreviation"] | "?";
        int score = c["score"] | -1;
        Serial.printf("    %-3s %-6s score=%d  state=%s\n", ha, abbr, score, state);
      }
    }
  }

  http.end();
  doc.clear();
  const uint32_t heap_postcleanup = int_free();
  Serial.printf("heap[post-cleanup]: %u bytes internal free\n", heap_postcleanup);

  // Peak internal heap = prefetched free minus the lowest free at any sample.
  uint32_t min_free = heap_prefetch;
  if (heap_posthandshake < min_free) min_free = heap_posthandshake;
  if (heap_postparse < min_free) min_free = heap_postparse;
  if (heap_postcleanup < min_free) min_free = heap_postcleanup;
  const uint32_t peak = heap_prefetch - min_free;

  Serial.println();
  Serial.println("=== T-0.5 RESULT ===");
  Serial.printf("prefetch free    : %u\n", heap_prefetch);
  Serial.printf("min free (any)   : %u\n", min_free);
  Serial.printf("PEAK internal    : %u bytes (%.1f KB)\n", peak, (double)peak / 1024.0);
  Serial.printf("GATE (<~50 KB)   : %s\n",
                peak < 51200 ? "PASS" : "OVER -- untuned; T-0.6 mbedTLS tuning expected to recover ~14 KB");
}

void loop() {
  delay(1000);
}
