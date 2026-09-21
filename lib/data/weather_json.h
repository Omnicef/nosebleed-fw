// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.1 — Open-Meteo decoding, framework-free (ArduinoJson + weather.h
// only), mirroring the espn_json split: the exact code the device runs is
// the code the host fixtures test. Transport (weather_fetch) is the
// ARDUINO-only tail — http.h + tls_take live there.
//
// Payload shape verified live 2026-09-20 against
// /v1/forecast?current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min
// — nesting depth 3 (root → current/daily → scalar array elements), well
// under the project-wide NestingLimit(20).

#pragma once

#include <cstddef>
#include <utility>

#include <ArduinoJson.h>

#include "weather.h"

namespace nb {
namespace data {

// Keep current.{temperature_2m,weather_code}, the temperature unit string
// and today's daily max/min — everything else (latitude echo, timezone,
// elevation, units table noise) is skipped without being allocated.
void build_weather_filter(JsonDocument& filter);

// Same NestingLimit(20) discipline as ESPN — cheap insurance against an
// upstream shape change, and the only pattern this project parses with.
template <typename TStream>
DeserializationError parse_weather(TStream&& in, JsonDocument& filter,
                                   JsonDocument& doc) {
    return deserializeJson(doc, std::forward<TStream>(in),
                           DeserializationOption::Filter(filter),
                           DeserializationOption::NestingLimit(20));
}

// Filtered root → Weather. false = unusable payload (no current
// temperature) — caller keeps last-good. temp/high/low round to whole
// degrees; cond resolves the WMO code ("" for unknown); unit is the LAST
// char of current_units.temperature_2m ("°C"/"°F" are UTF-8 — two bytes
// before the letter).
bool to_weather(JsonVariantConst root, Weather& out);

// URL builder + injection guard (pure so the host pins it): lat/lon must
// be plain decimals ([0-9+.-], 1..15 chars, at least one digit) or false —
// they ride into a query string, and everything in a URL that is not
// validated is an injection surface. false also when cap was too small.
bool weather_url(char* url, size_t cap, const char* lat, const char* lon,
                 bool imperial);

#ifdef ARDUINO
// GET + filtered buffered parse (tls_take → http_get → ReadBufferingStream),
// then to_weather. False on transport/parse/fallback failure — the caller
// keeps the last published Weather either way.
bool weather_fetch(const char* url, Weather& out);
#endif

}  // namespace data
}  // namespace nb
