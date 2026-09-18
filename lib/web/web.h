// SPDX-License-Identifier: GPL-3.0-only
//
// T-8.1 — device-side web bring-up. Owns ESPAsyncWebServer and later /api/*
// handlers. Lives in lib/web/, NOT lib/render/ (purity rule T-1.6).

#pragma once

#ifdef ARDUINO

namespace nb {
namespace web {

// Idempotent: constructs and starts the server once. false = out of memory.
bool init();

}  // namespace web
}  // namespace nb

#endif  // ARDUINO
