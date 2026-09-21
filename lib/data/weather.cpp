// SPDX-License-Identifier: GPL-3.0-only
// T-10.1 — device transport for the weather client: one buffered filtered
// fetch inside the shared one-TLS-session mutex, exactly the espn.cpp
// chain (tls_take → http_get → ReadBufferingStream → parse → decode).
// Compiled only under Arduino (native builds see the pure decoder alone).

#include "weather_json.h"

#if defined(ARDUINO)

#include <StreamUtils.h>

#include "espn.h"  // tls_take / tls_release / make_psram_doc
#include "http.h"

namespace nb {
namespace data {

bool weather_fetch(const char* url, Weather& out) {
    if (!tls_take(15000)) return false;
    JsonDocument filter;  // ~300 B — internal heap, like the e-docs in espn.cpp
    build_weather_filter(filter);
    JsonDocument doc = make_psram_doc();
    DeserializationError err;  // only read when the lambda ran
    const bool ok = http_get(url, [&](HttpStream& s) {
        ReadBufferingStream rs(s, 512);
        err = parse_weather(rs, filter, doc);
        return err == DeserializationError::Ok && !doc.overflowed();
    });
    tls_release();
    return ok && to_weather(doc, out);
}

}  // namespace data
}  // namespace nb

#endif  // ARDUINO
