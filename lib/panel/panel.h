// SPDX-License-Identifier: GPL-3.0-only
//
// T-2.8 — device-side panel wrapper. Owns the MatrixPanel_I2S_DMA instance
// and pushes lib/render Canvas16 windows into the DMA framebuffer. Lives in
// lib/panel/, NOT lib/render/ — this is the seam where Arduino enters
// (purity rule T-1.6 keeps lib/render host-compilable).

#pragma once

#ifdef ARDUINO

#include <cstdint>

// Relative: PlatformIO's LDF matches lib deps by dir name, and "canvas.h"
// does not name lib/render/.
#include "../render/canvas.h"
#include "../config/config.h"

namespace nb {
namespace panel {

// Build HUB75_I2S_CFG from the HardwareSetting (never hardcode geometry —
// AGENTS.md) and start the DMA output. false = begin() failed.
bool init(const config::HardwareSetting& hw);

// 0-100 %, from config.hw.brightness.
void set_brightness(uint8_t pct);

int width();
int height();
int refresh_rate();  // Hz, driver-derived (calculated_refresh_rate)

// Repaint the whole panel from `c` positioned at (dx, dy), reading
// off-canvas as black — every cell written every frame (persistent DMA
// framebuffer; PLAN T-7.3) — then flipDMABuffer() (no-op unless double_buff).
void blit(const Canvas16& c, int dx = 0, int dy = 0);

// T-2.9 — hold-scroll-hold-loop (the Python's boot-splash behaviour): if
// wider than the panel, hold, traverse 1 px/frame, hold, repeat forever;
// if it fits, just hold. Does not return — boot-splash/bring-up path only.
void splash(const Canvas16& c, uint32_t hold_ms);

}  // namespace panel
}  // namespace nb

#endif  // ARDUINO
