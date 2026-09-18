// SPDX-License-Identifier: GPL-3.0-only

#include "strip.h"

#include <cstring>

namespace nb {
namespace render {

int compute_pages(const uint16_t* block_w, int n, int panel_w, int32_t* page_x,
                  int max_pages) {
    int pages = 0;
    int32_t x = 0;
    int used = 0;
    for (int i = 0; i < n && pages < max_pages; ++i) {
        if (used > 0 && used + block_w[i] > panel_w) {
            x += used;  // page full; next card starts a new one
            used = 0;
        }
        if (used == 0) page_x[pages++] = x;
        used += block_w[i];
    }
    return pages;
}

int order_producers(CardProducer* const* ps, int n, int64_t now_utc,
                    bool preemption, int* order) {
    int out = 0;
    if (preemption) {
        for (int i = 0; i < n; ++i) {
            if (ps[i] != nullptr && ps[i]->is_visible(now_utc) &&
                ps[i]->has_live_priority_games())
                order[out++] = i;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (ps[i] == nullptr) continue;
        if (preemption && ps[i]->is_visible(now_utc) &&
            ps[i]->has_live_priority_games())
            continue;  // already placed in the priority pass
        order[out++] = i;
    }
    return out;
}

bool StripBuilder::init_scratch(uint16_t max_cards, uint16_t h) {
    free_scratch();
    cap_ = max_cards > kMaxStripCards ? kMaxStripCards : max_cards;
    for (int i = 0; i < cap_; ++i) {
        scratch_[i] = canvas_alloc(CARD_W, h);
        if (!scratch_[i].valid()) {
            free_scratch();
            return false;
        }
    }
    return cap_ > 0;
}

void StripBuilder::free_scratch() {
    for (int i = 0; i < cap_; ++i) canvas_free(scratch_[i]);
    cap_ = 0;
}

int StripBuilder::build(Strip& dst, CardProducer* const* producers,
                        const int* order, int order_count, int gap,
                        int panel_w, int64_t now_utc) {
    int n = 0;
    int oi = 0;
    CardProducer* last_p = nullptr;
    int last_budget = 0;
    for (; oi < order_count && n < cap_; ++oi) {
        CardProducer* p = producers[order[oi]];
        if (p == nullptr || !p->is_visible(now_utc)) continue;
        last_budget = cap_ - n;
        const int got = p->cards(scratch_ + n, last_budget, now_utc);
        if (got > 0) last_p = p;
        n += got;
    }
    // Truncation signal for the caller (lib/render cannot log). A visible
    // producer never yields zero cards, so any visible producer left in
    // `order` means cards were dropped. When `order` is exhausted exactly
    // at the cap, ask the last producer for one more slot than it got —
    // done AFTER composition below, so clobbering scratch is safe.
    bool truncated = false;
    for (int j = oi; !truncated && j < order_count; ++j) {
        CardProducer* p = producers[order[j]];
        truncated = p != nullptr && p->is_visible(now_utc);
    }
    truncated_ = truncated;
    canvas_free(dst.canvas);
    dst.page_count = 0;
    if (n == 0 || cap_ == 0) return 0;

    const uint16_t block = static_cast<uint16_t>(CARD_W + gap);
    const uint16_t w = static_cast<uint16_t>(n) * block;
    dst.canvas = canvas_alloc(w, scratch_[0].h);
    if (!dst.canvas.valid()) return 0;
    for (int i = 0; i < n; ++i) {
        const int x = i * block;  // gap columns stay black (alloc is zeroed)
        for (int y = 0; y < dst.canvas.h; ++y)
            std::memcpy(dst.canvas.px + static_cast<size_t>(y) * dst.canvas.w + x,
                        scratch_[i].px + static_cast<size_t>(y) * CARD_W,
                        CARD_W * sizeof(uint16_t));
    }
    if (!truncated_ && n == cap_ && last_p != nullptr && last_budget < cap_)
        truncated_ = last_p->cards(scratch_, last_budget + 1, now_utc) > last_budget;
    uint16_t widths[kMaxStripCards];
    for (int i = 0; i < n; ++i) widths[i] = block;
    dst.page_count = compute_pages(widths, n, panel_w, dst.page_x, kMaxStripCards);
    return n;
}

}  // namespace render
}  // namespace nb
