// SPDX-License-Identifier: GPL-3.0-only
//
// T-3.5 / T-3.6 — masked logo blit and abbreviation fallback. Pure render
// layer (purity rules apply): it sees decoded logo bytes, never the flash
// or the partition. lib/logos fills a LogoArt from the mmap'd atlas; this
// draws it.
//
// The blit is the C equivalent of the Python's
//   card.paste(logo, (x, y), mask=logo)
// on threshold-binary alpha: mask bit set -> copy the RGB565 pixel, clear ->
// keep the destination. That is an exact alpha test (0 or 255, nothing in
// between) — pixel-parity checkable against Pillow (test_native).

#pragma once

#include <cstdint>

#include "canvas.h"
#include "font.h"
#include "font_data.h"

namespace nb {

struct LogoArt {
    const uint16_t* px;    // w*h RGB565 host-order
    const uint8_t* mask;   // ceil(w/8)*h rows, MSB-first, 1 = opaque
    int w;
    int h;
};

// Blit with alpha test at (x, y) top-left; clips via Canvas16 bounds.
// Returns false on a degenerate ref without touching the canvas.
bool blit_logo(Canvas16& c, int x, int y, const LogoArt& a);

// T-3.6 — no logo in the atlas: abbreviation (max 3 chars, as the Python
// truncates) in team colour, spleen-5x8, vertically centred in a logo_h
// band at (x, y) — layout ported from the Python's _paste_logo fallback.
// Returns the pixel width consumed (fixed-width text plus Python's +1 box).
int draw_abbr_fallback(Canvas16& c, int x, int y, int logo_h, const char* abbr,
                       uint16_t color, const Font& font = FONT_SPLEEN_5X8);

// ESPN sends team colours as bare RGB hex ("aa182c"); the '#' is optional.
// Parse straight to RGB565 (the Python's _hex_to_rgb + canvas quantise).
bool parse_hex565(const char* hex, uint16_t& out);

}  // namespace nb
