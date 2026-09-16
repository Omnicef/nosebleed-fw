// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.4 — device-side ESPN fetch: transport (http.h) + the framework-free
// decoder (espn_json.h). The parse document lives in PSRAM via the official
// SpiRamAllocator, so a fetch costs ~0 internal heap by construction.
// One session at a time = caller discipline (T-5.7's sequential poll task).

#pragma once

#include <cstddef>

#include <ArduinoJson.h>

namespace nb {
namespace data {

// "https://site.api.espn.com/apis/site/v2/sports/{slug}/scoreboard"
// T-5.6 appends the ?dates= window. Returns false if cap was too small.
bool scoreboard_url(char* url, size_t cap, const char* slug);

// GET + filtered parse straight off the (buffered) stream. doc should come
// from make_psram_doc(). False on transport failure, parse error or
// overflow — caller keeps last-good data either way.
bool espn_fetch_scoreboard(const char* url, JsonDocument& doc);

// JsonDocument backed by SPIRAM. Returned by move (v7 has move semantics;
// copies would deep-clone, moves don't).
JsonDocument make_psram_doc();

}  // namespace data
}  // namespace nb
