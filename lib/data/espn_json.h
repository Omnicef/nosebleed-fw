// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.4/T-5.5 — ESPN scoreboard decoding, framework-free (ArduinoJson +
// game.h only), so the exact code the device runs is the code the host
// fixtures tests. The transport lives in espn.h (device-only).

#pragma once

#include <cstdint>
#include <utility>

#include <ArduinoJson.h>

#include "game.h"

namespace nb {
namespace data {

// Keep exactly what to_games() reads (Marquee norm_game's field set) —
// nothing else is allocated. The `[0]` element is the filter TEMPLATE for
// every array element (verified against JsonDeserializer.hpp: elements
// resolve their filter via filter[0UL]).
void build_scoreboard_filter(JsonDocument& filter);

// Filtered parse with the mandatory NestingLimit(20) — ESPN's MLB payload
// nests to depth 15 and the default 10 dies with TooDeep (measured, T-0.5).
// TStream: std::istream (host fixtures) or Stream& (device, wrapped in
// ReadBufferingStream by the transport).
template <typename TStream>
DeserializationError parse_scoreboard(TStream&& in, JsonDocument& filter,
                                      JsonDocument& doc) {
    return deserializeJson(doc, std::forward<TStream>(in),
                           DeserializationOption::Filter(filter),
                           DeserializationOption::NestingLimit(20));
}

// Marquee norm_game / norm_team_from_competitor / _norm_situation /
// _safe_int / _parse_date, ported. Reads a filtered scoreboard root,
// fills out->games (clamped to kMaxGamesPerLeague), returns the count.
// out->count/fetched_utc are NOT touched (the cache owns publication).
// start_utc: ESPN dates are whole-minute ISO-8601 UTC; unparseable -> 0
// (Python: now() — fixtures never hit it; device retries next poll anyway).
int to_games(JsonVariantConst root, GameList& out);

}  // namespace data
}  // namespace nb
