// SPDX-License-Identifier: GPL-3.0-only
// T-5.4 — the one scoreboard filter. Field set = Marquee norm_game's reads,
// measured down from the 1.46 MB MLB payload (T-0.5: 5,676 B retained raw).

#include "espn_json.h"

namespace nb {
namespace data {

void build_scoreboard_filter(JsonDocument& filter) {
    JsonObject ev = filter["events"][0].to<JsonObject>();
    ev["id"] = true;
    ev["date"] = true;

    JsonObject comp = ev["competitions"][0].to<JsonObject>();
    JsonObject st = comp["status"].to<JsonObject>();
    st["period"] = true;
    st["displayClock"] = true;
    st["type"]["state"] = true;
    st["type"]["shortDetail"] = true;

    JsonObject c = comp["competitors"][0].to<JsonObject>();
    c["homeAway"] = true;
    c["score"] = true;
    JsonObject tm = c["team"].to<JsonObject>();
    tm["id"] = true;
    tm["displayName"] = true;
    tm["name"] = true;  // Marquee fallback when displayName is absent
    tm["abbreviation"] = true;
    tm["color"] = true;

    comp["situation"] = true;  // small — keep the whole subtree
}

}  // namespace data
}  // namespace nb
