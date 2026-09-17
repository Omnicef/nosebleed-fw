// SPDX-License-Identifier: GPL-3.0-only
//
// T-6.1 — CardProducer interface. The Python's protocol returns a list of
// per-card PIL images; this keeps the same contract without dynamic lists.
// The caller owns a bounded array of CARD_W-wide canvases and the producer
// fills as many as it can into the remaining slots.

#pragma once

#include <cstdint>

#include "canvas.h"

namespace nb {
namespace render {

constexpr int CARD_W = 64;

class CardProducer {
  public:
    virtual ~CardProducer() = default;

    virtual const char* id() const = 0;
    virtual uint32_t cards_key(int64_t now_utc) const = 0;

    // Fills out[0..max_cards) with panel-height cards from producer state.
    // Returns the number of cards written; out canvases must be CARD_W wide.
    virtual int cards(Canvas16* out, int max_cards, int64_t now_utc) const = 0;

    virtual bool is_visible(int64_t now_utc) const {
        (void)now_utc;
        return true;
    }

    virtual bool has_live_priority_games() const { return false; }
};

// Concatenate visible producers into one card array in producer order.
inline int compose_cards(CardProducer* const* producers, int producer_count,
                         Canvas16* out, int max_cards, int64_t now_utc) {
    int written = 0;
    for (int i = 0; i < producer_count && written < max_cards; ++i) {
        CardProducer* p = producers[i];
        if (!p || !p->is_visible(now_utc)) continue;
        written += p->cards(out + written, max_cards - written, now_utc);
    }
    return written;
}

}  // namespace render
}  // namespace nb
