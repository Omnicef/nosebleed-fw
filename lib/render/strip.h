// SPDX-License-Identifier: GPL-3.0-only
//
// T-7.1 / T-7.4 / T-7.5 — the mega-strip. Producer cards concatenated with
// card_gap blank px into one wide RGB565 canvas (PSRAM on device), static-mode
// page starts, and the preemption order that floats live-favourite producers
// to the front. Pure lib/render: no Arduino, host-testable. The task loop in
// main.cpp drives it; publishing lives in scroll.h.

#pragma once

#include <cstdint>

#include "canvas.h"
#include "card_producer.h"

namespace nb {
namespace render {

// 8 leagues x 16 games + clock = 129 theoretical max; 48 is the memory
// budget (48 x (64+gap) x 32 x 2 B < 250 KB PSRAM per strip buffer) and the
// excess truncates. Revisit if a fat slate ever needs more.
constexpr int kMaxStripCards = 48;

struct Strip {
    Canvas16 canvas;                 // sum(CARD_W+gap) wide, panel-h tall
    int32_t page_x[kMaxStripCards];  // strip x of each static-mode page
    int page_count = 0;
};

// T-7.4 — greedily pack block widths into pages of at most panel_w px.
// Pages align to card boundaries and never split a card; a card wider than
// panel_w gets its own page. Returns the page count.
int compute_pages(const uint16_t* block_w, int n, int panel_w, int32_t* page_x,
                  int max_pages);

// T-7.5 — stable producer visit order. With preemption on, producers holding
// live priority (favourite) games come first; the rest follow, both groups in
// the original (carousel) order. Returns entries written to order[].
int order_producers(CardProducer* const* ps, int n, int64_t now_utc,
                    bool preemption, int* order);

// Scratch-card holder + strip composer. Not thread-safe by design: one
// writer (the core-0 rebuild path), publishing through StripHolder.
class StripBuilder {
  public:
    ~StripBuilder() { free_scratch(); }

    bool init_scratch(uint16_t max_cards, uint16_t h);
    void free_scratch();

    // Build cards from `producers` in `order`, concatenate with `gap` blank
    // px after each card, compute pages for panel_w. dst.canvas is freed and
    // replaced. Returns cards placed; 0 (with dst.canvas invalid) when there
    // is nothing to show or allocation failed — the render task then paints
    // black, never a half strip.
    int build(Strip& dst, CardProducer* const* producers, const int* order,
              int order_count, int gap, int panel_w, int64_t now_utc);

    // True when the last build hit the scratch cap with at least one more
    // visible producer left in `order` — cards were dropped. lib/render is
    // purity-locked from logging; the caller surfaces it.
    bool truncated() const { return truncated_; }

  private:
    Canvas16 scratch_[kMaxStripCards] = {};
    int cap_ = 0;
    bool truncated_ = false;
};

}  // namespace render
}  // namespace nb
