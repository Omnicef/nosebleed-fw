// SPDX-License-Identifier: GPL-3.0-only
//
// T-8.1 — device-side web bring-up. Owns ESPAsyncWebServer and later /api/*
// handlers. Lives in lib/web/, NOT lib/render/ (purity rule T-1.6).

#pragma once

#ifdef ARDUINO

namespace nb {
namespace render {
class StripHolder;
}
namespace web {

// Idempotent: constructs and starts the server once. false = out of memory.
bool init();

// T-8.6 — /preview and /api/system/preview snapshot this holder's front
// strip as a 24-bit BMP. Called once at startup, before init().
void set_preview_source(const render::StripHolder* holder);

// T-8.6 — POST /api/system/show-ip fires this (main.cpp arms the render
// task's IP splash). No-op if unset.
void set_show_ip_hook(void (*hook)());

}  // namespace web
}  // namespace nb

#endif  // ARDUINO
