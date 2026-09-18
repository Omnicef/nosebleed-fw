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

// "https://site.api.espn.com/apis/site/v2/sports/{slug}/scoreboard[?dates=YYYYMMDD]"
// T-5.6: pass dates = local_day() output to narrow the window (REQUIRED —
// PLAN §2 mitigation #1); nullptr keeps the unfiltered default window.
// Returns false if cap was too small.
bool scoreboard_url(char* url, size_t cap, const char* slug, const char* dates = nullptr);

// GET + filtered parse straight off the (buffered) stream. doc should come
// from make_psram_doc(). False on transport failure, parse error or
// overflow — caller keeps last-good data either way. *wire (if given)
// receives the bytes read off the socket, whatever the outcome.
bool espn_fetch_scoreboard(const char* url, JsonDocument& doc, size_t* wire = nullptr);

// T-8.4 — team list for the web team picker: GET .../sports/{path}/teams,
// filtered parse (id, displayName/name, abbreviation), serialized to
// [{"id","name","abbreviation"}] in PSRAM. Returns a pointer into a
// single-slot PSRAM cache (24 h TTL per path), *len its length; nullptr on
// failure (transport or parse). Caller copies before returning to any other
// request — the same web thread is the only writer, so the pointer holds
// for the duration of the handler.
const char* espn_teams_json(const char* path, size_t* len);

// Enforced one-TLS-session-at-a-time (AGENTS). Both espn fetches take this
// around their http_get. false = `ms` expired while another fetch held it.
bool tls_take(int ms);
void tls_release();

// JsonDocument backed by SPIRAM. Returned by move (v7 has move semantics;
// copies would deep-clone, moves don't).
JsonDocument make_psram_doc();

}  // namespace data
}  // namespace nb
