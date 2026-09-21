// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.2 — weather card. There is no Python layout to copy: Marquee's
// weather widget was a Phase-4 stub (owner-approved fresh design, Phase 10
// session header in WORKLOG). Three lines at panel h=32: condition at y=1
// in spleen-5x8 white (truncated to 12 glyphs = 60 px so nothing clips),
// temperature at y=9 in spleen-6x12 with a hand-drawn degree ring (the
// baked ASCII 32–126 glyph tables have no '°') and the unit letter at
// y=13 in 5x8, high/low at y=24 in 5x8 gray like the clock's date line.
// The r=2 ellipse is the pixel-proven Pillow-parity special case in
// primitives.cpp — the golden reproduces it with the same draw.ellipse.

#pragma once

#include <cstdio>
#include <cstring>

#include "../data/info_cache.h"
#include "card_producer.h"
#include "font.h"
#include "font_data.h"
#include "primitives.h"

namespace nb {
namespace render {

class WeatherWidget : public CardProducer {
  public:
    WeatherWidget(const data::InfoCache* cache, const bool* enabled)
        : cache_(cache), enabled_(enabled) {}

    const char* id() const override { return "weather"; }

    bool is_visible(int64_t) const override {
        data::Weather w{};
        return *enabled_ && cache_->snapshot_weather(&w) && w.fetched_utc != 0;
    }

    uint32_t cards_key(int64_t) const override {
        data::Weather w{};
        if (!cache_->snapshot_weather(&w) || w.fetched_utc == 0) return 0;
        uint32_t h = 2166136261u;
        for (const char* p = w.cond; *p != '\0'; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
        const uint32_t vals[4] = {static_cast<uint32_t>(w.temp), static_cast<uint32_t>(w.high),
                                  static_cast<uint32_t>(w.low), static_cast<uint8_t>(w.unit)};
        for (const uint32_t v : vals)
            for (int b = 0; b < 4; ++b) { h ^= (v >> (8 * b)) & 0xffu; h *= 16777619u; }
        return h;
    }

    int cards(Canvas16* out, int max_cards, int64_t) const override {
        data::Weather w{};
        if (!*enabled_ || max_cards < 1 || !out[0].valid() ||
            !cache_->snapshot_weather(&w) || w.fetched_utc == 0)
            return 0;

        Canvas16& c = out[0];
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) c.set(x, y, 0);

        const uint16_t white = rgb565(255, 255, 255), gray = rgb565(140, 140, 140);

        char cond[13];  // 12 glyphs + NUL: "Freezing drizzle" (16) truncates
        std::strncpy(cond, w.cond, sizeof cond - 1);
        cond[sizeof cond - 1] = '\0';
        const int cond_x = (CARD_W - text_width(FONT_SPLEEN_5X8,
                                                static_cast<int>(std::strlen(cond)))) / 2;
        draw_text(c, FONT_SPLEEN_5X8, cond_x < 0 ? 0 : cond_x, 1, cond, white);

        char tbuf[8];
        if (w.temp == data::kNoInt) {
            std::snprintf(tbuf, sizeof tbuf, "--");
        } else {
            std::snprintf(tbuf, sizeof tbuf, "%d", w.temp);
        }
        const int dlen = static_cast<int>(std::strlen(tbuf));
        const int dw = text_width(FONT_SPLEEN_6X12, dlen);
        const int total = dw + 2 + 5 + 2 + 5;  // digits, gap, ° ring, gap, unit
        const int x0 = (CARD_W - total) / 2;
        draw_text(c, FONT_SPLEEN_6X12, x0, 9, tbuf, white);
        ellipse(c, x0 + dw + 4, 12, 2, 2, white);
        const char ubuf[2] = {w.unit ? w.unit : '?', '\0'};
        draw_text(c, FONT_SPLEEN_5X8, x0 + dw + 9, 13, ubuf, white);

        char hl[24];
        const bool has_hi = w.high != data::kNoInt, has_lo = w.low != data::kNoInt;
        if (has_hi && has_lo) {
            std::snprintf(hl, sizeof hl, "H %d  L %d", w.high, w.low);
        } else if (has_hi) {
            std::snprintf(hl, sizeof hl, "H %d", w.high);
        } else if (has_lo) {
            std::snprintf(hl, sizeof hl, "L %d", w.low);
        } else {
            hl[0] = '\0';
        }
        if (hl[0] != '\0') {
            const int hx = (CARD_W - text_width(FONT_SPLEEN_5X8,
                                                static_cast<int>(std::strlen(hl)))) / 2;
            draw_text(c, FONT_SPLEEN_5X8, hx < 0 ? 0 : hx, 24, hl, gray);
        }
        return 1;
    }

  private:
    const data::InfoCache* cache_;
    const bool* enabled_;
};

}  // namespace render
}  // namespace nb
