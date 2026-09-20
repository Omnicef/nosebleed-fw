// SPDX-License-Identifier: GPL-3.0-only

#include "web.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Update.h>  // T-9.4 — esp_ota wrapper (arduino-esp32 component library)
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <new>

#include <cstring>
#include <memory>

#include "../config/config.h"
#include "../config/store.h"
#include "../data/espn.h"
#include "../logos/logos_ota.h"
#include "../net/net.h"
#include "../render/scroll.h"
#include "bmp.h"

#ifndef NB_FW_VERSION
#define NB_FW_VERSION "dev"
#endif

namespace nb {
namespace web {
namespace {

AsyncWebServer* g_server = nullptr;
bool g_fs_mounted = false;
void (*g_reboot_hook)(const char*) = nullptr;  // T-9.6/9.4 deferred restart

// T-4.4 restart banner, sticky once set on a save whose structural fields
// changed vs the stored value. RAM-only by design: a reboot applies the new
// config and clears it.
bool g_restart_required = false;

uint32_t internal_free() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

uint32_t internal_largest() {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void send_json(AsyncWebServerRequest* request, int code, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    request->send(code, "application/json", out);
}

void settings_json(const config::Config& c, JsonDocument& d) {
    const auto& hw = c.hw;
    d["rows"] = hw.rows;
    d["cols"] = hw.cols;
    d["chain_length"] = hw.chain_length;
    d["parallel"] = hw.parallel;
    d["brightness"] = hw.brightness;
    d["boot_splash_duration"] = hw.boot_splash_s;
    d["timezone"] = hw.timezone;
    d["display_mode"] = hw.display_mode == config::kDisplayStatic ? "static" : "scroll";
    d["scroll_speed"] = hw.scroll_speed;
    d["card_gap"] = hw.card_gap;
    d["clock_24h"] = hw.clock_24h != 0;
}

long lclamp(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ─── T-9.4 — firmware OTA: PUT /api/update, raw binary body ───────────────
//
// The Arduino Update library is the esp_ota wrapper shipped inside the
// arduino-esp32 component (libraries/Update/src/Update.h; verified against
// that header). begin() picks the inactive app slot, end() verifies the
// image and only THEN calls esp_ota_set_boot_partition — a corrupt upload
// can never point the bootloader at garbage. Rollback is NOT delegated to
// the PENDING_VERIFY state machine here: on this stack the staged entry
// reads VALID at boot#1 (measured 2026-09-20), so an image that dies before
// finishing setup would crash-loop forever. The guard in main.cpp
// (NbBootGuard) self-marks the image INVALID after three incomplete boots;
// the bootloader excludes INVALID entries and the other slot wins — verified
// end-to-end on hardware. USB flashing stays fully supported alongside OTA —
// GPLv3 §6 means owners can always install their own builds by other means.
class OtaHandler : public AsyncWebHandler {
  public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        // Namespaced constant ON PURPOSE: bare HTTP_PUT resolves to the global
        // `enum http_method` in http_parser.h (pulled in by ESPAsyncWebServer's
        // own includes; value 4, not the 1<<4 the request reports) — it
        // compiles and never matches (hardware-proven 404).
        return request->method() == AsyncWebRequestMethod::HTTP_PUT &&
               request->url() == "/api/update";
    }
    bool isRequestHandlerTrivial() const override { return false; }

    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
                    size_t total) override {
        if (index == 0) {
            aborted_ = false;
            got_ = 0;
            Update.abort();  // release any handle left by an aborted upload (stuck until reboot otherwise)
            if (total == 0) {  // no Content-Length — Update.begin wants a size (curl -T sends one)
                request->send(411, "application/json",
                              "{\"error\":\"Content-Length required (use: curl -T firmware.bin .../api/update)\"}");
                aborted_ = true;
                return;
            }
            if (!Update.begin(total, U_FLASH)) {
                Update.printError(Serial);
                request->send(400, "application/json",
                              "{\"error\":\"refused: no inactive app slot, or image larger than the slot\"}");
                aborted_ = true;
                return;
            }
            Serial.printf("[ota] update started: %u B\n", static_cast<unsigned>(total));
        }
        if (aborted_) return;
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
            Update.abort();  // idempotent; releases the staging handle
            request->send(500, "application/json", "{\"error\":\"flash write failed mid-upload\"}");
            aborted_ = true;
            return;
        }
        got_ += len;
        if (got_ / 0x40000 != (got_ - len) / 0x40000)
            Serial.printf("[ota] %u / %u B\n", static_cast<unsigned>(got_), static_cast<unsigned>(total));
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        if (aborted_) return;  // a response already went out mid-stream
        if (!Update.end(true)) {  // validates the image; sets the boot slot only on success
            Update.printError(Serial);
            request->send(422, "application/json",
                          "{\"error\":\"image invalid — boot target unchanged\"}");
            return;
        }
        Serial.println("[ota] image valid, rebooting into it (rolls back if it fails to start)");
        request->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
        if (g_reboot_hook != nullptr) g_reboot_hook("firmware update applied");
    }

  private:
    bool aborted_ = true;
    size_t got_ = 0;
};

OtaHandler g_ota;  // static — one instance, registered below

// PUT /api/settings body — the SPA's full form. Absent keys keep their
// stored values; out-of-range numbers are clamped, wrong types ignored.
void handle_settings_put(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>()) {
        request->send(400, "application/json", "{\"error\":\"expected a JSON object\"}");
        return;
    }
    config::Config c;
    config::load(c);
    const config::HardwareSetting old_hw = c.hw;  // only .hw is compared; a full Config copy
                                                  // blew the async_tcp stack canary (2026-09-19)
    if (json["brightness"].is<long>()) c.hw.brightness = static_cast<uint8_t>(lclamp(json["brightness"], 0, 100));
    if (json["rows"].is<long>()) c.hw.rows = static_cast<uint16_t>(lclamp(json["rows"], 1, 512));
    if (json["cols"].is<long>()) c.hw.cols = static_cast<uint16_t>(lclamp(json["cols"], 1, 512));
    if (json["chain_length"].is<long>())
        c.hw.chain_length = static_cast<uint16_t>(lclamp(json["chain_length"], 1, 8));
    if (json["parallel"].is<long>())
        c.hw.parallel = static_cast<uint8_t>(lclamp(json["parallel"], 1, 3));
    if (json["boot_splash_duration"].is<long>())
        c.hw.boot_splash_s = static_cast<uint16_t>(lclamp(json["boot_splash_duration"], 0, 600));
    if (json["timezone"].is<const char*>())
        strlcpy(c.hw.timezone, json["timezone"], config::kTzLen);
    if (json["display_mode"].is<const char*>())
        c.hw.display_mode = std::strcmp(json["display_mode"], "static") == 0
                                ? config::kDisplayStatic
                                : config::kDisplayScroll;
    if (json["scroll_speed"].is<float>() || json["scroll_speed"].is<long>())
        c.hw.scroll_speed =
            static_cast<float>(lclamp(static_cast<long>(json["scroll_speed"].as<float>()), 10, 200));
    if (json["card_gap"].is<long>())
        c.hw.card_gap = static_cast<uint8_t>(lclamp(json["card_gap"], 0, 32));
    if (json["clock_24h"].is<bool>()) c.hw.clock_24h = json["clock_24h"].as<bool>() ? 1 : 0;

    const bool structural = config::hw_structural_changed(old_hw, c.hw);
    if (!config::save(c)) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
    }
    if (structural) g_restart_required = true;
    Serial.printf("[web] settings saved, restart_required=%d\n", structural);
    JsonDocument d;
    d["restart_required"] = structural;
    send_json(request, 200, d);
}

void handle_system_get(AsyncWebServerRequest* request) {
    JsonDocument d;
    d["ip"] = WiFi.localIP().toString();
    d["uptime_s"] = static_cast<uint32_t>(millis() / 1000);
    d["free_heap"] = internal_free();
    d["psram_free"] =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    d["version"] = NB_FW_VERSION;
    d["restart_required"] = g_restart_required;
    send_json(request, 200, d);
}

// ─── T-8.4 — /api/sports/*, /api/favorites ────────────────────────────────

int league_idx(const String& slug) {
    for (int i = 0; i < config::kLeagueSlugCount; ++i)
        if (slug == config::kLeagueSlugs[i]) return i;
    return -1;
}

void favorite_json(JsonObject d, const config::Favorite& f, int id) {
    d["id"] = id;
    d["league"] = f.league;
    d["team_id"] = f.team_id;
    d["team_name"] = f.team_name;
    d["team_abbr"] = f.team_abbr;
}

void handle_leagues_get(AsyncWebServerRequest* request) {
    config::Config c;
    config::load(c);
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < c.league_count; ++i) {
        JsonObject o = a.add<JsonObject>();
        o["id"] = c.leagues[i].id;
        o["enabled"] = c.leagues[i].enabled != 0;
    }
    send_json(request, 200, d);
}

// PUT /api/sports/leagues/{id} body {"enabled": bool}. SPA defers the
// widget consequences to restart (its own copy says so); the row persists.
void handle_league_put(AsyncWebServerRequest* request, JsonVariant& json) {
    const String id = request->url().substring(strlen("/api/sports/leagues/"));
    int j = -1;
    config::Config c;
    config::load(c);
    for (int i = 0; i < c.league_count; ++i)
        if (id == c.leagues[i].id) j = i;
    if (j < 0) {
        request->send(404, "application/json", "{\"error\":\"unknown league\"}");
        return;
    }
    if (!json.is<JsonObject>() || !json["enabled"].is<bool>()) {
        request->send(400, "application/json", "{\"error\":\"enabled required\"}");
        return;
    }
    c.leagues[j].enabled = json["enabled"].as<bool>() ? 1 : 0;
    if (!config::save(c)) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
    }
    Serial.printf("[web] league %s enabled=%d\n", id.c_str(), c.leagues[j].enabled);
    JsonDocument d;
    d["id"] = id;
    d["enabled"] = c.leagues[j].enabled != 0;
    send_json(request, 200, d);
}

// GET /api/sports/{league}/teams — ESPN proxy with PSRAM cache (espn.cpp).
void handle_teams_get(AsyncWebServerRequest* request) {
    static const char* kPrefix = "/api/sports/";
    static const char* kSuffix = "/teams";
    const String url = request->url();
    if (!url.startsWith(kPrefix) || !url.endsWith(kSuffix) ||
        url.length() <= strlen(kPrefix) + strlen(kSuffix)) {
        request->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    const String slug = url.substring(strlen(kPrefix), url.length() - strlen(kSuffix));
    const int lg = league_idx(slug);
    if (lg < 0) {
        request->send(404, "application/json", "{\"error\":\"unknown league\"}");
        return;
    }
    size_t len = 0;
    const char* json = nb::data::espn_teams_json(config::kLeagueApiPaths[lg], &len);
    if (json == nullptr || len == 0) {
        request->send(502, "application/json", "{\"error\":\"teams fetch failed\"}");
        return;
    }
    // Copy out of the PSRAM cache: the response object must own its bytes
    // (an async flush racing a later cache refresh must never see them move).
    String out(json, len);
    request->send(200, "application/json", out);
}

void handle_favorites_get(AsyncWebServerRequest* request) {
    config::Config c;
    config::load(c);
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < c.favorite_count; ++i) favorite_json(a.add<JsonObject>(), c.favorites[i], i);
    send_json(request, 200, d);
}

// POST /api/favorites {league, team_id, team_name, team_abbr}
void handle_favorite_post(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>() || !json["league"].is<const char*>() ||
        (!json["team_id"].is<const char*>() && !json["team_id"].is<int64_t>())) {
        request->send(400, "application/json", "{\"error\":\"league and team_id required\"}");
        return;
    }
    const String league = json["league"].as<const char*>();
    if (league_idx(league) < 0) {
        request->send(400, "application/json", "{\"error\":\"unknown league\"}");
        return;
    }
    char team_id[16];
    if (json["team_id"].is<const char*>()) {
        strlcpy(team_id, json["team_id"], sizeof team_id);
    } else {
        snprintf(team_id, sizeof team_id, "%lld",
                 static_cast<long long>(json["team_id"].as<int64_t>()));
    }
    config::Config c;
    config::load(c);
    for (int i = 0; i < c.favorite_count; ++i)
        if (league == c.favorites[i].league && strcmp(team_id, c.favorites[i].team_id) == 0) {
            request->send(409, "application/json", "{\"error\":\"already a favorite\"}");
            return;
        }
    if (c.favorite_count >= config::kMaxFavorites) {
        request->send(409, "application/json", "{\"error\":\"favorite list full\"}");
        return;
    }
    config::Favorite& f = c.favorites[c.favorite_count];
    strlcpy(f.league, league.c_str(), sizeof f.league);
    strlcpy(f.team_id, team_id, sizeof f.team_id);
    strlcpy(f.team_name,
            json["team_name"].is<const char*>() ? json["team_name"].as<const char*>() : "",
            sizeof f.team_name);
    strlcpy(f.team_abbr,
            json["team_abbr"].is<const char*>() ? json["team_abbr"].as<const char*>() : "",
            sizeof f.team_abbr);
    f.priority = 1;
    const int id = c.favorite_count++;
    if (!config::save(c)) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
    }
    JsonDocument d;
    favorite_json(d.to<JsonObject>(), f, id);
    send_json(request, 200, d);
}

// DELETE /api/favorites/{league}/{team_id}
void handle_favorite_delete(AsyncWebServerRequest* request) {
    static const char* kPrefix = "/api/favorites/";
    const String url = request->url();
    const int slash = url.indexOf('/', strlen(kPrefix));
    if (slash < 0) {
        request->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    const String league = url.substring(strlen(kPrefix), slash);
    const String team_id = url.substring(slash + 1);
    config::Config c;
    config::load(c);
    int j = -1;
    for (int i = 0; i < c.favorite_count; ++i)
        if (league == c.favorites[i].league && team_id == c.favorites[i].team_id) j = i;
    if (j < 0) {
        request->send(404, "application/json", "{\"error\":\"not a favorite\"}");
        return;
    }
    for (int i = j; i + 1 < c.favorite_count; ++i) c.favorites[i] = c.favorites[i + 1];
    c.favorite_count--;
    if (!config::save(c)) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
    }
    JsonDocument d;
    d["removed"] = true;
    send_json(request, 200, d);
}

// ─── T-8.5 — /api/widgets (carousel order, enable, dwell) ────────────────

int widget_find(const config::Config& c, const String& id) {
    for (int i = 0; i < c.widget_count; ++i)
        if (id == c.widgets[i].id) return i;
    return -1;
}

void widget_json(JsonObject o, const config::WidgetConfig& w) {
    o["id"] = w.id;
    o["type"] = w.type;
    o["order"] = w.order;
    o["enabled"] = w.enabled != 0;
    o["dwell_seconds"] = w.dwell_s;
    if (w.league[0] != '\0') o["league"] = w.league;
}

// Widgets in carousel order (by `order`, insertion sort over <=16 rows).
int widget_order_list(const config::Config& c, uint8_t* idx) {
    int n = 0;
    for (int i = 0; i < c.widget_count && n < config::kMaxWidgets; ++i)
        idx[n++] = static_cast<uint8_t>(i);
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && c.widgets[idx[b]].order < c.widgets[idx[b - 1]].order; --b) {
            const uint8_t t = idx[b];
            idx[b] = idx[b - 1];
            idx[b - 1] = t;
        }
    return n;
}

void widgets_array(JsonDocument& d, const config::Config& c) {
    uint8_t idx[config::kMaxWidgets];
    const int n = widget_order_list(c, idx);
    JsonArray a = d.to<JsonArray>();
    for (int j = 0; j < n; ++j) widget_json(a.add<JsonObject>(), c.widgets[idx[j]]);
}

void handle_widgets_get(AsyncWebServerRequest* request) {
    config::Config c;
    config::load(c);
    JsonDocument d;
    widgets_array(d, c);
    send_json(request, 200, d);
}

// PUT /api/widgets/{id} body {"enabled": bool} and/or {"dwell_seconds": n}.
void handle_widget_put(AsyncWebServerRequest* request, JsonVariant& json) {
    const String id = request->url().substring(strlen("/api/widgets/"));
    if (!json.is<JsonObject>() ||
        (!json["enabled"].is<bool>() && !json["dwell_seconds"].is<float>() &&
         !json["dwell_seconds"].is<long>())) {
        request->send(400, "application/json", "{\"error\":\"enabled or dwell_seconds required\"}");
        return;
    }
    config::Config c;
    config::load(c);
    int j = widget_find(c, id);
    if (j < 0) {
        request->send(404, "application/json", "{\"error\":\"unknown widget\"}");
        return;
    }
    if (json["enabled"].is<bool>()) c.widgets[j].enabled = json["enabled"].as<bool>() ? 1 : 0;
    if (json["dwell_seconds"].is<float>() || json["dwell_seconds"].is<long>()) {
        const long dw = lclamp(static_cast<long>(json["dwell_seconds"].as<float>()), 1, 120);
        c.widgets[j].dwell_s = static_cast<float>(dw);
    }
    if (!config::save(c)) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
    }
    JsonDocument d;
    widget_json(d.to<JsonObject>(), c.widgets[j]);
    send_json(request, 200, d);
}

// POST /api/widgets/reorder body {"ids": ["clock", ...]} — drag-to-reorder.
// Order is positional; ids not listed keep their relative order at the tail.
void handle_widgets_reorder(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>() || !json["ids"].is<JsonArray>()) {
        request->send(400, "application/json", "{\"error\":\"ids array required\"}");
        return;
    }
    config::Config c;
    config::load(c);
    bool assigned[config::kMaxWidgets] = {};
    uint16_t next = 0;
    const JsonArrayConst ids = json["ids"].as<JsonArrayConst>();  // bound, not inline — gcc13 -Wdangling-reference
    for (JsonVariantConst v : ids) {
        if (!v.is<const char*>()) continue;
        const int j = widget_find(c, String(v.as<const char*>()));
        if (j >= 0 && !assigned[j]) {
            c.widgets[j].order = next++;
            assigned[j] = true;
        }
    }
    for (int i = 0; i < c.widget_count; ++i)
        if (!assigned[i]) c.widgets[i].order = next++;
    if (!config::save(c)) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
    }
    JsonDocument d;
    d["ok"] = true;
    send_json(request, 200, d);
}

// ─── T-8.6 — /preview (24-bit BMP, 54-byte header, no zlib) ──────────────

const render::StripHolder* g_strip = nullptr;
void (*g_show_ip_hook)() = nullptr;

// Response-owned PSRAM buffer: the chunked filler's std::function keeps the
// shared_ptr alive exactly as long as the socket flush needs it — no static
// buffer to race, no leak. (send(uint8_t*,len) would alias: v3's
// AsyncProgmemResponse stores the pointer without copying.)
struct BmpBuf {
    uint8_t* p = nullptr;
    size_t n = 0;
    ~BmpBuf() { heap_caps_free(p); }
};

void handle_preview_get(AsyncWebServerRequest* request) {
    if (g_strip == nullptr) {
        request->send(503, "application/json", "{\"error\":\"preview not wired\"}");
        return;
    }
    const render::Strip* s = g_strip->front();
    if (s == nullptr || !s->canvas.valid()) {
        request->send(503, "application/json", "{\"error\":\"no strip yet\"}");
        return;
    }
    const int w = s->canvas.w, h = s->canvas.h;
    const uint32_t row = bmp_row(static_cast<uint32_t>(w));
    const uint32_t img = row * static_cast<uint32_t>(h);
    auto bmp = std::make_shared<BmpBuf>();
    bmp->n = 54 + img;
    bmp->p = static_cast<uint8_t*>(heap_caps_malloc(bmp->n, MALLOC_CAP_SPIRAM));
    if (bmp->p == nullptr) {
        request->send(500, "application/json", "{\"error\":\"preview alloc failed\"}");
        return;
    }
    write_bmp_header(bmp->p, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    uint8_t* b = bmp->p;
    for (uint32_t r = 0; r < static_cast<uint32_t>(h); ++r) {  // BMP row 0 = bottom
        const int y = h - 1 - static_cast<int>(r);
        uint8_t* dst = b + 54 + r * row;
        for (int x = 0; x < w; ++x) {
            uint8_t red, gr, bl;
            nb::unpack565(s->canvas.get(x, y), red, gr, bl);
            dst[3 * x] = bl;
            dst[3 * x + 1] = gr;
            dst[3 * x + 2] = red;
        }
        for (uint32_t p = static_cast<uint32_t>(w) * 3; p < row; ++p) dst[p] = 0;  // pad to 4
    }
    request->sendChunked("image/bmp",
                         [bmp](uint8_t* dst, size_t len, size_t index) -> size_t {
                             if (index >= bmp->n) return 0;
                             const size_t n = len < bmp->n - index ? len : bmp->n - index;
                             memcpy(dst, bmp->p + index, n);
                             return n;
                         });
}

}  // namespace

bool init() {
    if (g_server != nullptr) return true;

    const uint32_t f0 = internal_free();
    const uint32_t l0 = internal_largest();

    AsyncWebServer* srv = new (std::nothrow) AsyncWebServer(80);
    if (srv == nullptr) return false;

    const uint32_t f1 = internal_free();
    const uint32_t l1 = internal_largest();

    const bool fs_mounted = g_fs_mounted || SPIFFS.begin(true, "/web", 10, "web");
    if (fs_mounted) {
        g_fs_mounted = true;
        Serial.printf("[web] mounted web partition (%lu/%lu B)\n",
                      static_cast<unsigned long>(SPIFFS.usedBytes()),
                      static_cast<unsigned long>(SPIFFS.totalBytes()));
    } else {
        Serial.println("[web] web partition mount failed");
    }

    const uint32_t f2 = internal_free();
    const uint32_t l2 = internal_largest();

    srv->on(AsyncURIMatcher::exact("/ping"), AsyncWebRequestMethod::HTTP_GET,
            [](AsyncWebServerRequest* request) { request->send(200, "text/plain", "pong"); });

    if (g_fs_mounted) {
        const auto serve_index = [](AsyncWebServerRequest* request) {
            request->send(SPIFFS, "/index.html");
        };
        srv->on(AsyncURIMatcher::exact("/"), AsyncWebRequestMethod::HTTP_GET, serve_index);
        srv->on(AsyncURIMatcher::exact("/index.html"), AsyncWebRequestMethod::HTTP_GET, serve_index);
        // T-8.7 — vendored onboarding page (inline CSS, no CDN; T-9.1's
        // captive portal serves the same file from the web partition).
        const auto serve_onboard = [](AsyncWebServerRequest* request) {
            request->send(SPIFFS, "/onboard.html");
        };
        srv->on(AsyncURIMatcher::exact("/onboard"), AsyncWebRequestMethod::HTTP_GET, serve_onboard);
        srv->on(AsyncURIMatcher::exact("/onboard.html"), AsyncWebRequestMethod::HTTP_GET,
                serve_onboard);
    }

    // T-8.3 — /api/settings (GET read, PUT write) and /api/system.
    srv->on(AsyncURIMatcher::exact("/api/settings"), AsyncWebRequestMethod::HTTP_GET,
            [](AsyncWebServerRequest* request) {
                config::Config c;
                config::load(c);
                JsonDocument d;
                settings_json(c, d);
                send_json(request, 200, d);
            });
    auto* settings_put = new (std::nothrow)
        AsyncCallbackJsonWebHandler(AsyncURIMatcher::exact("/api/settings"), handle_settings_put);
    if (settings_put != nullptr) {
        settings_put->setMethod(AsyncWebRequestMethod::HTTP_PUT);
        settings_put->setMaxContentLength(1024);
        srv->addHandler(settings_put);
    }
    srv->on(AsyncURIMatcher::exact("/api/system"), AsyncWebRequestMethod::HTTP_GET,
            handle_system_get);

    // T-8.4 — sports and favourites. Registration order matters: the exact
    // leagues route must win over the /api/sports/ prefix (first canHandle
    // wins in AsyncWebServer).
    srv->on(AsyncURIMatcher::exact("/api/sports/leagues"), AsyncWebRequestMethod::HTTP_GET,
            handle_leagues_get);
    auto* league_put = new (std::nothrow)
        AsyncCallbackJsonWebHandler(AsyncURIMatcher::prefix("/api/sports/leagues/"), handle_league_put);
    if (league_put != nullptr) {
        league_put->setMethod(AsyncWebRequestMethod::HTTP_PUT);
        league_put->setMaxContentLength(256);
        srv->addHandler(league_put);
    }
    srv->on(AsyncURIMatcher::prefix("/api/sports/"), AsyncWebRequestMethod::HTTP_GET,
            handle_teams_get);
    srv->on(AsyncURIMatcher::exact("/api/favorites"), AsyncWebRequestMethod::HTTP_GET,
            handle_favorites_get);
    auto* fav_post = new (std::nothrow)
        AsyncCallbackJsonWebHandler(AsyncURIMatcher::exact("/api/favorites"), handle_favorite_post);
    if (fav_post != nullptr) {
        fav_post->setMethod(AsyncWebRequestMethod::HTTP_POST);
        fav_post->setMaxContentLength(512);
        srv->addHandler(fav_post);
    }
    srv->on(AsyncURIMatcher::prefix("/api/favorites/"), AsyncWebRequestMethod::HTTP_DELETE,
            handle_favorite_delete);

    // T-8.5 — widget carousel. /api/widgets/reorder (POST, exact) is a
    // different method from PUT /api/widgets/{id} (prefix) — no shadowing.
    srv->on(AsyncURIMatcher::exact("/api/widgets"), AsyncWebRequestMethod::HTTP_GET,
            handle_widgets_get);
    auto* widget_put = new (std::nothrow)
        AsyncCallbackJsonWebHandler(AsyncURIMatcher::prefix("/api/widgets/"), handle_widget_put);
    if (widget_put != nullptr) {
        widget_put->setMethod(AsyncWebRequestMethod::HTTP_PUT);
        widget_put->setMaxContentLength(256);
        srv->addHandler(widget_put);
    }
    auto* widgets_reorder = new (std::nothrow) AsyncCallbackJsonWebHandler(
        AsyncURIMatcher::exact("/api/widgets/reorder"), handle_widgets_reorder);
    if (widgets_reorder != nullptr) {
        widgets_reorder->setMethod(AsyncWebRequestMethod::HTTP_POST);
        widgets_reorder->setMaxContentLength(1024);
        srv->addHandler(widgets_reorder);
    }

    // T-8.6 — /preview as BMP; the SPA's dashboard uses the
    // /api/system/preview alias, and its "Show IP" button the POST.
    srv->on(AsyncURIMatcher::exact("/preview"), AsyncWebRequestMethod::HTTP_GET,
            handle_preview_get);
    srv->on(AsyncURIMatcher::exact("/api/system/preview"), AsyncWebRequestMethod::HTTP_GET,
            handle_preview_get);
    srv->on(AsyncURIMatcher::exact("/api/system/show-ip"), AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest* request) {
                if (g_show_ip_hook != nullptr) g_show_ip_hook();
                JsonDocument d;
                d["ok"] = true;
                send_json(request, 200, d);
            });

    // T-9.6 — factory reset: clear the "nb" NVS namespace (settings + WiFi
    // credentials), leave firmware, logos and the web partition alone. The
    // reboot lands the device in first-boot state: seeded defaults and the
    // provisioning AP (net.cpp sees no stored credentials and no built-in
    // target). The confirm arg is the accident guard on this path; the
    // physical path is the 5 s BOOT hold (main.cpp loop()). Query or form
    // arg both work — ESPAsyncWebServer folds query params into arg().
    srv->on(AsyncURIMatcher::exact("/api/system/factory-reset"), AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest* request) {
                JsonDocument d;
                if (request->arg("confirm") != "1") {
                    d["ok"] = false;
                    d["error"] = "this erases all settings and wifi credentials; resend with confirm=1";
                    send_json(request, 400, d);
                    return;
                }
                if (!config::reset()) {
                    d["ok"] = false;
                    d["error"] = "NVS clear failed";
                    send_json(request, 500, d);
                    return;
                }
                Serial.println("[web] factory reset: NVS cleared, rebooting");
                d["ok"] = true;
                d["rebooting"] = true;
                send_json(request, 200, d);
                if (g_reboot_hook != nullptr) g_reboot_hook("factory reset: NVS cleared");
            });

    // T-9.4 — firmware OTA upload target.
    srv->addHandler(&g_ota);

    // T-9.5 — logo atlas update: store the URL (https only, so atlas bytes
    // ride the same TLS discipline as everything else), drop the stale
    // ETag, and flag the poll task to run the check on its next pass
    // (single TLS-session rule keeps the fetch out of this task).
    srv->on(AsyncURIMatcher::exact("/api/logos/update"), AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest* request) {
                JsonDocument d;
                const String url = request->arg("url");
                if (url.length() == 0 || url.length() > 255 ||
                    !url.startsWith("https://")) {
                    d["ok"] = false;
                    d["error"] = "https url arg required";
                    send_json(request, 400, d);
                    return;
                }
                if (!nb::logos::ota::set_url(url.c_str())) {
                    d["ok"] = false;
                    d["error"] = "NVS write failed";
                    send_json(request, 500, d);
                    return;
                }
                d["ok"] = true;
                d["scheduled"] = true;
                send_json(request, 200, d);
            });

    // T-9.1 — provisioning POST: onboard.html's form target (urlencoded
    // ssid/pass; the server parses plain POST bodies into arg()). There
    // is deliberately NO GET counterpart and nothing here logs a value —
    // credentials exist only in the separate NVS blob (T-9.2). On accept,
    // nudge the net task to drop its backoff and try the new network.
    srv->on(AsyncURIMatcher::exact("/api/net/connect"), AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest* request) {
                nb::config::Creds c = {};
                strlcpy(c.ssid, request->arg("ssid").c_str(), sizeof(c.ssid));
                strlcpy(c.pass, request->arg("pass").c_str(), sizeof(c.pass));
                JsonDocument d;
                if (!nb::config::save_creds(c)) {  // save validates (lengths + no control chars)
                    d["ok"] = false;
                    d["error"] = "ssid/password rejected or storage write failed";
                    send_json(request, 400, d);
                    return;
                }
                nb::net::request_reconnect();
                d["ok"] = true;
                d["ssid"] = request->arg("ssid");  // echoing the network name is fine — just submitted
                send_json(request, 200, d);
            });

    // T-9.1 — captive-portal capture. While the provisioning AP is up,
    // every unknown path — the phone's connectivity probes
    // (generate_204, hotspot-detect.html, …) included — 302s to the
    // onboarding page so the portal opens itself. Outside AP mode, plain
    // 404 as before: the SPA keeps its not-found behaviour.
    srv->onNotFound([](AsyncWebServerRequest* request) {
        if (nb::net::ap_active()) {
            AsyncWebServerResponse* r = request->beginResponse(302, "text/plain", "");
            r->addHeader("Location", String("http://") + nb::net::ap_ip() + "/onboard");
            request->send(r);
        } else {
            request->send(404);
        }
    });

    const uint32_t f3 = internal_free();
    const uint32_t l3 = internal_largest();

    srv->begin();

    const uint32_t f4 = internal_free();
    const uint32_t l4 = internal_largest();

    Serial.printf("[web] listening on :80, internal heap cost server=%lu fs=%lu routes=%lu "
                  "begin=%lu total=%lu B (free %lu B, largest %lu B)\n",
                  static_cast<unsigned long>(f0 - f1), static_cast<unsigned long>(f1 - f2),
                  static_cast<unsigned long>(f2 - f3), static_cast<unsigned long>(f3 - f4),
                  static_cast<unsigned long>(f0 - f4), static_cast<unsigned long>(f4),
                  static_cast<unsigned long>(l4));
    Serial.printf("[web] largest free block: %lu -> %lu B\n", static_cast<unsigned long>(l0),
                  static_cast<unsigned long>(l4));

    g_server = srv;
    return true;
}

void set_preview_source(const render::StripHolder* holder) { g_strip = holder; }
void set_show_ip_hook(void (*hook)()) { g_show_ip_hook = hook; }
void set_reboot_hook(void (*hook)(const char*)) { g_reboot_hook = hook; }

}  // namespace web
}  // namespace nb

#endif  // ARDUINO
