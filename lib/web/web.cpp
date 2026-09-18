// SPDX-License-Identifier: GPL-3.0-only

#include "web.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>

#include <new>

namespace nb {
namespace web {
namespace {

AsyncWebServer* g_server = nullptr;
bool g_fs_mounted = false;

uint32_t internal_free() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

uint32_t internal_largest() {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
