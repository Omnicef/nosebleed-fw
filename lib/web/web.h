// SPDX-License-Identifier: GPL-3.0-only
//
// T-8.1 — device-side web bring-up. Owns ESPAsyncWebServer and later /api/*
// handlers. Lives in lib/web/, NOT lib/render/ (purity rule T-1.6).

#pragma once

#ifdef ARDUINO

#include <atomic>
#include <cstdint>

namespace nb {
namespace render {
class StripHolder;
}
namespace web {

// Idempotent: constructs and starts the server once. false = out of memory.
bool init();

// T-8.6 — /preview and /api/system/preview snapshot a panel-sized window of
// this holder's front strip as a 24-bit BMP; win_x is the render task's
// published window origin, so the image tracks what the panel shows rather
// than recomputing it at request time (they differ mid-scroll). The full
// composed strip moves to /api/system/preview/strip for debugging.
// Called once at startup, before init().
void set_preview_source(const render::StripHolder* holder,
                        const std::atomic<int32_t>* win_x);

// T-8.6 — POST /api/system/show-ip fires this (main.cpp arms the render
// task's IP splash). No-op if unset.
void set_show_ip_hook(void (*hook)());

// T-9.6/T-9.4 — POST /api/system/factory-reset (confirmed) and a finished
// OTA upload fire this after the response is queued; main.cpp arms a
// deferred esp_restart so the reply flushes first — handlers run on the
// async_tcp task and must not block it. The string lives until the restart.
void set_reboot_hook(void (*hook)(const char* why));

}  // namespace web
}  // namespace nb

#endif  // ARDUINO
