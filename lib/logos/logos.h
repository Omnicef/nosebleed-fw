// SPDX-License-Identifier: GPL-3.0-only
//
// T-3.4 — logos.bin mmap reader. Maps the `logos` data partition with
// esp_partition_mmap and binary-searches the (league, abbr) index in place:
// zero allocation, zero copy (accept: heap before == heap after). Returned
// pointers are raw flash-mapped addresses — valid only for byte reads, and
// every miss through here costs a real SPI flash read, which is why lookup
// is restricted to strip rebuilds (T-3.7 assert below).
//
// Layout mirrors tools/build_logos.py exactly (PLAN §3 / T-3.2): LE, 12 B
// header, 22 B sorted index entries — struct "<8s4sIHHH" is 8+4+4+2+2+2 = 22,
// the trailing rsvd u16 is part of the stride (wrong here desyncs every
// lookup past entry 0; test_native catches it against the real atlas).
// NO packed structs — the on-disk u32 offset is only 2-byte aligned, so
// fields are decoded byte-wise (the S3 tolerates
// unaligned loads but the mapping is flash; byte reads are the honest model
// and host-portable on top). The pure core compiles on the host too —
// test_native reads the same logos.bin from a malloc'd buffer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nb {
namespace logos {

constexpr uint32_t kMagic = 0x474C424Eu;  // "NBLG" little-endian
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 12;
constexpr size_t kEntrySize = 22;

struct Header {
    uint16_t version;
    uint16_t count;
    uint16_t logo_height;
};

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool parse_header(const uint8_t* base, Header& h) {
    if (rd32(base) != kMagic) return false;
    h.version = rd16(base + 4);
    h.count = rd16(base + 6);
    h.logo_height = rd16(base + 8);
    return h.version == kVersion;
}

// A decoded index row. Entry i lives at base + kHeaderSize + i*kEntrySize.
struct Entry {
    char league[9];  // NUL-terminated copy of the 8 padded bytes
    char abbr[5];    // NUL-terminated copy of the 4 padded bytes
    uint32_t offset;
    uint16_t w;
    uint16_t h;
};

inline Entry entry_at(const uint8_t* base, size_t i) {
    const uint8_t* p = base + kHeaderSize + i * kEntrySize;
    Entry e;
    memcpy(e.league, p, 8);
    e.league[8] = '\0';
    memcpy(e.abbr, p + 8, 4);
    e.abbr[4] = '\0';
    e.offset = rd32(p + 12);
    e.w = rd16(p + 16);
    e.h = rd16(p + 18);
    return e;
}

inline size_t blob_size(const Entry& e) {
    return static_cast<size_t>(e.w) * e.h * 2 +
           ((static_cast<size_t>(e.w) + 7) / 8) * e.h;
}

// Comparison against the raw padded key fields — the order build_logos.py
// sorted in (memcmp of NUL-padded fields preserves Python tuple order).
inline int key_cmp(const char* la, const char* aa, const uint8_t* p) {
    int c = strncmp(la, reinterpret_cast<const char*>(p), 8);
    if (c != 0) return c;
    return strncmp(aa, reinterpret_cast<const char*>(p + 8), 4);
}

struct Ref {
    const uint8_t* px;    // w*h RGB565 LE, flash-mapped
    const uint8_t* mask;  // ceil(w/8)*h bytes, MSB-first, 1 = opaque
    uint16_t w;
    uint16_t h;
};

// Binary search over `count` sorted entries. False on miss. Pure pointer
// arithmetic — no allocation, no copy, on any path.
inline bool find(const uint8_t* base, size_t count,
                 const char* league, const char* abbr, Ref& out) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const uint8_t* p = base + kHeaderSize + mid * kEntrySize;
        const int c = key_cmp(league, abbr, p);
        if (c == 0) {
            const Entry e = entry_at(base, mid);
            out.px = base + e.offset;
            out.mask = out.px + static_cast<size_t>(e.w) * e.h * 2;
            out.w = e.w;
            out.h = e.h;
            return true;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return false;
}

// T-3.7 — flash-access discipline. Logo lookups are legal in BOOTSTRAP
// (device test) and REBUILD (strip composition) only. The per-frame path
// must never call find(); calling set_phase(FRAME) before frames start
// turns any stray lookup into a hard abort — opt in with
// -DNB_LOGOS_ASSERT=1 (the Arduino release prebuilts define NDEBUG, so
// plain assert() would not survive there).
enum class Phase : uint8_t { NEVER, BOOTSTRAP, REBUILD, FRAME };

#ifdef NB_LOGOS_ASSERT
inline Phase& phase_ref() {
    static Phase p = Phase::NEVER;
    return p;
}
inline void set_phase(Phase p) { phase_ref() = p; }
// Checked by the CALLING lookup wrapper (logos_esp::lookup on device), so
// the raw pure find() stays free for the host test.
inline void assert_lookup_allowed() {
    const Phase p = phase_ref();
    if (p != Phase::BOOTSTRAP && p != Phase::REBUILD) __builtin_trap();
}
#else
inline void set_phase(Phase) {}
inline void assert_lookup_allowed() {}
#endif

}  // namespace logos
}  // namespace nb
