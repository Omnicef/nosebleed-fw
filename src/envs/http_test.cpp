// SPDX-License-Identifier: GPL-3.0-only
//
// env:http — T-0.4/T-0.5/T-5.x TLS + filtered ESPN fetch. Moved verbatim out of main.cpp
// (Split test envs). Selected by build_src_filter, not by this #ifdef, but
// the guard is kept so the moved text is byte-identical and a stray compile
// of this file under another env stays inert.

#include "common.h"

#ifdef NB_HTTP_TEST
// T-5.3 — HTTP transport proof (env:httptest). Boot order is the hard rule:
// WiFi -> SNTP -> first TLS (cert notBefore validation needs the clock).
// 1. forced DNS failure: every attempt fails, backoff is honoured, nothing
//    crashes or leaks; 2. NFL scoreboard over TLS (T-04-proven URL): status
//    200 and body bytes counted off HttpStream == Content-Length.
// Internal heap is sampled around each http_get(): esp_http_client_close +
// cleanup run on every path including mid-body failure, so the post-call
// delta must be ~0 — that is the "never leaks a session" measurement.
#include <WiFi.h>
#include <cstdlib>
#include <StreamUtils.h>
#include "http.h"
#include "espn.h"
#include "espn_json.h"
#include "date_window.h"
#include "poll.h"
#include "cache.h"
#include <atomic>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

static uint32_t http_int_free() {
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

// T-5.7 — poll-task device proof (NB_POLL_DEMO). Real topology: this loop
// runs on a task pinned to core 0 (like the shipping poll task); an fps
// probe emulating the render task's 30 fps frame budget runs on core 1 at
// prio 3. Five pro leagues enabled, per-league deadlines via PollScheduler
// (live 20 s / idle 120 s per LeagueConfig, 5 s boot stagger). Accept
// evidence: one full cycle across five leagues with timing, a per-cycle
// internal-heap watermark, max CONCURRENT TLS sessions == 1, and core-1
// fps never below 28 while core 0 does TLS.
static nb::data::DataCache* g_pcache = nullptr;
static std::atomic<int> g_sess{0}, g_sess_max{0}, g_fps_min10{9999};
static volatile uint32_t g_cycle_ms = 0;
static volatile int g_poll_fails = 0;

static void poll_fps_probe(void*) {
    Serial.printf("  [fps] started on core %d\n", xPortGetCoreID());
    uint32_t frames = 0, t0 = millis(), tlog = t0;
    for (;;) {
        ++frames;
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps frame budget
        const uint32_t now = millis();
        if (now - tlog >= 10000) {
            const int fps10 = static_cast<int>(frames * 10000u / (now - tlog));
            if (fps10 < g_fps_min10) g_fps_min10 = fps10;
            Serial.printf("  [fps] %.1f fps (min %.1f)\n", fps10 / 10.0f,
                          g_fps_min10.load() / 10.0f);
            frames = 0;
            tlog = now;
        }
        (void)t0;
    }
}

static void poll_task(void*) {
    using namespace nb::data;
    Serial.printf("  poll: task on core %d, int_free=%u largest=%u\n", xPortGetCoreID(),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    static PollScheduler sch;
    static const int kEnabled[] = {0, 2, 5, 6, 7};  // nfl nba mlb nhl epl
    for (int i = 0; i < nb::config::kLeagueSlugCount; ++i)
        sch.set(i, {false, 20, 120});
    for (int lg : kEnabled) sch.set(lg, {true, 20, 120});
    sch.reset(time(nullptr));

    g_pcache = static_cast<nb::data::DataCache*>(
        heap_caps_calloc(1, sizeof(nb::data::DataCache), MALLOC_CAP_SPIRAM));
    if (!g_pcache) {
        Serial.println("  poll: FAIL — no PSRAM for DataCache");
        ++g_poll_fails;
        vTaskDelete(nullptr);
    }

    bool first_done[8] = {};
    int n_first = 0;
    const uint32_t cycle_t0 = millis();
    uint32_t heap_min = http_int_free();

    const uint32_t t_end = millis() + 150000;
    while (millis() < t_end) {
        const time_t now = time(nullptr);
        for (int lg = 0; lg < nb::config::kLeagueSlugCount; ++lg) {
            if (!sch.due(lg, now)) continue;
            char url[192], dates[16];
            local_day(dates, sizeof dates, now);
            scoreboard_url(url, sizeof url, nb::config::kLeagueApiPaths[lg], dates);
            size_t wire = 0;
            int games = 0;
            bool ok = false, has_live = false;
            const uint32_t t0 = millis();
            const uint32_t f0 = http_int_free();
            ++g_sess;
            if (g_sess > g_sess_max) g_sess_max.store(g_sess.load());
            {
                JsonDocument doc = make_psram_doc();
                ok = espn_fetch_scoreboard(url, doc, &wire);
                if (ok) {
                    GameList* w = g_pcache->writable(lg);
                    w->count = to_games(doc, *w);
                    filter_yesterday_today(w, now);
                    games = w->count;
                    if (games == 0) {  // opportunistic yesterday leg (T-5.6)
                        char yd[16];
                        if (local_yesterday(yd, sizeof yd, now) &&
                            scoreboard_url(url, sizeof url, nb::config::kLeagueApiPaths[lg], yd)) {
                            JsonDocument doc2 = make_psram_doc();
                            if (espn_fetch_scoreboard(url, doc2, &wire)) {
                                w->count = to_games(doc2, *w);
                                games = w->count;
                            }
                        }
                    }
                    for (int i = 0; i < games; ++i)
                        has_live |= (w->games[i].status == kStatusIn);
                    g_pcache->publish(lg, static_cast<int64_t>(now));
                } else {
                    ++g_poll_fails;  // last-good kept, per hard rule
                }
            }
            --g_sess;
            Serial.printf("  poll[%s]: ok=%d games=%d wire=%u %lu ms int_delta=%ld B\n",
                          nb::config::kLeagueSlugs[lg], ok, games,
                          static_cast<unsigned>(wire),
                          static_cast<unsigned long>(millis() - t0),
                          static_cast<long>(f0 - http_int_free()));
            sch.done(lg, time(nullptr), has_live);
            if (!first_done[lg]) {
                first_done[lg] = true;
                if (++n_first == 5) {
                    g_cycle_ms = millis() - cycle_t0;
                    Serial.printf("  poll-cycle: 5 leagues in %lu ms\n",
                                  static_cast<unsigned long>(g_cycle_ms));
                }
            }
            const uint32_t hf = http_int_free();
            if (hf < heap_min) heap_min = hf;
        }
        const time_t t = time(nullptr);
        int64_t sleep_s = sch.next_wake(t) - t;
        if (sleep_s < 1) sleep_s = 1;
        if (sleep_s > 30) sleep_s = 30;
        vTaskDelay(pdMS_TO_TICKS(1000 * sleep_s));
    }

    const bool cycle_ok = g_cycle_ms > 0;
    const bool pass = cycle_ok && g_sess_max <= 1 && g_fps_min10 >= 280;
    Serial.printf("  poll-demo: cycle=%d(%lu ms) sess_max=%d fps_min=%.1f heap_min=%u B fails=%d\n",
                  cycle_ok, static_cast<unsigned long>(g_cycle_ms), g_sess_max.load(),
                  g_fps_min10.load() / 10.0f, heap_min, g_poll_fails);
    Serial.printf("  POLL RESULT: %s\n", pass ? "PASS" : "FAIL");
    vTaskDelete(nullptr);
}

static void http_test() {
    int fails = 0;
    Serial.println("T-5.3 HTTP transport test:");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const uint8_t st = WiFi.waitForConnectResult(20000);
    if (st != WL_CONNECTED) {
        Serial.printf("  FAIL: wifi status=%u\n", static_cast<unsigned>(st));
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    Serial.printf("  wifi %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    time_t now = 0;
    uint32_t waited = 0;
    while (now < 1700000000 && waited < 20000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
        now = time(nullptr);
    }
    if (now < 1700000000) {
        Serial.println("  FAIL: SNTP not synced in 20 s — refusing TLS (hard rule)");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    Serial.printf("  sntp synced in %lu ms (TLS may proceed)\n",
                  static_cast<unsigned long>(waited));

    // 1. forced DNS failure — .invalid is reserved-NXDOMAIN by RFC 6761.
    {
        nb::data::HttpStat stat;
        const uint32_t t0 = millis(), f0 = http_int_free();
        const bool ok = nb::data::http_get(
            "http://nosebleed-nope.invalid./scoreboard.json",
            [](nb::data::HttpStream&) { return true; }, &stat);
        const uint32_t dt = millis() - t0;
        const int32_t d = static_cast<int32_t>(f0 - http_int_free());
        Serial.printf("  dns-fail: ok=%d status=%d elapsed=%lu ms heap_delta=%ld B\n",
                      ok, stat.status, static_cast<unsigned long>(dt),
                      static_cast<long>(d));
        // must fail; must spend >= the 0.5+1.0+2.0 s backoff floor
        if (ok || dt < 3500 || (d > 1024) || (d < -1024)) {
            ++fails;
            Serial.println("    FAIL: expected exhausted retries (>=3.5 s), no heap drift");
        }
    }

    // 2. TLS success — the exact T-0.4 URL. Count every body byte off the
    // stream; cleanup happens inside http_get_once, so measure after it.
    {
        nb::data::HttpStat stat;
        size_t n = 0;
        const uint32_t t0 = millis(), f0 = http_int_free();
        const bool ok = nb::data::http_get(
            "https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard",
            [&](nb::data::HttpStream& s) {
                char buf[512];
                for (;;) {
                    const size_t r = s.readBytes(buf, sizeof buf);
                    if (r == 0) break;
                    n += r;
                }
                return n > 1000;
            },
            &stat);
        const uint32_t dt = millis() - t0;
        const int32_t d = static_cast<int32_t>(f0 - http_int_free());
        Serial.printf("  tls: ok=%d status=%d clen=%lld bytes=%u elapsed=%lu ms heap_delta=%ld B\n",
                      ok, stat.status, static_cast<long long>(stat.clen),
                      static_cast<unsigned>(n), static_cast<unsigned long>(dt),
                      static_cast<long>(d));
        if (!ok || stat.status != 200 || (stat.clen >= 0 && stat.clen != (int64_t)n)) {
            ++fails;
            Serial.println("    FAIL: expected 200 with bytes == Content-Length");
        }
        if (d > 1024 || d < -1024) {
            ++fails;
            Serial.println("    FAIL: session heap not reclaimed (leak)");
        }
    }

    // 3. T-5.4 — filtered ESPN client on the live stream: ReadBufferingStream
    // (512 B, never exercised in the spike) feeding filter + NestingLimit(20)
    // parse. TLS session is already closed here, so the internal delta is the
    // parse alone and must be ~0 — the doc is PSRAM, the buffer is 512 B.
    {
        char url[160];
        if (!nb::data::scoreboard_url(url, sizeof url, "baseball/mlb")) ++fails;
        auto doc = nb::data::make_psram_doc();
        const uint32_t f0 = http_int_free(), t0 = millis();
        const bool ok = nb::data::espn_fetch_scoreboard(url, doc);
        const int32_t d = static_cast<int32_t>(f0 - http_int_free());
        Serial.printf("  espn: ok=%d events=%u kept=%u B internal_delta=%ld B elapsed=%lu ms\n",
                      ok,
                      ok ? static_cast<unsigned>(doc["events"].size()) : 0u,
                      ok ? static_cast<unsigned>(measureJson(doc)) : 0u,
                      static_cast<long>(d), static_cast<unsigned long>(millis() - t0));
        if (!ok) {
            ++fails;
            Serial.println("    FAIL: filtered MLB fetch");
        } else {
            Serial.printf("    spot: id=%s state=%s away=%s\n",
                          doc["events"][0]["id"].as<const char*>(),
                          doc["events"][0]["competitions"][0]["status"]["type"]["state"].as<const char*>(),
                          doc["events"][0]["competitions"][0]["competitors"][0]["team"]["abbreviation"].as<const char*>());
            if (d > 2048) {
                ++fails;
                Serial.println("    FAIL: parse internal delta > 2 KB — filter/allocator wrong");
            }
        }
    }

    // 4. T-5.6 (REQUIRED mitigation) — single local day via ?dates=.
    // Baseline: 6,081 ms / full window unmitigated. Measure over runs;
    // wire time is network-weather (T-5.3/5.4 both saw multi-fold swings).
    {
        char url[192], dates[16];
        setenv("TZ", "America/New_York", 1);  // config default; boot path does this via T-4.6
        tzset();
        const time_t now = time(nullptr);  // SNTP was synced for phase 2
        if (!nb::data::local_day(dates, sizeof dates, now)) ++fails;
        if (!nb::data::scoreboard_url(url, sizeof url, "baseball/mlb", dates)) ++fails;
        Serial.printf("  window: dates=%s tz=%s\n", dates, getenv("TZ") ? getenv("TZ") : "(unset)");
        for (int i = 1; i <= 3; ++i) {
            auto doc = nb::data::make_psram_doc();
            size_t wire = 0;
            const uint32_t t0 = millis();
            const bool ok = nb::data::espn_fetch_scoreboard(url, doc, &wire);
            Serial.printf("  run%d: ok=%d wire=%u B events=%u kept=%u B elapsed=%lu ms\n", i, ok,
                          static_cast<unsigned>(wire),
                          ok ? static_cast<unsigned>(doc["events"].size()) : 0u,
                          ok ? static_cast<unsigned>(measureJson(doc)) : 0u,
                          static_cast<unsigned long>(millis() - t0));
            if (!ok) ++fails;
            if (i < 3) vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    // 5. T-5.6 diagnostic — wire is only ~1.4 s of the ~8 s run (phase 2
    // moves the same-size NFL body in 1.35 s); parse owns the rest. Re-parse
    // ONE captured body from PSRAM memory with filter/doc in either heap to
    // find which allocation is slow. Throwaway; not the shipping path.
    {
        static constexpr size_t kBodyCap = 600u * 1024u;
        char* body = static_cast<char*>(heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM));
        char url[192], dates[16];
        nb::data::local_day(dates, sizeof dates, time(nullptr));
        if (!nb::data::scoreboard_url(url, sizeof url, "baseball/mlb", dates)) ++fails;
        size_t got = 0;
        const uint32_t tw0 = millis();
        const bool fetched =
            body && nb::data::http_get(url, [&](nb::data::HttpStream& s) {
                char buf[1024];
                for (;;) {
                    const size_t r = s.readBytes(buf, sizeof buf);
                    if (r == 0) break;
                    if (got + r > kBodyCap) return false;
                    memcpy(body + got, buf, r);
                    got += r;
                }
                return got > 1000;
            });
        Serial.printf("  parse-diag: fetched=%d body=%u B wire=%lu ms\n", fetched,
                      static_cast<unsigned>(got), static_cast<unsigned long>(millis() - tw0));
        if (fetched) {
            JsonDocument filt_int;
            nb::data::build_scoreboard_filter(filt_int);
            auto filt_psram = nb::data::make_psram_doc();
            nb::data::build_scoreboard_filter(filt_psram);
            for (int v = 0; v < 4; ++v) {
                // v0 = shipping combo (psram filter + psram doc); v3 = raw
                // parse, no filter, tokenizer baseline.
                const bool psram_filter = (v == 0 || v == 2);
                const bool psram_doc = (v == 0 || v == 1);
                auto parse_once = [&]() {
                    JsonDocument doc = psram_doc ? nb::data::make_psram_doc() : JsonDocument();
                    JsonDocument& f = psram_filter ? filt_psram : filt_int;
                    struct MemView {  // reader duck-type: size_t readBytes(char*, size_t)
                        const char* p;
                        size_t n, i = 0;
                        int read() { return i < n ? (uint8_t)p[i++] : -1; }
                        size_t readBytes(char* b, size_t m) {
                            const size_t k = (n - i < m) ? n - i : m;
                            memcpy(b, p + i, k);
                            i += k;
                            return k;
                        }
                    } mv{body, got};
                    const uint32_t f0 = http_int_free(), t0 = micros();
                    const DeserializationError err =
                        (v == 3) ? deserializeJson(doc, mv, DeserializationOption::NestingLimit(20))
                                 : nb::data::parse_scoreboard(mv, f, doc);
                    const uint32_t dt = (micros() - t0) / 1000u;
                    const int32_t d = static_cast<int32_t>(f0 - http_int_free());
                    Serial.printf("    filt=%-5s doc=%-8s: err=%d %lu ms internal_delta=%ld B\n",
                                  (v == 3) ? "none" : (psram_filter ? "psram" : "internal"),
                                  psram_doc ? "psram" : "internal", static_cast<int>(bool(err)),
                                  static_cast<unsigned long>(dt), static_cast<long>(d));
                };
                parse_once();  // warm cache
                parse_once();  // measured pass
            }
        }
        heap_caps_free(body);
    }

    Serial.printf("  RESULT: %s\n", fails ? "FAIL" : "PASS");
    // 6. T-5.7 — real-topology poll proof runs as tasks (poll on core 0,
    // fps probe on core 1); logs POLL RESULT when done.
#ifdef NB_POLL_DEMO
    Serial.printf("  poll-demo boot: int_free=%u largest=%u psram_free=%u\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    xTaskCreatePinnedToCore(poll_fps_probe, "fps", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(poll_task, "poll", 12 * 1024, nullptr, 2, nullptr, 0);
    Serial.println("  poll-demo: tasks created");
#endif
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_HTTP_TEST

void setup() {
    boot_prologue();
    http_test();  // never returns
}
void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }
