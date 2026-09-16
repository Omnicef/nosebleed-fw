// SPDX-License-Identifier: GPL-3.0-only
//
// T-2.1 — RGB565 canvas. Framework-agnostic (no Arduino/ESP/FreeRTOS). A
// Canvas16 is a view over a caller-owned pixel buffer; canvas_alloc/free route
// through an injectable allocator so the device can hand us PSRAM while the
// host test uses malloc. See AGENTS.md purity rules.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nb {

// RGB565 pack/unpack. The golden PIL renders are quantised to this exact pack
// (r>>3, g>>2, b>>3) before comparison, so the parity domain is RGB565 — what
// the HUB75 panel actually receives. Unpack replicates the high bits so a
// 5/6-bit value expands to the nearest 8-bit value.
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline void unpack565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const unsigned r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

struct Canvas16 {
    uint16_t* px = nullptr;
    uint16_t w = 0;
    uint16_t h = 0;

    bool valid() const { return px != nullptr && w > 0 && h > 0; }

    // Bounds-checked: out-of-range writes are dropped, reads read black.
    void set(int x, int y, uint16_t c) {
        if (static_cast<unsigned>(x) < w && static_cast<unsigned>(y) < h)
            px[y * w + x] = c;
    }
    uint16_t get(int x, int y) const {
        if (static_cast<unsigned>(x) < w && static_cast<unsigned>(y) < h)
            return px[y * w + x];
        return 0;
    }
};

using AllocFn = void* (*)(size_t);
using FreeFn = void (*)(void*);

// Override once at boot to back canvas_alloc with PSRAM. Defaults to malloc.
void canvas_set_allocator(AllocFn alloc, FreeFn free_fn);

// Zeroed (black) w*h RGB565 buffer. Returns an invalid canvas on failure.
Canvas16 canvas_alloc(uint16_t w, uint16_t h);
void canvas_free(Canvas16& c);

}  // namespace nb
