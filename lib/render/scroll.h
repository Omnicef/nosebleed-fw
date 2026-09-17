// SPDX-License-Identifier: GPL-3.0-only
//
// T-7.2 / T-7.3 — scroll engine: window arithmetic, the full-panel window
// paint, static paging, and double-buffered strip publication. Pure; the
// device task loop in main.cpp uses panel::blit(canvas, -w0, 0), which is the
// identical arithmetic proven on hardware at T-2.8 — blit_window is the host
// mirror of it and the only way to test any of this unplugged (PLAN §4: no
// simulator renders HUB75).

#pragma once

#include <atomic>
#include <cstdint>

#include "strip.h"

namespace nb {
namespace render {

// T-7.3 — a period of strip_w + panel_w gives every loop a panel_w-black
// lead-in: the window enters fully black and leaves fully black. The window
// start in strip coords lives in [-panel_w, strip_w); Canvas16::get reads
// off-strip as black, so lead-in and overhang need no special case.
inline int scroll_period(int strip_w, int panel_w) { return strip_w + panel_w; }

inline int scroll_window_x(int strip_w, int panel_w, int32_t scroll_x) {
    const int p = scroll_period(strip_w, panel_w);
    int s = scroll_x % p;
    if (s < 0) s += p;
    return s - panel_w;
}

// T-7.2 — a rebuild must not snap the ticker to 0: keep the position, re-wrap
// modulo the NEW period so a strip that shrank stays in range.
inline int32_t scroll_rewrap(int32_t scroll_x, int strip_w, int panel_w) {
    const int p = scroll_period(strip_w, panel_w);
    int32_t s = scroll_x % p;
    if (s < 0) s += p;
    return s;
}

// T-7.3 — write EVERY dst cell, reading the strip off-bounds as black. The
// splash-traverse smear of T-2.8 came from a clipped push exactly like this;
// a full repaint is the cure and this is its host mirror.
void blit_window(const Canvas16& strip, Canvas16& dst, int w0);

// T-7.4 — static mode: dwell dwell_s seconds per page, then advance.
struct PageState {
    int page = 0;
    int64_t next_change_utc = 0;  // 0 = not started yet
};
int page_window_x(const Strip& s, PageState& st, int64_t now_utc, int dwell_s);

// T-7.2 — strip publication. Writer (core 0, poll-paced): build into back(),
// commit(). Reader (core 1, every frame): front() + generation(). The
// pointer swap is atomic and gen is bumped after it, so a reader that sees a
// new generation sees the new strip and re-wraps its scroll_x. Two commits
// inside one blit would let the reader paint from a strip being overwritten —
// bounded to one glitched frame, and rebuilds are poll-paced (>= seconds).
// (ponytail: no third buffer; add one if a real panel ever shows the glitch.)
class StripHolder {
  public:
    Strip* back() { return &bufs_[alt_]; }
    void commit() {
        cur_.store(&bufs_[alt_], std::memory_order_release);
        gen_.store(gen_.load(std::memory_order_relaxed) + 1,
                   std::memory_order_release);
        alt_ ^= 1;
    }
    const Strip* front() const { return cur_.load(std::memory_order_acquire); }
    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }

  private:
    Strip bufs_[2];
    unsigned alt_ = 0;
    std::atomic<Strip*> cur_{nullptr};
    std::atomic<uint32_t> gen_{0};
};

}  // namespace render
}  // namespace nb
