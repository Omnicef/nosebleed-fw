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
#include <time.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include "miniz.h"    // S3 ROM tinfl: tinfl_decompress_mem_to_mem (zlib stream)
#include "mlb_fix.h"  // zlib-wrapped 1.46 MB fixture (88 KB) -- see header note

// Wokwi's single virtual AP: open network, empty password, channel 6 skips the
// scan. A hardware build overrides these via its own defines; no real SSID here.
#define WIFI_SSID    "Wokwi-GUEST"
#define WIFI_PASS    ""
#define WIFI_CHANNEL 6

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

// The Wokwi gateway cannot TCP-connect to the gist CDN (DNS resolves, connect
// returns -1) while ESPN TLS works (T-0.4), so the identical 1.46 MB bytes are
// fed from flash instead of a socket. THE GATE is the cost of the *filtered
// parse*, which is transport-independent -- this Stream makes
// ReadBufferingStream + deserializeJson run the exact same code path a socket
// would. Delta 1 (the TLS session) is a separate line, measured at T-0.4.
struct MemStream : Stream {
  const uint8_t* p;
  size_t n;
  MemStream(const uint8_t* data, size_t len) : p(data), n(len) {}
  int read() override {
    if (!n) return -1;
    const int b = p[0];
    p += 1;
    n -= 1;
    return b;
  }
  int available() override { return (int)n; }
  int peek() override { return n ? p[0] : -1; }
  size_t write(uint8_t) override { return 1; }  // read-only stream; never called
};

static uint32_t int_free() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}
static uint32_t psram_free() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

// THE GATE is the internal-heap LOW-WATER across the parse, not just the end
// state. deserializeJson() is one call we cannot sample inside, so a task on
// core 1 hammers the internal counter while the parse runs on core 0. Reading
// the heap counter allocates nothing, so it does not perturb the measurement.
static volatile uint32_t parse_low_water = 0xFFFFFFFFu;
static volatile bool     parse_sampling  = false;

static void parse_sampler_task(void*) {
  while (parse_sampling) {
    const uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (f < parse_low_water) parse_low_water = f;
    vTaskDelay(1);  // ~1 kHz -- the parse runs for seconds on Wokwi
  }
  vTaskDelete(nullptr);
}

// Inflate the 88 KB zlib payload in flash into a 1.46 MB PSRAM buffer. Run in a
// dedicated task: tinfl_decompress_mem_to_mem() keeps a ~10 KB tinfl_decompressor
// on the caller stack (the 32 KB LZ dict is the PSRAM output buffer, not the
// stack), so a 24 KB task stack is ample and the default 8 KB loop task is not.
// One-time setup, self-deletes before the heap baseline is taken, so its cost
// cancels out of every delta.
static uint8_t*      raw_buf     = nullptr;  // PSRAM, mlb_fix_raw_len bytes
static uint32_t      raw_len     = 0;        // decompressed bytes (== raw_len)
static volatile int  inflate_rc  = -1;
static volatile bool inflate_done = false;

static void inflate_task(void*) {
  raw_buf = (uint8_t*)heap_caps_malloc(mlb_fix_raw_len, MALLOC_CAP_SPIRAM);
  if (!raw_buf) { inflate_rc = -2; inflate_done = true; vTaskDelete(nullptr); return; }
  size_t out = mlb_fix_raw_len;
  const size_t rc = tinfl_decompress_mem_to_mem(
      raw_buf, out, mlb_fix_data, mlb_fix_len,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (rc == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) inflate_rc = -1;
  else { raw_len = (uint32_t)out; inflate_rc = 0; }
  inflate_done = true;
  vTaskDelete(nullptr);
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
    Serial.println("T-0.5 DONE");
    return;
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
    Serial.println("T-0.5 DONE");
    return;
  }
  Serial.printf("SNTP synced     : ok (waited %lu ms)\n", (unsigned long)waited);

  // The Wokwi gateway blocks TCP egress to the gist CDN (measured: DNS resolves
  // to 185.199.111.133 but connect() -> -1) while ESPN TLS works (T-0.4). So the
  // identical 1.46 MB bytes come from flash, not a socket. THE GATE is the cost
  // of the *filtered parse*, which is transport-independent. Delta 1 (TLS session)
  // is a separate line, already measured at T-0.4 against ESPN.
  //
  // Wokwi won't flash/boot a 2.25 MB app image (a raw 1.46 MB const array
  // boot-loops the 2nd-stage bootloader; an identical-size zero array does too,
  // so it's a size limit, not content). So the fixture is zlib-wrapped in flash
  // (88 KB) and inflated into PSRAM here, one-time, before the baseline.

  inflate_done = false;
  xTaskCreatePinnedToCore(inflate_task, "inflate", 24576, nullptr, 2, nullptr, 0);
  while (!inflate_done) delay(1);
  if (inflate_rc != 0) {
    Serial.printf("INFLATE FAILED (rc=%d) -- raw buf %s\n", inflate_rc,
                  raw_buf ? "alloc'd" : "NULL");
    Serial.println("T-0.5 DONE");
    return;
  }
  Serial.printf("inflated        : %u B raw JSON in PSRAM (from %u B zlib in flash)\n",
                raw_len, mlb_fix_len);

  // ---- T-0.5: filtered streaming parse of the 1.46 MB MLB scoreboard ----
  const uint32_t heap_prefetch = int_free();
  Serial.printf("heap[prefetch]   : %u bytes internal free\n", heap_prefetch);

  JsonDocument filter;          // small, internal -- built once, reused per poll
  build_filter(filter);

  SpiRamAllocator psram_alloc;
  JsonDocument doc(&psram_alloc);  // parse doc in PSRAM -- internal heap stays clean

  // Start the sampler before the parse so it catches the true internal low-water.
  parse_low_water = 0xFFFFFFFFu;
  parse_sampling = true;
  xTaskCreatePinnedToCore(parse_sampler_task, "hsampler", 2048, nullptr, 3, nullptr, 1);

  // Build the read buffer, THEN take the pre-parse baseline, so the buffer + the
  // sampler's 2 KB stack are in the baseline and cancel out of delta 2.
  MemStream mem(raw_buf, raw_len);
  StreamUtils::ReadBufferingStream buffered(mem, 512);
  const uint32_t heap_preparse = int_free();
  const uint32_t psram_preparse = psram_free();
  Serial.printf("heap[pre-parse]   : %u bytes internal free\n", heap_preparse);

  DeserializationError err = deserializeJson(doc, buffered, DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(20));  // ESPN nests to depth 15; ArduinoJson default is 10

  parse_sampling = false;
  vTaskDelay(5);
  const uint32_t heap_postparse = int_free();
  const uint32_t psram_postparse = psram_free();
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
        const char* score = c["score"] | "?";  // ESPN stores score as a JSON string
        Serial.printf("    %-3s %-6s score=%s  state=%s\n", ha, abbr, score, state);
      }
    }
  }

  doc.clear();
  filter.clear();
  const uint32_t heap_postcleanup = int_free();
  Serial.printf("heap[post-cleanup]: %u bytes internal free\n", heap_postcleanup);

  // T-0.5 verdict -- phase deltas, NOT a single total (PLAN.md T-0.5). No TLS in
  // this run (gateway blocks the host), so [1] is the parse-setup cost (filter
  // doc) and the TLS session is the T-0.4 figure. THE GATE is [2].
  const uint32_t d1_setup = heap_prefetch - heap_preparse;                       // filter doc (internal)
  const uint32_t d2_parse = heap_preparse - parse_low_water;                     // THE GATE
  const uint32_t d3_doc   = psram_preparse - psram_postparse;                    // retained doc (PSRAM)
  const uint32_t d4_leak  = (heap_prefetch > heap_postcleanup)
                                ? heap_prefetch - heap_postcleanup : 0;           // leak check

  Serial.println();
  Serial.println("=== T-0.5 RESULT (internal heap phase deltas) ===");
  Serial.printf("[1] pre-fetch -> pre-parse (filter doc)      : %6u B (%5.1f KB)  TLS session = T-0.4 figure (no net here)\n",
                d1_setup, (double)d1_setup / 1024.0);
  Serial.printf("[2] pre-parse -> parse low-water (GATE)      : %6u B (%5.1f KB)  expect small\n",
                d2_parse, (double)d2_parse / 1024.0);
  Serial.printf("[3] PSRAM delta across the parse (doc)       : %6u B (%5.1f KB)  expect ~4.5 KB, <= 8 KB\n",
                d3_doc, (double)d3_doc / 1024.0);
  Serial.printf("[4] post-cleanup vs pre-fetch (leak)         : %6u B (%5.1f KB)  expect < 1 KB (+512 B read buf in scope)\n",
                d4_leak, (double)d4_leak / 1024.0);
  Serial.printf("raw internal free: prefetch=%u pre-parse=%u parse-low-water=%u post-parse=%u post-cleanup=%u\n",
                heap_prefetch, heap_preparse, parse_low_water, heap_postparse, heap_postcleanup);
  Serial.printf("VERDICT: GATE [2] = %s\n",
                (d2_parse < 8192) ? "PASS -- filtered parse is cheap in internal heap"
                                  : "FAIL -- internal parse cost is large; filter is not doing its job");
  Serial.println("T-0.5 DONE");
}

void loop() {
  delay(1000);
}
