// SPDX-License-Identifier: GPL-3.0-only
//
// Placeholder for lib/render/ (populated in Phase 2, T-2.1 onward).
//
// The purity guard (tools/check_render_purity.sh) runs against this directory.
// Nothing here may include Arduino.h, esp_*.h, freertos/*, WiFi.h, nvs*.h, or
// pull in Arduino's non-POD string class — see AGENTS.md "Hard rules". That is
// what keeps render tests runnable on a laptop and the T-9.3 IDF conversion
// cheap.

#pragma once

#include <cstddef>
#include <cstdint>
