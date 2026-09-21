// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.4 — ticker card producer. One card per item, three centred spleen-
// 5x8 lines (12 glyphs = 60 px, the widths the decoder already caps to):
// label gray y=1, value white y=12, delta colored by leading sign
// (+ green / − red / neutral white) y=22. Same class serves news/stocks/
// crypto — the engine sees nothing special: a CardProducer with the usual
// snapshot-backed key, exactly like the scoreboard, so "no special-
// casing in the engine" is the acceptance proof. No Python layout exists
// (Marquee's ticker.py was a stub) — owner-approved fresh design.

#pragma once

#include <cstring>

#include "../data/info_cache.h"
#include "card_producer.h"
#include "font.h"
#include "font_data.h"

namespace nb {
namespace render {

class TickerWidget : public CardProducer {
  public:
    TickerWidget(const data::InfoCache* cache, int src, const char* id, const bool* enabled)
        : cache_(cache), src_(src), id_(id), enabled_(enabled) {}

    const char* id() const override { return id_; }

    bool is_visible(int64_t) const override {
        data::TickerList tl{};
        return *enabled_ && cache_->snapshot_ticker(src_, &tl) && tl.count > 0;
    }

    uint32_t cards_key(int64_t) const override {
        data::TickerList tl{};
        if (!cache_->snapshot_ticker(src_, &tl) || tl.count <= 0) return 0;
        // D-1 lesson: hash what the cards DRAW, not just what they are —
        // a price tick that only moves l2 must still change the key.
        uint32_t h = 2166136261u;
        for (int i = 0; i < tl.count; ++i) {
            const char* fields[3] = {tl.items[i].label, tl.items[i].l1, tl.items[i].l2};
            for (const char* s : fields) {
                for (; *s != '\0'; ++s) { h ^= static_cast<uint8_t>(*s); h *= 16777619u; }
                h ^= 0x1f;  // field separator so boundaries can't collide
                h *= 16777619u;
            }
        }
        return h;
    }

    int cards(Canvas16* out, int max_cards, int64_t) const override {
        data::TickerList tl{};
        if (!*enabled_ || max_cards < 1 || !cache_->snapshot_ticker(src_, &tl) || tl.count <= 0)
            return 0;
        int n = tl.count;
        if (n > max_cards) n = max_cards;
        for (int i = 0; i < n; ++i) {
            Canvas16& c = out[i];
            if (!c.valid()) return i;
            for (int y = 0; y < c.h; ++y)
                for (int x = 0; x < c.w; ++x) c.set(x, y, 0);
            const auto& it = tl.items[i];
            draw_line(c, it.label, rgb565(140, 140, 140), 1);
            draw_line(c, it.l1, rgb565(255, 255, 255), 12);
            uint16_t col = rgb565(255, 255, 255);
            if (it.l2[0] == '+') col = rgb565(60, 210, 60);
            else if (it.l2[0] == '-' || it.l2[0] == '\xe2') col = rgb565(255, 80, 80);
            draw_line(c, it.l2, col, 22);
        }
        return n;
    }

  private:
    void draw_line(Canvas16& c, const char* s, uint16_t col, int y) const {
        if (s[0] == '\0') return;
        const int w = text_width(FONT_SPLEEN_5X8, static_cast<int>(std::strlen(s)));
        const int x = (CARD_W - w) / 2;
        draw_text(c, FONT_SPLEEN_5X8, x < 0 ? 0 : x, y, s, col);
    }

    const data::InfoCache* cache_;
    int src_;
    const char* id_;
    const bool* enabled_;
};

}  // namespace render
}  // namespace nb
