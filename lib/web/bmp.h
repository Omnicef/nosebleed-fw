// SPDX-License-Identifier: GPL-3.0-only
//
// T-8.6 — BMP header writer, split out so the host suite can pin the exact
// bytes (web.cpp runs only on device). 24-bit bottom-up BI_RGB:
// BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40), rows padded to 4.

#pragma once

#include <cstdint>

namespace nb {
namespace web {

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

inline void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = v >> 24;
}

// The magic is two ASCII bytes, NOT a little-endian u16: put16(0x424D)
// writes 4D 42 — "MB" — every field valid, every browser rejects it.
// Measured on the live dashboard; keep it characters.
inline uint32_t bmp_row(uint32_t w) { return (w * 3 + 3) & ~3U; }  // pad to 4

inline void write_bmp_header(uint8_t* b, uint32_t w, uint32_t h) {
    const uint32_t img = bmp_row(w) * h;
    b[0] = 'B';
    b[1] = 'M';
    put32(b + 2, 54 + img);  // bfSize
    put32(b + 10, 54);       // bfOffBits
    put32(b + 14, 40);       // biSize
    put32(b + 18, w);
    put32(b + 22, h);        // positive: bottom-up rows
    put16(b + 26, 1);        // biPlanes
    put16(b + 28, 24);       // biBitCount
    put32(b + 34, img);      // biSizeImage
    put32(b + 38, 2835);     // ~72 dpi
    put32(b + 42, 2835);
}

}  // namespace web
}  // namespace nb
