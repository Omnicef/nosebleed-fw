// SPDX-License-Identifier: GPL-3.0-only
//
// T-4.2 — NVS config store. One namespace ("nb"), one blob ("cfg")
// holding the whole Config. Replaces the Python's SQLite WAL + JSON
// snapshot recovery: a corrupt or absent blob falls back to seeded
// defaults, which are then persisted (self-healing, no crash loop).
// Device-only (Preferences); the pure parts live in config.h.

#pragma once

#include "config.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace nb {
namespace config {

// true = restored a valid blob. false = absent/corrupt/incompatible:
// `out` holds seeded defaults, persisted back to NVS. Never fails.
bool load(Config& out);

// Persist `c` whole. false = NVS write failed (config still valid in RAM).
// On success notifies the bound render task (T-4.3 — the Python's
// settings_event analogue), if any.
bool save(const Config& c);

// Drop the blob so the next load() seeds defaults (factory-reset hook,
// T-9.6). true = NVS reachable (blob absent afterwards either way).
bool reset();

#ifdef ARDUINO
// The render task registers itself here at startup; every successful
// save() sends it a task notification. One writer (web), one listener.
void bind_render_task(TaskHandle_t render);
#endif

}  // namespace config
}  // namespace nb
