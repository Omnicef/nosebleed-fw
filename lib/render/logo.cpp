// SPDX-License-Identifier: GPL-3.0-only
//
// T-3.5/T-3.6 implementation. See logo.h for the paste(mask) equivalence
// argument and the fallback layout provenance.

#include "logo.h"

namespace nb {

bool blit_logo(Canvas16& c, int x, int y, const LogoArt& a) {
    if (a.px == nullptr || a.mask == nullptr || a.w <= 0 || a.h <= 0)
        return false;
    const int stride = (a.w + 7) / 8;
    for (int row = 0; row < a.h; ++row) {
        for (int col = 0; col < a.w; ++col) {
            const int i = row * a.w + col;
            if (!(a.mask[row * stride + (col >> 3)] & (0x80 >> (col & 7))))
                continue;  // transparent: destination survives
            c.set(x + col, y + row, a.px[i]);
        }
    }
    return true;
}

int draw_abbr_fallback(Canvas16& c, int x, int y, int logo_h, const char* abbr,
                       uint16_t color, const Font& font) {
    char buf[4] = {abbr[0], abbr[1], abbr[2], '\0'};  // Python: abbreviation[:3]
    const int len = static_cast<int>(buf[0] ? (buf[1] ? (buf[2] ? 3 : 2) : 1) : 0);
    const int text_y = y + (logo_h - font.box_h) / 2;  // Python: (logo_h - 8) // 2
    draw_text(c, font, x, text_y, buf, color);
    return text_width(font, len);
}

bool parse_hex565(const char* hex, uint16_t& out) {
    if (hex == nullptr) return false;
    if (*hex == '#') ++hex;
    unsigned v[3] = {0, 0, 0};
    for (int comp = 0; comp < 3; ++comp) {
        unsigned acc = 0;
        for (int d = 0; d < 2; ++d) {
            const char ch = *hex++;
            int val;
            if (ch >= '0' && ch <= '9') val = ch - '0';
            else if (ch >= 'a' && ch <= 'f') val = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') val = ch - 'A' + 10;
            else return false;
            acc = acc * 16 + static_cast<unsigned>(val);
        }
        v[comp] = acc;
    }
    out = rgb565(static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]),
                 static_cast<uint8_t>(v[2]));
    return true;
}

}  // namespace nb
