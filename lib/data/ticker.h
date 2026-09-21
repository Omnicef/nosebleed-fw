// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.3 — ticker payload. One flat display-ready item per line: the
// decoder truncates/formats so the widget draws strings and does no math.
// Widths are card widths (12 glyphs of spleen-5x8 = 60 px), so nothing can
// clip at draw time. POD, one memcpy through the seqlock InfoCache.

#pragma once

#include <cstdint>

namespace nb {
namespace data {

constexpr int kTickerMaxItems = 10;  // crypto ids and news headlines cap here

struct TickerItem {
    char label[13];  // "BTC" id / "AAPL" / news source name — pre-truncated
    char l1[13];     // price "81,268" / headline line 1
    char l2[13];     // "+1.1%" / headline line 2 ("" if none)
};

struct TickerList {
    int64_t fetched_utc;
    int16_t count;
    TickerItem items[kTickerMaxItems];
};

}  // namespace data
}  // namespace nb
