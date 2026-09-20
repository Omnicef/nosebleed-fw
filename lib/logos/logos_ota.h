// SPDX-License-Identifier: GPL-3.0-only
//
// T-9.5 — logo OTA. Conditional GET (ETag) of logos.bin into a PSRAM
// buffer, full structural validation BEFORE any partition is touched, then
// erase + write + re-mmap. A rebranded team's art updates without a firmware
// flash; a corrupt or wrong download can never leave the table half-written,
// because nothing is erased until every index entry has been bounds-checked
// against the downloaded bytes.
//
// NVS (namespace "nb", factory-reset-clearable): "logo_url" (https-only),
// "logo_etag". Keys live here, not in the Config blob — the atlas URL is
// not per-render config.
//
// API signatures verified against
// ~/esp/esp-idf-v5.5.5/components/esp_partition/include/esp_partition.h
// (write/erase_range → esp_err_t, munmap → void) and the event struct in
// esp_http_client.h.

#pragma once

#ifdef ESP_PLATFORM

#include <atomic>
#include <cstdint>
#include <cstring>
#include <strings.h>

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "http.h"
#include "logos_esp.h"

namespace nb {
namespace logos {

namespace ota {

constexpr size_t kCap = 0x200000;  // == logos partition size (partitions.csv)

// Set by /api/logos/update; polled (and consumed) by the poll task.
inline std::atomic<bool>& requested() {
    static std::atomic<bool> v{false};
    return v;
}

inline bool set_url(const char* url) {
    if (strncmp(url, "https://", 8) != 0) return false;
    Preferences p;
    String old;
    if (p.begin("nb", true)) {
        old = p.getString("logo_url");
        p.end();
    }
    if (!p.begin("nb")) return false;
    p.putString("logo_url", url);
    // Drop the ETag only on a real source change — a manual "check now"
    // against the same URL must still be a conditional GET.
    if (strcmp(url, old.c_str()) != 0) p.remove("logo_etag");
    p.end();
    requested() = true;
    return true;
}

// One atlas blob must be self-consistent before it may touch flash.
inline bool validate(const uint8_t* buf, size_t len) {
    Header h;
    if (!parse_header(buf, h) || h.count == 0) return false;
    if (kHeaderSize + static_cast<size_t>(h.count) * kEntrySize > len) return false;
    for (uint16_t i = 0; i < h.count; ++i) {
        const Entry e = entry_at(buf, i);
        if (e.w == 0 || e.h == 0 || e.w > 128 || e.h > 128) return false;
        const size_t blob = static_cast<size_t>(e.w) * e.h * 2 + (e.w * e.h + 7) / 8;
        if (static_cast<size_t>(e.offset) + blob > len) return false;
    }
    return true;
}

// 1 updated · 0 unchanged or not configured · -1 failed. Runs on the poll
// task only (single TLS-session discipline; the flash write yields every
// 64 KB so the DMA and render tasks never starve).
inline int check() {
    Preferences p;
    String url, etag;
    if (p.begin("nb", true)) {
        url = p.getString("logo_url");
        etag = p.getString("logo_etag");
        p.end();
    }
    if (url.length() == 0) return 0;
    Serial.printf("[logos] check url=%s have-etag=%u\n", url.c_str(), etag.length());

    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM));
    if (!buf) return -1;
    size_t got = 0;
    data::HttpStat st;
    const bool ok = data::http_get_once(
        url.c_str(),
        [&](data::HttpStream& s) {
            while (got < kCap) {
                const size_t r = s.readBytes(reinterpret_cast<char*>(buf) + got, 4096);
                if (r == 0) break;
                got += r;
            }
            return true;  // real verdict is clen + validate() below
        },
        &st, etag.length() ? "If-None-Match" : nullptr, etag.c_str());
    Serial.printf("[logos] status=%d got-etag=%s\n", st.status, st.etag);
    if (st.status == 304 || (ok && st.status == 200 && etag.length() && st.etag[0] &&
                             strcmp(st.etag, etag.c_str()) == 0)) {
        free(buf);
        return 0;  // server confirmed our copy
    }
    if (!ok || st.status != 200 || st.clen <= 0 || st.clen > static_cast<int64_t>(kCap) ||
        got != static_cast<size_t>(st.clen)) {
        free(buf);  // non-200 / absent-or-mismatched length: truncated body
        return -1;
    }
    if (!validate(buf, got)) {
        free(buf);
        return -1;
    }

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "logos");
    if (!part || part->size < kCap) {
        free(buf);
        return -1;
    }

    // Past this point the old table is forfeit on failure — init_mmap below
    // then leaves ready=false and every card falls back to abbreviations
    // (T-3.6) until the next successful check. Erase/write are 4 KiB-chunked
    // with a tick every 64 KB: ~20 yields over the swap, poll is not latency
    // sensitive.
    Table& t = table();
    if (t.ready) {
        esp_partition_munmap(t.handle);
        t = Table{};
    }
    bool swapped = false;
    do {
        bool err = false;
        for (size_t o = 0; o < part->size && !err; o += 65536) {
            if (esp_partition_erase_range(part, o, 65536) != ESP_OK) err = true;
            vTaskDelay(1);
        }
        for (size_t o = 0; o < got && !err; o += 4096) {
            const size_t n = (got - o < 4096) ? got - o : 4096;
            if (esp_partition_write(part, o, buf + o, n) != ESP_OK) err = true;
            if ((o & 0xFFFF) == 0) vTaskDelay(1);
        }
        if (err) break;
        if (!init_mmap()) break;
        swapped = memcmp(t.base, buf, got) == 0;  // re-read through the new map
        if (!swapped) {
            esp_partition_munmap(t.handle);
            t = Table{};
        }
    } while (0);
    free(buf);
    if (!swapped) {
        init_mmap();  // re-map whatever survived; invalid header ⇒ ready=false
        return -1;
    }
    if (p.begin("nb")) {
        if (st.etag[0]) p.putString("logo_etag", st.etag);
        p.end();
    }
    return 1;
}

}  // namespace ota
}  // namespace logos
}  // namespace nb

#endif  // ESP_PLATFORM
