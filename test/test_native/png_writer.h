// SPDX-License-Identifier: GPL-3.0-only
//
// Header-only PNG writer for the native render tests. Writes 8-bit RGB using
// a single uncompressed (stored) DEFLATE block — no zlib dependency, ~40 lines.
// Data size stays far below the 65 KB block limit for our small cards.

#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

namespace nb {

inline uint32_t png_crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(v >> 24); out.push_back(v >> 16); out.push_back(v >> 8); out.push_back(v);
}

inline void png_chunk(std::vector<uint8_t>& out, const char* type,
                      const std::vector<uint8_t>& data) {
    std::vector<uint8_t> body;
    body.insert(body.end(), type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    put_be32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, png_crc32(body.data(), body.size()));
}

inline uint32_t adler32(const std::vector<uint8_t>& d) {
    uint32_t a = 1, b = 0;
    for (uint8_t x : d) { a = (a + x) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

// rgb: w*h*3 bytes, row-major. Returns true on success.
inline bool write_png_rgb(const char* path, int w, int h, const uint8_t* rgb) {
    std::vector<uint8_t> raw;
    const size_t stride = static_cast<size_t>(w) * 3;
    raw.reserve((stride + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgb + y * stride, rgb + (y + 1) * stride);
    }

    std::vector<uint8_t> idat;
    idat.push_back(0x78); idat.push_back(0x01);  // zlib header (stored)
    const uint16_t len = static_cast<uint16_t>(raw.size());
    idat.push_back(0x01);                          // final block, stored
    idat.push_back(len & 0xFF); idat.push_back(len >> 8);
    const uint16_t nlen = static_cast<uint16_t>(~len);
    idat.push_back(nlen & 0xFF); idat.push_back(nlen >> 8);
    idat.insert(idat.end(), raw.begin(), raw.end());
    const uint32_t ad = adler32(raw);
    idat.push_back(ad >> 24); idat.push_back(ad >> 16); idat.push_back(ad >> 8); idat.push_back(ad);

    std::vector<uint8_t> out;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    put_be32(ihdr, w); put_be32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2);  // bit depth 8, colour type 2 (RGB)
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", idat);
    png_chunk(out, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return wrote == out.size();
}

}  // namespace nb
