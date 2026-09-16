// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.3 — HTTP transport. esp_http_client + esp_crt_bundle (never a pinned
// cert), 15 s timeout, explicit `Accept-Encoding: identity` (we cannot
// inflate), and the Akamai UA-allowlist User-Agent measured at T-0.4/T-0.5.
// HttpStream is the proven T-0.5 Stream view so StreamUtils'
// ReadBufferingStream pulls ≥512 B chunks, not single bytes, off the TLS
// socket. Retries match Marquee's _get(): 3 retries, 0.5/1.0/2.0 s with
// 0–300 ms added jitter.
//
// Session hygiene: exactly one cleanup per init on every path, including
// mid-body failure — "never leaks a session" holds by construction, and the
// httptest env measures the heap delta to prove it. One session at a time is
// the caller's discipline (T-5.7: the poll task is the only caller, and it
// is sequential by design).
//
// Device-only (esp/Arduino headers), like the rest of lib/data except
// game.h/cache.h.

#pragma once

#include <cstddef>

#include <Arduino.h>
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_crt_bundle.h"

namespace nb {
namespace data {

// Stream view over an open esp_http_client (T-0.5 spike pattern). readBytes
// is the hot path — it is what ReadBufferingStream drives.
class HttpStream : public Stream {
  public:
    explicit HttpStream(esp_http_client_handle_t h) : h_(h) {}
    int read() override {
        char b;
        const int r = esp_http_client_read(h_, &b, 1);
        if (r > 0) { bytes_ += 1; return static_cast<uint8_t>(b); }
        return -1;
    }
    int available() override { return 0; }  // unknown in advance; unused
    int peek() override { return -1; }
    size_t write(uint8_t) override { return 1; }  // read-only; never called
    size_t readBytes(char* buf, size_t n) override {
        const int r = esp_http_client_read(h_, buf, (int)n);
        if (r > 0) { bytes_ += (size_t)r; return (size_t)r; }
        return 0;
    }
    size_t bytes() const { return bytes_; }

  private:
    esp_http_client_handle_t h_;
    size_t bytes_ = 0;
};

struct HttpStat {
    int status = 0;          // HTTP status, 0 = never got one
    int64_t clen = -1;       // Content-Length, -1 = absent (header signature: int64_t)
};

// One GET attempt. On HTTP 200, fn(HttpStream&) reads the body; its return
// value is this call's success. Always cleans up exactly once.
template <typename Fn>
bool http_get_once(const char* url, Fn fn, HttpStat* stat = nullptr) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.user_agent = "python-requests/2.31";  // Akamai UA allowlist (T-0.4)
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 1024;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    HttpStat local;
    bool ok = false;
    do {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        if (esp_http_client_fetch_headers(c) < 0) break;
        local.status = esp_http_client_get_status_code(c);
        local.clen = esp_http_client_get_content_length(c);
        if (local.status != 200) break;
        HttpStream s(c);
        ok = fn(s);
    } while (0);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (stat) *stat = local;
    return ok;
}

// Marquee _get(): 3 retries, exponential 0.5/1.0/2.0 s + uniform 0–300 ms
// jitter. Returns the last attempt's stat.
template <typename Fn>
bool http_get(const char* url, Fn fn, HttpStat* stat = nullptr) {
    static const uint32_t kBackoffMs[4] = {0, 500, 1000, 2000};
    bool ok = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(kBackoffMs[attempt] + (esp_random() % 300)));
        }
        if ((ok = http_get_once(url, fn, stat))) break;
    }
    return ok;
}

}  // namespace data
}  // namespace nb
