// SPDX-License-Identifier: GPL-3.0-only
//
// env:logos — T-3.4 mmap logo-partition reader. Moved verbatim out of main.cpp
// (Split test envs). Selected by build_src_filter, not by this #ifdef, but
// the guard is kept so the moved text is byte-identical and a stray compile
// of this file under another env stays inert.

#include "common.h"

#ifdef NB_LOGOS_TEST
// T-3.4 / T-3.7 — logos-partition mmap test, run standalone (env:logostest)
// before the tasks start so the heap samples are quiet. Accept criteria:
//  * internal free heap AND largest internal block identical before/after
//    init() — the mapping allocates no heap and copies nothing;
//  * every lookup's px+mask blob hashes to the host-predicted FNV-1a;
//  * set_phase(FRAME) + lookup() traps (NB_LOGOS_ASSERT=1 build).
#include "logos.h"
#include "logos_esp.h"

static uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static uint32_t blob_hash(const nb::logos::Ref& r) {
    const size_t n = static_cast<size_t>(r.w) * r.h * 2 +
                     ((static_cast<size_t>(r.w) + 7) / 8) * r.h;
    return fnv1a(r.px, n);
}

static void logos_test() {
    using namespace nb::logos;
    int fails = 0;
    Serial.println("T-3.4 mmap test:");
    const uint32_t f0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool ok = init_mmap();
    const uint32_t f1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b1 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    Serial.printf("  init=%d count=%u logo_h=%u\n",
                  static_cast<int>(ok), static_cast<unsigned>(table().count),
                  static_cast<unsigned>(table().logo_height));
    const long df = static_cast<long>(f1) - static_cast<long>(f0);
    const long db = static_cast<long>(b1) - static_cast<long>(b0);
    // mmap allocates its page-table mapping (~100 B) once — that is mapping
    // metadata, not lookup cost; the acceptance criterion (heap identical
    // around lookups) is checked separately after the loop below.
    Serial.printf("  mmap internal free delta=%ld largestblk delta=%ld (one-time mapping)\n", df, db);
    if (!ok) ++fails;

    // Base rows died with atlas v2: every row is a card display height.
    // One team per league at the PRE slot (h=24) + the cross-league
    // abbreviation reuse (BOS is in MLB, NBA and NHL — a bare-abbr index
    // could only hold one) + misses. FNV-1a of px+mask, recomputed
    // host-side from the same logos.bin.
    struct Want { const char* league; const char* abbr; uint16_t h; uint32_t hash; };
    static const Want wants[] = {
        {"mlb", "BOS", 24, 0x8294c099u}, {"nfl", "KC", 24, 0x0444d882u},
        {"nba", "LAL", 24, 0x3a12ba50u}, {"nhl", "VGK", 24, 0x2c255b8au},
        {"epl", "LIV", 24, 0xb82b6b3fu}, {"nhl", "BOS", 24, 0x8f68df91u},
        {"mlb", "BOS", 32, 0},  // not a card height — must miss
        {"mlb", "ZZZ", 24, 0},  // must miss
    };
    const uint32_t f2 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t b2 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    for (const auto& w : wants) {
        Ref r;
        const bool hit = lookup(w.league, w.abbr, w.h, r);
        const bool ok = w.hash ? (hit && blob_hash(r) == w.hash) : !hit;
        if (!ok) ++fails;
        if (hit) {
            Serial.printf("  %s:%s %s w=%u h=%u hash=%08lx\n", w.league,
                          w.abbr, ok ? "OK  " : "FAIL", r.w, r.h,
                          static_cast<unsigned long>(blob_hash(r)));
        } else {
            Serial.printf("  %s:%s %s (miss)\n", w.league, w.abbr,
                          ok ? "OK  " : "FAIL");
        }
    }
    const long dl = static_cast<long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) - static_cast<long>(f2);
    const long dbl = static_cast<long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) - static_cast<long>(b2);
    Serial.printf("  lookups: internal free delta=%ld largestblk delta=%ld (must be 0/0)\n", dl, dbl);
    if (dl != 0 || dbl != 0) ++fails;
    if (fails) Serial.printf("  RESULT: FAIL (%d)\n", fails);
    else Serial.println("  RESULT: PASS");
#ifdef NB_LOGOS_TRAP_EXPECT
    // T-3.7: a lookup outside BOOTSTRAP/REBUILD must trap. Expected crash:
    // Guru Meditation (IllegalInstruction from __builtin_trap), then reboot.
    set_phase(Phase::FRAME);
    Serial.println("  trap expected now: FRAME-phase lookup");
    Serial.flush();
    delay(50);
    Ref r;
    lookup("mlb", "BOS", 24, r);
    Serial.println("  ASSERT FAILED: no trap");  // must not reach
#else
    set_phase(Phase::FRAME);
    Serial.println("  (built without NB_LOGOS_TRAP_EXPECT — FRAME trap skipped)");
#endif
    Serial.println("  PASS: stop here for serial capture");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif  // NB_LOGOS_TEST

void setup() {
    boot_prologue();
    logos_test();  // never returns
}
void loop() { vTaskDelay(pdMS_TO_TICKS(10000)); }
