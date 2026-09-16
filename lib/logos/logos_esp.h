// SPDX-License-Identifier: GPL-3.0-only
//
// T-3.4/T-3.7 — device side of the logo reader: locate the `logos`
// partition, mmap it once, expose a phase-checked lookup. Include from ESP
// translation units only (logos.h defines the pure core both sides share).
//
// esp_partition_mmap signature verified against
// ~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/
// esp_partition/include/esp_partition.h:
//   esp_err_t esp_partition_mmap(const esp_partition_t*, size_t offset,
//       size_t size, esp_partition_mmap_memory_t, const void**,
//       esp_partition_mmap_handle_t*);   // handle is uint32_t

#pragma once

#ifdef ESP_PLATFORM

#include <esp_err.h>
#include <esp_partition.h>

#include "logos.h"

namespace nb {
namespace logos {

struct Table {
    const uint8_t* base = nullptr;  // flash-mapped, byte-readable
    size_t count = 0;
    uint16_t logo_height = 0;
    esp_partition_mmap_handle_t handle = 0;
    bool ready = false;
};

inline Table& table() {
    static Table t;
    return t;
}

// Map the whole partition read-only. Maps VA pages — allocates NO heap, and
// the index/blob reads below never allocate either (T-3.4 accept: heap
// before == heap after). Returns false with a reason on any failure; the
// caller must then run the abbreviation fallback for everything (T-3.6).
inline bool init_mmap(const char* label = "logos") {
    Table& t = table();
    if (t.ready) return true;
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), label);
    if (p == nullptr) return false;
    const void* ptr = nullptr;
    const esp_err_t err = esp_partition_mmap(
        p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &t.handle);
    if (err != ESP_OK) return false;
    t.base = static_cast<const uint8_t*>(ptr);
    Header h;
    if (!parse_header(t.base, h)) return false;
    if (kHeaderSize + static_cast<size_t>(h.count) * kEntrySize > p->size)
        return false;  // truncated/garbage table
    t.count = h.count;
    t.logo_height = h.logo_height;
    t.ready = true;
    set_phase(Phase::BOOTSTRAP);
    return true;
}

// The one legal lookup entry point (asserts the T-3.7 phase discipline).
inline bool lookup(const char* league, const char* abbr, Ref& out) {
    assert_lookup_allowed();
    if (!table().ready) return false;
    return find(table().base, table().count, league, abbr, out);
}

}  // namespace logos
}  // namespace nb

#endif  // ESP_PLATFORM
