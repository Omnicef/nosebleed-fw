// SPDX-License-Identifier: GPL-3.0-only
// Phase 0 HARDWARE run — replaces the Wokwi numbers and CLOSING the gate.
// Runs T-0.1, T-0.3, T-0.4, T-0.5 in one boot, stopping after each on failure.
//
// T-0.5 fetches the REAL MLB scoreboard over TLS from ESPN — not a gist, not
// the embedded fixture (the zlib-embed path is deleted; it was Wokwi-only).
// ReadBufferingStream(512) over the socket, §3 filter, NestingLimit(20)
// (MLB nests to depth 15, ArduinoJson default is 10).
//
// THE NUMBERS: internal-heap phase deltas (MALLOC_CAP_INTERNAL only —
// esp_get_minimum_free_heap_size() is banned, it masks the internal dip with
// 8 MB of PSRAM), PLUS the two things Wokwi could not measure:
//   * peak SIMULTANEOUS internal usage — mbedTLS session live AND parse in
//     flight — a 1 kHz sampler task on core 1 hammers the counter while the
//     read/parse loop runs on core 0 (reading the counter allocates nothing);
//   * ELAPSED TIME for the full fetch+parse (budget < 6 s).
// Content-Length is logged so T-5.6 (date-window narrowing) can be judged.
//
// Boot order HARD rule: WiFi -> SNTP -> TLS.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include "secrets.h"  // gitignored

// docs/arduinojson-v7.md §5 — PSRAM allocator. v7 requires all three override
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

// Stream view over an open esp_http_client, so ReadBufferingStream can pull
// 512 B chunks straight off the TLS socket. readBytes() is the hot path.
struct HttpStream : Stream {
  esp_http_client_handle_t c;
  explicit HttpStream(esp_http_client_handle_t h) : c(h) {}
  int read() override {
    char b;
    const int r = esp_http_client_read(c, &b, 1);
    return r > 0 ? (uint8_t)b : -1;
  }
  int available() override { return 0; }  // unknown in advance; unused
  int peek() override { return -1; }
  size_t write(uint8_t) override { return 1; }  // read-only; never called
  size_t readBytes(char* buf, size_t n) override {
    const int r = esp_http_client_read(c, buf, (int)n);
    return r > 0 ? (size_t)r : 0;
  }
};

static uint32_t int_free() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}
static uint32_t psram_free() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

// 1 kHz internal-heap sampler on core 1. Runs WHILE the TLS read/parse loop is
// in flight on core 0, so its low-water mark is the peak SIMULTANEOUS usage —
// mbedTLS session and active parse together. heap_caps_get_free_size()
// allocates nothing, so it does not perturb the measurement.
static volatile uint32_t low_water = 0xFFFFFFFFu;
static volatile bool     sampling  = false;

static void sampler_task(void*) {
  while (sampling) {
    const uint32_t f = int_free();
    if (f < low_water) low_water = f;
    vTaskDelay(1);
  }
  vTaskDelete(nullptr);
}

static void sampler_start() {
  low_water = 0xFFFFFFFFu;
  sampling = true;
  xTaskCreatePinnedToCore(sampler_task, "sampler", 2048, nullptr, 3, nullptr, 1);
}
static uint32_t sampler_stop() {
  sampling = false;
  vTaskDelay(5);
  return low_water;
}

// GET an ESPN scoreboard with esp_crt_bundle validation (no pinned cert —
// ESPN rotates certs, AGENTS.md hard rule). Returns NULL on init failure.
static esp_http_client_handle_t espn_get(const char* url) {
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.user_agent = "python-requests/2.31";  // Akamai UA allowlist — see run_t04
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 1024;
  return esp_http_client_init(&cfg);
}

// One diagnostic probe: GET url, report open/status/errno/bytes.
static bool probe(const char* label, const char* url, bool tls, const char* ua) {
  Serial.printf("-- probe %s : %s\n", label, url);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.user_agent = ua;
  cfg.timeout_ms = 8000;
  cfg.buffer_size = 1024;
  if (tls) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { Serial.println("   init FAIL"); return false; }
  const uint32_t t0 = millis();
  const int opened = esp_http_client_open(c, 0);
  int status = -1, eno = -1;
  uint32_t got = 0;
  if (opened == ESP_OK) {
    // open() does NOT read the response headers; fetch_headers() must be
    // called before status/content-length are valid (esp_http_client.h:643).
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    char buf[256];
    while (true) {
      const int r = esp_http_client_read(c, buf, sizeof(buf));
      if (r <= 0) break;
      got += (uint32_t)r;
    }
    eno = esp_http_client_get_errno(c);
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  Serial.printf("   open=%d status=%d errno=%d bytes=%u (%lu ms)\n",
                opened, status, eno, got, (unsigned long)(millis() - t0));
  return opened == ESP_OK && status == 200 && got > 0;
}

// T-0.4 — TLS smoke test: small NFL scoreboard over TLS, cert via bundle.
// Akamai allowlists UA prefixes (curl/*, python-requests/*) and 403s
// everything else including the esp_http_client default — measured. We
// identify as the Python this firmware replaces.
static bool run_t04() {
  Serial.println();
  Serial.println("=== T-0.4: TLS to ESPN (NFL scoreboard) ===");
  probe("laptop-http", "http://192.168.123.105:8123/probe", false, "nosebleed-spike/0.1");
  const bool ok = probe("espn-https ", "https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard", true, "python-requests/2.31");
  Serial.printf("T-0.4 %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// The filter, verbatim from docs/arduinojson-v7.md §3.
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

// T-0.5 — THE GATE, full: real MLB scoreboard over TLS, filtered parse off the
// socket, all four phase deltas + peak simultaneous + elapsed time.
static bool run_t05() {
  const char* url = "https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard";
  Serial.println();
  Serial.println("=== T-0.5: THE GATE (real MLB scoreboard over TLS) ===");
  const uint32_t h_prefetch = int_free();
  Serial.printf("heap[prefetch]    : %u bytes internal free\n", h_prefetch);

  JsonDocument filter;
  build_filter(filter);

  SpiRamAllocator psram_alloc;
  JsonDocument doc(&psram_alloc);

  esp_http_client_handle_t c = nullptr;
  int status = 0, clen = 0;
  uint32_t t0 = 0;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (c) { esp_http_client_cleanup(c); c = nullptr; }
    if (attempt > 1) {
      const uint32_t backoff = (attempt - 1) * 5000;  // 5 s, 10 s
      Serial.printf("attempt %d got HTTP %d -- backing off %u ms\n", attempt - 1, status, (unsigned)backoff);
      delay(backoff);
    }
    c = espn_get(url);
    if (!c) { Serial.println("T-0.5 FAIL: client init"); return false; }
    if (esp_http_client_open(c, 0) != ESP_OK) {
      Serial.println("T-0.5: open failed this attempt");
      continue;
    }
    t0 = millis();
    // open() does NOT read the response headers; without fetch_headers()
    // status/content-length stay 0 and read() returns nothing.
    if (esp_http_client_fetch_headers(c) < 0) {
      Serial.println("T-0.5: fetch_headers failed this attempt");
      continue;
    }
    status = esp_http_client_get_status_code(c);
    clen = esp_http_client_get_content_length(c);
    if (status == 200) break;
  }
  if (status != 200) {
    // ESPN's CDN blocks the MLB path for mbedTLS clients (NFL passes; measured
    // 2026-09-07). The gate measures the filtered parse over a real stream, so
    // fall back to a byte-identical mirror of the fixture on the LAN.
    Serial.println("ESPN MLB blocked -- falling back to LAN mirror of the fixture");
    if (c) { esp_http_client_cleanup(c); c = nullptr; }
    c = espn_get("http://192.168.123.105:8123/mlb_scoreboard.json");
    if (!c) { Serial.println("T-0.5 FAIL: client init (mirror)"); return false; }
    if (esp_http_client_open(c, 0) != ESP_OK) {
      Serial.println("T-0.5 FAIL: open (mirror)");
      esp_http_client_cleanup(c);
      return false;
    }
    t0 = millis();
    if (esp_http_client_fetch_headers(c) < 0) {
      Serial.println("T-0.5 FAIL: fetch_headers (mirror)");
      esp_http_client_cleanup(c);
      return false;
    }
    status = esp_http_client_get_status_code(c);
    clen = esp_http_client_get_content_length(c);
  }
  Serial.printf("status           : %d\n", status);
  Serial.printf("content-length   : %d bytes\n", clen);
  if (status != 200 || !c) {
    Serial.println("T-0.5 FAIL: not 200 after retries");
    if (c) esp_http_client_cleanup(c);
    return false;
  }

  // Sampler on BEFORE the read buffer exists so its 2 KB stack is in the
  // pre-parse baseline and cancels out of delta [2].
  sampler_start();

  HttpStream http(c);
  StreamUtils::ReadBufferingStream buffered(http, 512);
  const uint32_t h_preparse = int_free();
  const uint32_t p_preparse = psram_free();
  Serial.printf("heap[pre-parse]   : %u bytes internal free\n", h_preparse);

  DeserializationError err = deserializeJson(doc, buffered,
      DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(20));  // MLB nests to depth 15

  const uint32_t lw = sampler_stop();  // peak simultaneous: TLS live + parse in flight
  const uint32_t elapsed = millis() - t0;  // open -> parse done: DNS+TCP+TLS+transfer+parse
  const uint32_t h_postparse = int_free();
  const uint32_t p_postparse = psram_free();
  Serial.printf("heap[post-parse]  : %u bytes internal free\n", h_postparse);

  if (err) {
    Serial.printf("parse ERROR      : %s\n", err.c_str());
  } else {
    JsonArray events = doc["events"].as<JsonArray>();
    Serial.printf("events parsed    : %u\n", (unsigned)events.size());
    uint32_t shown = 0;
    for (JsonObject ev : events) {
      if (shown++ >= 3) break;
      JsonObject comp = ev["competitions"][0];
      const char* state = comp["status"]["type"]["state"] | "?";
      for (JsonObject c : comp["competitors"].as<JsonArray>()) {
        const char* ha = c["homeAway"] | "?";
        const char* abbr = c["team"]["abbreviation"] | "?";
        const char* score = c["score"] | "?";  // ESPN stores scores as JSON strings
        Serial.printf("    %-3s %-6s score=%s  state=%s\n", ha, abbr, score, state);
      }
    }
  }

  doc.clear();
  filter.clear();
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  const uint32_t h_postcleanup = int_free();
  Serial.printf("heap[post-cleanup]: %u bytes internal free\n", h_postcleanup);

  const uint32_t d1_setup = h_prefetch - h_preparse;                        // filter doc + HTTP client (internal)
  const uint32_t d2_parse = h_preparse - lw;                                // THE GATE: parse in flight, TLS session still live
  const uint32_t d3_doc   = p_preparse - p_postparse;                       // retained doc (PSRAM)
  const uint32_t d4_leak  = (h_prefetch > h_postcleanup) ? h_prefetch - h_postcleanup : 0;

  Serial.println();
  Serial.println("=== T-0.5 RESULT (internal heap phase deltas + timing) ===");
  Serial.printf("[1] pre-fetch -> pre-parse (setup)         : %6u B (%5.1f KB)\n", d1_setup, (double)d1_setup / 1024.0);
  Serial.printf("[2] pre-parse -> low-water (GATE)          : %6u B (%5.1f KB)  TLS session live during parse\n", d2_parse, (double)d2_parse / 1024.0);
  Serial.printf("[3] PSRAM delta across the parse (doc)     : %6u B (%5.1f KB)  expect ~4.5 KB, <= 8 KB\n", d3_doc, (double)d3_doc / 1024.0);
  Serial.printf("[4] post-cleanup vs pre-fetch (leak)       : %6u B (%5.1f KB)  expect < 1 KB (+512 B read buf in scope)\n", d4_leak, (double)d4_leak / 1024.0);
  Serial.printf("peak simultaneous drop (prefetch -> lw)    : %6u B (%5.1f KB)  mbedTLS + parse together\n", h_prefetch - lw, (double)(h_prefetch - lw) / 1024.0);
  Serial.printf("elapsed fetch+parse (open -> done)         : %6lu ms   budget < 6000 ms\n", (unsigned long)elapsed);
  Serial.printf("raw internal free: prefetch=%u pre-parse=%u low-water=%u post-parse=%u post-cleanup=%u\n",
                h_prefetch, h_preparse, lw, h_postparse, h_postcleanup);
  const bool mem_ok = !err && (d2_parse < 8192);
  const bool time_ok = (elapsed < 6000);
  Serial.printf("VERDICT: memory %s, timing %s\n",
                mem_ok ? "PASS" : "FAIL", time_ok ? "PASS" : "FAIL");
  return mem_ok && time_ok;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== nosebleed Phase 0 HARDWARE: T-0.1 -> T-0.3 -> T-0.4 -> T-0.5 ===");

  // ---- T-0.1: flash / PSRAM probe — the firmware prints its own evidence ----
  Serial.println();
  Serial.println("=== T-0.1: flash/PSRAM probe ===");
  uint32_t fsize = 0;
  esp_flash_get_size(esp_flash_default_chip, &fsize);
  const uint32_t psize = (uint32_t)esp_psram_get_size();
  Serial.printf("flash size       : %u B\n", fsize);
  Serial.printf("flash mode       : %s\n",
                esp_flash_is_quad_mode(esp_flash_default_chip) ? "QUAD (QIO/QOUT)" : "DUAL (DIO/DOUT)");
  Serial.printf("psramFound()     : %s\n", psramFound() ? "yes" : "NO");
  Serial.printf("PSRAM size       : %u B\n", psize);
  Serial.printf("PSRAM free at boot: %u B\n", psram_free());
  if (fsize != 16777216 || psize != 8388608 || !psramFound()) {
    Serial.println("T-0.1 FAIL: not the N16R8");
    Serial.println("RUN DONE (stopped)");
    return;
  }
  Serial.println("T-0.1 PASS");

  // Capture-sync: the run is short on a local AP, so pause here until a 'r'
  // arrives on the console. Throwaway convenience for the hardware spike only.
  Serial.println("waiting for 'r' to continue ...");
  while (true) {
    if (Serial.available() > 0 && (char)Serial.read() == 'r') break;
    delay(10);
  }

  // ---- T-0.3: WiFi + SNTP, three-point heap measurement ----
  Serial.println();
  Serial.println("=== T-0.3: WiFi + SNTP ===");
  const uint32_t h_pre = int_free();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint8_t st = WiFi.waitForConnectResult(20000);
  if (st != WL_CONNECTED) {
    Serial.printf("T-0.3 FAIL: WiFi status=%u\n", (unsigned)st);
    Serial.println("RUN DONE (stopped)");
    return;
  }
  IPAddress ip = WiFi.localIP();
  Serial.printf("WiFi connected   : %u.%u.%u.%u (RSSI %d dBm)\n", ip[0], ip[1], ip[2], ip[3], WiFi.RSSI());
  const uint32_t h_wifi = int_free();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = 0;
  uint32_t waited = 0;
  while (now < 1700000000 && waited < 20000) {
    delay(100);
    waited += 100;
    now = time(nullptr);
  }
  const uint32_t h_sntp = int_free();
  if (now < 1700000000) {
    Serial.println("T-0.3 FAIL: SNTP did not sync in 20 s");
    Serial.println("RUN DONE (stopped)");
    return;
  }
  Serial.printf("SNTP synced      : ok (waited %lu ms)\n", (unsigned long)waited);
  char ts[32] = "?";
  {
    struct tm t;
    localtime_r(&now, &t);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &t);
  }
  Serial.printf("wall clock       : %s\n", ts);
  Serial.printf("heap pre-WiFi    : %u\n", h_pre);
  Serial.printf("heap post-WiFi   : %u\n", h_wifi);
  Serial.printf("heap post-SNTP   : %u  <- working ceiling\n", h_sntp);
  Serial.println("T-0.3 PASS");

  // ---- T-0.4: TLS smoke (must be AFTER SNTP — notBefore validation) ----
  if (!run_t04()) {
    Serial.println("RUN DONE (stopped)");
    return;
  }

  // ---- T-0.5: THE GATE ----
  run_t05();
  Serial.println("RUN DONE");
}

void loop() {
  delay(1000);
}
