// SPDX-License-Identifier: GPL-3.0-only
// T-5.4 — device transport for espn_json: one buffered filtered fetch.
// Compiled only under Arduino (native builds see the decoder alone; the
// chain LDF compiles every .cpp in a used lib dir).

#include "espn.h"

#if defined(ARDUINO)

#include <cstdio>

#include <StreamUtils.h>

#include "esp_heap_caps.h"

#include "espn_json.h"
#include "http.h"

namespace nb {
namespace data {
namespace {

struct SpiRamAllocator : ArduinoJson::Allocator {
    void* allocate(size_t s) override { return heap_caps_malloc(s, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t s) override { return heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM); }
};

SpiRamAllocator g_psram_alloc;

}  // namespace

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
    JsonDocument filter(&g_psram_alloc);
    build_scoreboard_filter(filter);
    DeserializationError err;  // never read unless the lambda ran (http_get ok==false otherwise)
    const bool ok = http_get(url, [&](HttpStream& s) {
        ReadBufferingStream rs(s, 512);
        err = parse_scoreboard(rs, filter, doc);
        if (wire) *wire = s.bytes();  // self-counting stream (ESPN replies chunked)
        return err == DeserializationError::Ok && !doc.overflowed();
    });
    return ok;
}

}  // namespace data
}  // namespace nb

#endif  // ARDUINO
