// SPDX-License-Identifier: GPL-3.0-only
//
// env:cache — T-5.2 DataCache pointer-swap stress. Moved verbatim out of main.cpp
// (Split test envs). Selected by build_src_filter, not by this #ifdef, but
// the guard is kept so the moved text is byte-identical and a stray compile
// of this file under another env stays inert.

#include "common.h"

#ifdef NB_CACHE_TEST
#include "cache.h"
#include <cstring>
#include <cstdio>

using nb::data::DataCache;
using nb::data::Game;
using nb::data::GameList;
using nb::data::kMaxGamesPerLeague;

static void fill_generation(GameList* l, uint32_t gen) {
    l->count = 1 + static_cast<int>(gen % kMaxGamesPerLeague);
    char id[16];
    std::snprintf(id, sizeof id, "%lu", static_cast<unsigned long>(gen));
    for (int i = 0; i < l->count; i++) {
        Game& g = l->games[i];
        nb::data::copy_str(g.id, sizeof g.id, id);
        nb::data::copy_str(g.status_display, sizeof g.status_display, "Gen");
        g.period = static_cast<int16_t>(gen % 1000);
        g.away_score = static_cast<int16_t>(gen % 10);
        g.home_score = static_cast<int16_t>(gen % 100);
        g.start_utc = gen;
        g.situation.outs = static_cast<int16_t>(gen % 4);
        g.away.colour = gen * 2654435761u;
    }
}

static int check_generation(const GameList& l) {
    if (l.count < 1 || l.count > kMaxGamesPerLeague) return -1;
    const uint32_t gen = static_cast<uint32_t>(l.games[0].start_utc);
    char id[16];
    std::snprintf(id, sizeof id, "%lu", static_cast<unsigned long>(gen));
    if (l.count != 1 + static_cast<int>(gen % kMaxGamesPerLeague)) return static_cast<int>(gen);
    for (int i = 0; i < l.count; i++) {
        const Game& g = l.games[i];
        if (g.start_utc != (int64_t)gen || std::strcmp(g.id, id) != 0 ||
            g.period != (int16_t)(gen % 1000) || g.away_score != (int16_t)(gen % 10) ||
            g.home_score != (int16_t)(gen % 100) || g.situation.outs != (int16_t)(gen % 4) ||
            g.away.colour != gen * 2654435761u)
            return static_cast<int>(gen);
    }
    return 0;
}

static DataCache* g_sc = nullptr;
static GameList *g_ctl = nullptr, *g_snap = nullptr;
static std::atomic<bool> g_ctl_stop{false};
static std::atomic<int> g_torn{0}, g_ctl_torn{0}, g_reads{0}, g_retry{0}, g_maxbusy{0};
static std::atomic<int> g_hm_torn{0}, g_hm_retries{0};
static std::atomic<uint32_t> g_ctl_gen{0}, g_str_gen{0};  // max gen the core-1 reader saw

static void ctl_writer(void*) {  // forbidden pattern: mutate in place
    uint32_t gen = 1;
    while (!g_ctl_stop.load()) {
        fill_generation(g_ctl, ++gen);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelete(nullptr);
}

static void ctl_reader(void*) {  // holds the pointer across the read
    volatile uint32_t spin = 0;
    while (!g_ctl_stop.load()) {
        GameList* held = g_ctl;  // no swap, no seqlock: same object
        if (static_cast<uint32_t>(held->games[0].start_utc) > g_ctl_gen.load())
            g_ctl_gen.store(static_cast<uint32_t>(held->games[0].start_utc));
        int saw = -1;
        for (int i = 0; i < held->count; i++) {
            const int64_t g = held->games[i].start_utc;
            for (uint32_t k = 0; k < 60000; k++) spin += k;  // dwell ~ms, longer than a fill
            if (g != saw && saw != -1) {
                g_ctl_torn.fetch_add(100);
                break;
            }
            saw = static_cast<int>(g);
        }
        if (check_generation(*held) != 0) g_ctl_torn.fetch_add(1);
        vTaskDelay(1);
    }
    vTaskDelete(nullptr);
}

static void str_writer(void*) {  // poll role: core 0 prio 2, 50 Hz x 4 leagues
    uint32_t gen = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 180000) {
        for (int l = 0; l < 4; l++) {
            GameList* b = g_sc->writable(l);
            fill_generation(b, ++gen);
            g_sc->publish(l, gen);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(nullptr);
}

static void str_reader(void*) {  // render role: core 1 prio 3, 30 Hz snapshots
    const uint32_t t0 = millis();
    while (millis() - t0 < 180000) {
        vTaskDelay(pdMS_TO_TICKS(33));
        if (!g_sc->snapshot(0, g_snap)) {
            const int b = ++g_retry;
            if (b > g_maxbusy.load()) g_maxbusy.store(b);
            continue;
        }
        ++g_reads;
        if (static_cast<uint32_t>(g_snap->games[0].start_utc) > g_str_gen.load())
            g_str_gen.store(static_cast<uint32_t>(g_snap->games[0].start_utc));
        if (check_generation(*g_snap) != 0) ++g_torn;
    }
    vTaskDelete(nullptr);
}

// Hammer phase: same protocol, adversarial contention — writer max-rates
// league 0 (real 50 Hz cadence gives ~1 expected retry in 180 s, too few
// to prove the retry branch ever fires on Xtensa). Torn must stay 0.
static std::atomic<bool> g_hm_done{false};
static void hm_writer(void*) {
    uint32_t gen = 900000, n = 0;
    while (!g_hm_done.load()) {
        GameList* b = g_sc->writable(0);
        fill_generation(b, ++gen);
        g_sc->publish(0, gen);
        if (++n % 200 == 0) vTaskDelay(1);  // keep core 0 idle fed (TASK_WDT)
    }
    vTaskDelete(nullptr);
}

static void hm_reader(void*) {
    uint32_t n = 0;
    while (!g_hm_done.load()) {
        if (++n % 50 == 0) vTaskDelay(1);  // unconditional: a retry storm must not starve core 1
        if (!g_sc->snapshot(0, g_snap)) {
            ++g_hm_retries;
            continue;
        }
        if (check_generation(*g_snap) != 0) ++g_hm_torn;
    }
    vTaskDelete(nullptr);
}

static void cache_test() {
    Serial.println("T-5.2 device seqlock stress (cachetest):");
    g_sc = static_cast<DataCache*>(heap_caps_calloc(1, sizeof(DataCache), MALLOC_CAP_SPIRAM));
    g_ctl = static_cast<GameList*>(heap_caps_calloc(1, sizeof(GameList), MALLOC_CAP_INTERNAL));
    g_snap = static_cast<GameList*>(heap_caps_calloc(1, sizeof(GameList), MALLOC_CAP_INTERNAL));
    if (!g_sc || !g_ctl || !g_snap) { Serial.println("CACHE RESULT: FAIL (alloc)"); for (;; vTaskDelay(100)); }
    fill_generation(g_ctl, 1);

    xTaskCreatePinnedToCore(ctl_reader, "cr", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(ctl_writer, "cw", 3 * 1024, nullptr, 2, nullptr, 0);
    const uint32_t t0 = millis();
    while (!g_ctl_torn.load() && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(5));
    g_ctl_stop.store(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    Serial.printf("  control (in-place mutation): torn=%d last_gen=%lu after %lu ms\n",
                  g_ctl_torn.load(), static_cast<unsigned long>(g_ctl_gen.load()),
                  static_cast<unsigned long>(millis() - t0));
    const bool teeth = g_ctl_torn.load() > 0;
    const bool vis_ctl = g_ctl_gen.load() > 1;  // did core 1 see core 0's RAM writes at all?

    xTaskCreatePinnedToCore(hm_reader, "hr", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(hm_writer, "hw", 3 * 1024, nullptr, 2, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(10000));
    g_hm_done.store(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    Serial.printf("  hammer (max-rate league 0, 10 s): retries=%d torn=%d\n", g_hm_retries.load(),
                  g_hm_torn.load());

    xTaskCreatePinnedToCore(str_reader, "sr", 3 * 1024, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(str_writer, "sw", 3 * 1024, nullptr, 2, nullptr, 0);
    uint32_t tlog = millis();
    const uint32_t t_end = tlog + 185000;  // fixed anchor — tlog re-anchors each print
    while (static_cast<int32_t>(t_end - millis()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (millis() - tlog >= 30000) {
            tlog = millis();
            Serial.printf("  stress: reads=%d retries=%d max_busy=%d torn=%d last_gen=%lu int_free=%u\n",
                          g_reads.load(), g_retry.load(), g_maxbusy.load(), g_torn.load(),
                          static_cast<unsigned long>(g_str_gen.load()),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        }
    }
    const bool pass = teeth && vis_ctl && g_torn.load() == 0 && g_reads.load() > 1000 &&
                      g_hm_retries.load() > 0 && g_hm_torn.load() == 0 &&
                      g_str_gen.load() > 8000;  // ~50 Hz x 180 s of publishes seen
    Serial.printf("CACHE RESULT: %s torn=%d reads=%d retries=%d max_busy=%d control_torn=%d "
                  "ctl_gen=%lu str_gen=%lu hm_retries=%d hm_torn=%d\n",
                  pass ? "PASS" : "FAIL", g_torn.load(), g_reads.load(), g_retry.load(),
                  g_maxbusy.load(), g_ctl_torn.load(),
                  static_cast<unsigned long>(g_ctl_gen.load()),
                  static_cast<unsigned long>(g_str_gen.load()), g_hm_retries.load(),
                  g_hm_torn.load());
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_CACHE_TEST

void setup() {
    boot_prologue();
    cache_test();  // never returns
}
void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }
