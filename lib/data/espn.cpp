// SPDX-License-Identifier: GPL-3.0-only
// T-5.4 — device transport for espn_json: one buffered filtered fetch.
// Compiled only under Arduino (native builds see the decoder alone; the
// chain LDF compiles every .cpp in a used lib dir).

#include "espn.h"

#if defined(ARDUINO)

#include <cstdio>
#include <cstring>
#include <ctime>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <StreamUtils.h>

#include "esp_heap_caps.h"

#include "espn_json.h"
#include "http.h"

namespace nb {
namespace data {
namespace {

struct SpiRamAllocator : ArduinoJson::Allocator {
    void* allocate(size_t s) override { return heap_caps_malloc(s, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { return heap_caps_free(p); }
    void* reallocate(void* p, size_t s) override { return heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM); }
};

SpiRamAllocator g_psram_alloc;

// One TLS session at a time (AGENTS hard rule) — poll and the T-8.4 team
// picker contend here. Created once, never deleted.
SemaphoreHandle_t g_tls_mux = nullptr;
StaticSemaphore_t g_tls_mem;
portMUX_TYPE g_tls_init_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

bool tls_take(int ms) {
    taskENTER_CRITICAL(&g_tls_init_mux);
    if (g_tls_mux == nullptr) g_tls_mux = xSemaphoreCreateMutexStatic(&g_tls_mem);
    taskEXIT_CRITICAL(&g_tls_init_mux);
    return xSemaphoreTake(g_tls_mux, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void tls_release() { xSemaphoreGive(g_tls_mux); }

JsonDocument make_psram_doc() { return JsonDocument(&g_psram_alloc); }

bool scoreboard_url(char* url, size_t cap, const char* slug, const char* dates) {
    const int n = dates
        ? snprintf(url, cap,
                   "https://site.api.espn.com/apis/site/v2/sports/%s/scoreboard?dates=%s",
                   slug, dates)
        : snprintf(url, cap,
                   "https://site.api.espn.com/apis/site/v2/sports/%s/scoreboard", slug);
    return n > 0 && (size_t)n < cap;
}

bool espn_fetch_scoreboard(const char* url, JsonDocument& doc, size_t* wire) {
    if (!tls_take(15000)) return false;
    JsonDocument filter(&g_psram_alloc);
    build_scoreboard_filter(filter);
    DeserializationError err;  // never read unless the lambda ran (http_get ok==false otherwise)
    const bool ok = http_get(url, [&](HttpStream& s) {
        ReadBufferingStream rs(s, 512);
        err = parse_scoreboard(rs, filter, doc);
        if (wire) *wire = s.bytes();  // self-counting stream (ESPN replies chunked)
        return err == DeserializationError::Ok && !doc.overflowed();
    });
    tls_release();
    return ok;
}

namespace {

// Single-slot PSRAM team-list cache, one path at a time (the picker browses
// one league at a time; worst league ~1,000 rows ~= 60 KB of 8 MB).
char* g_teams = nullptr;
size_t g_teams_len = 0;
char g_teams_path[48] = {};
time_t g_teams_ts = 0;

}  // namespace

const char* espn_teams_json(const char* path, size_t* len) {
    const time_t now = time(nullptr);
    if (g_teams && g_teams_ts != 0 && strncmp(g_teams_path, path, sizeof g_teams_path) == 0 &&
        now - g_teams_ts < 86400) {
        if (len) *len = g_teams_len;
        return g_teams;
    }

    char url[160];
    if (snprintf(url, sizeof url,
                 "https://site.api.espn.com/apis/site/v2/sports/%s/teams", path) >=
        (int)sizeof url)
        return nullptr;

    JsonDocument filter(&g_psram_alloc);
    JsonObject tm = filter["sports"][0]["leagues"][0]["teams"][0]["team"].to<JsonObject>();
    tm["id"] = true;
    tm["displayName"] = true;
    tm["name"] = true;  // norm_team's fallback when displayName is absent
    tm["abbreviation"] = true;

    JsonDocument doc = make_psram_doc();
    bool fetched = false;
    if (tls_take(10000)) {
        DeserializationError err;
        fetched = http_get(url, [&](HttpStream& s) {
            ReadBufferingStream rs(s, 512);
            err = parse_scoreboard(rs, filter, doc);  // same Filter + NestingLimit(20)
            return err == DeserializationError::Ok && !doc.overflowed();
        });
        tls_release();
    }
    if (!fetched) return g_teams;  // stale cache beats nothing

    // Serialize [{"id","name","abbreviation"}] into a growing PSRAM buffer.
    // Per-entry serializeJson escapes every field, so names with quotes
    // cannot corrupt the array; an entry that overflows the scratch is
    // dropped rather than truncated mid-escape.
    JsonArrayConst arr = doc["sports"][0]["leagues"][0]["teams"].as<JsonArrayConst>();
    size_t cap = 8192, n_out = 0;
    char* buf = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) return g_teams;
    buf[n_out++] = '[';
    bool first = true;
    char entry[192];
    bool failed = false;
    for (JsonObjectConst t : arr) {
        JsonObjectConst team = t["team"];
        JsonDocument e;
        char id[16] = "";
        if (team["id"].is<const char*>()) {
            strlcpy(id, team["id"].as<const char*>(), sizeof id);
        } else if (team["id"].is<int64_t>()) {
            snprintf(id, sizeof id, "%lld", static_cast<long long>(team["id"].as<int64_t>()));
        }
        const char* name = team["displayName"].is<const char*>()
                               ? team["displayName"].as<const char*>()
                               : (team["name"].is<const char*>() ? team["name"].as<const char*>()
                                                                  : "");
        const char* abbr = team["abbreviation"].is<const char*>()
                               ? team["abbreviation"].as<const char*>()
                               : "";
        e["id"] = id;
        e["name"] = name;
        e["abbreviation"] = abbr;
        const size_t n = serializeJson(e, entry, sizeof entry);
        if (n == 0 || n >= sizeof entry) continue;
        const size_t need = n_out + (first ? 0 : 1) + n + 1;  // +1: closing ]
        if (need > cap) {
            cap *= 2;
            char* bigger = static_cast<char*>(heap_caps_realloc(buf, cap, MALLOC_CAP_SPIRAM));
            if (bigger == nullptr) {
                failed = true;
                break;
            }
            buf = bigger;
        }
        if (!first) buf[n_out++] = ',';
        memcpy(buf + n_out, entry, n);
        n_out += n;
        first = false;
    }
    if (failed) {
        heap_caps_free(buf);
        return g_teams;
    }
    buf[n_out++] = ']';

    // Single-slot swap: shrink best-effort, keep the working buffer if the
    // (PSRAM) realloc ever fails — oversized beats lost.
    char* shrunk = static_cast<char*>(heap_caps_realloc(buf, n_out, MALLOC_CAP_SPIRAM));
    g_teams_len = n_out;
    g_teams = shrunk != nullptr ? shrunk : buf;
    g_teams_path[sizeof g_teams_path - 1] = '\0';
    strlcpy(g_teams_path, path, sizeof g_teams_path);
    g_teams_ts = now;
    if (len) *len = g_teams_len;
    return g_teams;
}

}  // namespace data
}  // namespace nb

#endif  // ARDUINO
