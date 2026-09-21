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

// Clear the whole "nb" namespace so the next boot seeds defaults — the
// factory-reset hook (T-9.6). Covers the config blob, the credentials blob
// (a factory reset returns to first-boot provisioning, T-9.2) and any key
// added later. true = NVS reachable.
bool reset();

// T-9.2 — WiFi credentials: separate blob ("net"), deliberately NOT
// part of Config so no config GET/serialisation path can echo a
// password. false = absent, corrupt or schema-invalid; callers must
// then fall back (T-9.2: compile-time secrets) or provision (T-9.1).
// Nothing here logs the values; do not add logging at call sites.
bool load_creds(Creds& out);
bool save_creds(const Creds& c);
bool clear_creds();

// T-10.3 — ticker API keys: separate blob ("keys"), same discipline as
// creds — never in Config, never GET, never logged. false = absent or
// invalid; an all-empty (unset) ApiKeys is valid and means "no sources".
bool load_keys(ApiKeys& out);
bool save_keys(const ApiKeys& k);

#ifdef ARDUINO
// The render task registers itself here at startup; every successful
// save() sends it a task notification. One writer (web), one listener.
void bind_render_task(TaskHandle_t render);
#endif

}  // namespace config
}  // namespace nb
