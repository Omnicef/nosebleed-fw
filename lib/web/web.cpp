// SPDX-License-Identifier: GPL-3.0-only

#include "web.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <new>

#include <cstring>

#include "../config/config.h"
#include "../config/store.h"

#ifndef NB_FW_VERSION
#define NB_FW_VERSION "dev"
#endif

namespace nb {
namespace web {
namespace {

AsyncWebServer* g_server = nullptr;
bool g_fs_mounted = false;

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

// PUT /api/settings body — the SPA's full form. Absent keys keep their
// stored values; out-of-range numbers are clamped, wrong types ignored.
void handle_settings_put(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>()) {
        request->send(400, "application/json", "{\"error\":\"expected a JSON object\"}");
        return;
    }
    config::Config c, old;
    config::load(c);
    old = c;
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

    const bool structural = config::hw_structural_changed(old.hw, c.hw);
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

}  // namespace web
}  // namespace nb

#endif  // ARDUINO
