// SPDX-License-Identifier: GPL-3.0-only
// T-5.4 — the one scoreboard filter. Field set = Marquee norm_game's reads,
// measured down from the 1.46 MB MLB payload (T-0.5: 5,676 B retained raw).

#include "espn_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nb {
namespace data {
namespace {

// Howard Hinnant's days_from_civil — no <ctime>/timegm portability roulette,
// deterministic on host and device alike.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Marquee _safe_int: int(value) over ints and numeric strings, else None.
int16_t safe_int(JsonVariantConst v) {
    if (v.is<int32_t>()) return static_cast<int16_t>(v.as<int32_t>());
    if (v.is<int64_t>()) return static_cast<int16_t>(v.as<int64_t>());
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        char* end = nullptr;
        const long long n = strtoll(s, &end, 10);
        if (end != s && *end == '\0') return static_cast<int16_t>(n);
    }
    return kNoInt;
}

// ESPN "2026-06-14T16:15Z" (whole-minute UTC). Fail -> 0 (see header note).
int64_t parse_date(JsonVariantConst v) {
    if (!v.is<const char*>()) return 0;
    int y, mo, d, h, mi;
    if (sscanf(v.as<const char*>(), "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) == 5 &&
        mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
        return days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 +
               h * 3600 + mi * 60;
    }
    return 0;
}

uint32_t parse_hex6(JsonVariantConst v) {
    const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
    if (s && *s == '#') ++s;
    char* end = nullptr;
    const unsigned long c = (s && *s) ? strtoul(s, &end, 16) : 0xFFFFFF;
    return (end && *end == '\0') ? static_cast<uint32_t>(c) : 0xFFFFFF;
}

// Marquee norm_team_from_competitor ("AWY"/"HME" placeholder path included
// via the caller passing a null competitor).
void norm_team(JsonVariantConst competitor, const char* placeholder_abbr, Team& t) {
    // Marquee: placeholder only when the COMPETITOR entry itself is absent
    // (empty dict is falsy) — a present competitor with no team still gets
    // id "0" / abbr "???" from the defaults below.
    JsonVariantConst team = competitor["team"];
    if (competitor.isNull() && placeholder_abbr) {
        copy_str(t.id, sizeof t.id, "0");
        copy_str(t.name, sizeof t.name, placeholder_abbr);
        copy_str(t.abbr, sizeof t.abbr, placeholder_abbr);
        t.colour = 0xFFFFFF;
        return;
    }
    JsonVariantConst id = team["id"];
    if (id.is<const char*>()) {
        copy_str(t.id, sizeof t.id, id.as<const char*>());
    } else if (id.is<int64_t>()) {
        snprintf(t.id, sizeof t.id, "%lld", static_cast<long long>(id.as<int64_t>()));
    } else {
        copy_str(t.id, sizeof t.id, "0");
    }
    const char* disp = team["displayName"].is<const char*>() ? team["displayName"].as<const char*>() : "";
    const char* nm = *disp ? disp
                     : (team["name"].is<const char*>() ? team["name"].as<const char*>() : "");
    copy_str(t.name, sizeof t.name, nm);
    copy_str(t.abbr, sizeof t.abbr,
             team["abbreviation"].is<const char*>() ? team["abbreviation"].as<const char*>() : "???");
    t.colour = parse_hex6(team["color"]);
}

}  // namespace

int to_games(JsonVariantConst root, GameList& out) {
    if (!root["events"].is<JsonArrayConst>()) return 0;
    int n = 0;
    for (JsonObjectConst ev : root["events"].as<JsonArrayConst>()) {
        if (n == kMaxGamesPerLeague) break;
        Game& g = out.games[n];
        memset(&g, 0, sizeof g);
        g.period = g.away_score = g.home_score = kNoInt;

        JsonVariantConst comp = ev["competitions"][0];
        JsonVariantConst st = comp["status"]["type"];
        const char* state = st["state"].is<const char*>() ? st["state"].as<const char*>() : "pre";
        g.status = strcmp(state, "in") == 0   ? kStatusIn
                   : strcmp(state, "post") == 0 ? kStatusPost
                                                : kStatusPre;
        copy_str(g.status_display, sizeof g.status_display,
                 st["shortDetail"].is<const char*>() ? st["shortDetail"].as<const char*>() : "");
        g.period = safe_int(comp["status"]["period"]);
        if (comp["status"]["displayClock"].is<const char*>()) {
            copy_str(g.clock, sizeof g.clock, comp["status"]["displayClock"].as<const char*>());
        }

        JsonArrayConst cs = comp["competitors"].is<JsonArrayConst>()
                                ? comp["competitors"].as<JsonArrayConst>()
                                : JsonArrayConst();
        JsonVariantConst away = cs[0].is<JsonObjectConst>() ? cs[0] : JsonVariantConst();
        JsonVariantConst home = cs.size() > 1 ? cs[1] : JsonVariantConst();
        for (JsonObjectConst c : cs) {
            if (!strcmp(c["homeAway"].as<const char*>(), "away")) away = c;
            else if (!strcmp(c["homeAway"].as<const char*>(), "home")) home = c;
        }
        norm_team(away, "AWY", g.away);
        norm_team(home, "HME", g.home);
        g.away_score = safe_int(away["score"]);
        g.home_score = safe_int(home["score"]);

        JsonVariantConst id = ev["id"];
        if (id.is<const char*>()) copy_str(g.id, sizeof g.id, id.as<const char*>());
        else if (id.is<int64_t>()) snprintf(g.id, sizeof g.id, "%lld", (long long)id.as<int64_t>());

        g.start_utc = parse_date(ev["date"]);

        // Marquee _norm_situation: falsy (missing OR null) -> None
        JsonVariantConst sit = comp["situation"];
        if (sit.is<JsonObjectConst>()) {
            g.has_situation = 1;
            g.situation.on_first = sit["onFirst"].is<bool>() && sit["onFirst"].as<bool>();
            g.situation.on_second = sit["onSecond"].is<bool>() && sit["onSecond"].as<bool>();
            g.situation.on_third = sit["onThird"].is<bool>() && sit["onThird"].as<bool>();
            g.situation.is_red_zone = sit["isRedZone"].is<bool>() && sit["isRedZone"].as<bool>();
            g.situation.balls = safe_int(sit["balls"]);
            g.situation.strikes = safe_int(sit["strikes"]);
            g.situation.outs = safe_int(sit["outs"]);
            g.situation.down = safe_int(sit["down"]);
            g.situation.distance = safe_int(sit["distance"]);
            g.situation.yard_line = safe_int(sit["yardLine"]);
            JsonVariantConst poss = sit["possession"];
            if (poss.is<JsonObjectConst>()) poss = poss["id"];
            if (poss.is<const char*>()) copy_str(g.situation.possession, sizeof g.situation.possession, poss.as<const char*>());
            else if (poss.is<int64_t>()) snprintf(g.situation.possession, sizeof g.situation.possession, "%lld", (long long)poss.as<int64_t>());
        }
        ++n;
    }
    return n;
}

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
