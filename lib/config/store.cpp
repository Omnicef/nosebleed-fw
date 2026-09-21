// SPDX-License-Identifier: GPL-3.0-only
//
// T-4.2 — NVS wrapper. Verified against
// ~/.platformio/packages/framework-arduinoespressif32/libraries/Preferences/
// src/Preferences.h. PlatformIO's LLD compiles the whole lib/config dir for
// both envs (same #ifdef ARDUINO pattern as src/main.cpp).

#include "store.h"

#ifdef ARDUINO

#include <Preferences.h>
#include <cstring>

namespace nb {
namespace config {

namespace {
constexpr char kNs[] = "nb";
constexpr char kKey[] = "cfg";
constexpr char kKeyNet[] = "net";  // T-9.2 — credentials live here, not in cfg
constexpr char kKeyKeys[] = "keys";  // T-10.3 — ticker API keys, same rule
TaskHandle_t s_render = nullptr;
}  // namespace

void bind_render_task(TaskHandle_t render) { s_render = render; }

bool load(Config& out) {
    Preferences p;
    if (p.begin(kNs)) {
        const size_t len = p.getBytesLength(kKey);
        const bool ok = len == sizeof(Config) &&
                        p.getBytes(kKey, &out, sizeof(out)) == sizeof(Config) &&
                        validate(out);
        p.end();
        if (ok) return true;
    }
    set_defaults(out);
    save(out);  // persist so absent/corrupt state self-heals on next boot
    return false;
}

bool save(const Config& c) {
    Preferences p;
    if (!p.begin(kNs)) return false;
    const size_t put = p.putBytes(kKey, &c, sizeof(c));
    p.end();
    if (put != sizeof(c)) return false;
    if (s_render) xTaskNotifyGive(s_render);
    return true;
}

bool reset() {
    Preferences p;
    if (!p.begin(kNs)) return false;
    // Whole-namespace clear (T-9.6): cfg, creds and every key added later
    // (logo OTA url/etag, …) without touching this function. WiFi keeps its
    // own namespace (nvs.net80211), as does every other component — none of
    // it is "our" config, and none of it is cleared.
    const bool cleared = p.clear();
    p.end();
    return cleared;
}

// T-9.2 — credentials. Separate blob, no logging anywhere in this path.

bool load_creds(Creds& out) {
    Preferences p;
    if (!p.begin(kNs)) return false;
    const size_t len = p.getBytesLength(kKeyNet);
    const bool ok = len == sizeof(Creds) &&
                    p.getBytes(kKeyNet, &out, sizeof(out)) == sizeof(Creds) &&
                    creds_valid(out);
    p.end();
    if (!ok) std::memset(&out, 0, sizeof(out));
    return ok;
}

bool save_creds(const Creds& c) {
    if (!creds_valid(c)) return false;
    Preferences p;
    if (!p.begin(kNs)) return false;
    const size_t put = p.putBytes(kKeyNet, &c, sizeof(c));
    p.end();
    return put == sizeof(c);
}

bool clear_creds() {
    Preferences p;
    if (!p.begin(kNs)) return false;
    const bool had = p.getBytesLength(kKeyNet) > 0;
    const bool gone = !had || p.remove(kKeyNet);
    p.end();
    return gone;
}

// T-10.3 — ticker API keys. Separate blob, same no-logging discipline.
// An absent blob is NOT an error: no keys is a valid steady state
// (news/stocks off, crypto still works).

bool load_keys(ApiKeys& out) {
    Preferences p;
    if (!p.begin(kNs)) {
        std::memset(&out, 0, sizeof(out));
        return false;
    }
    // isKey first: "no keys stored" is the NORMAL steady state, and
    // getBytesLength on an absent key prints an [E] line every time —
    // which at the poll task's 1 s tick was once per second.
    const size_t len = p.isKey(kKeyKeys) ? p.getBytesLength(kKeyKeys) : 0;
    const bool ok = len == sizeof(ApiKeys) &&
                    p.getBytes(kKeyKeys, &out, sizeof(out)) == sizeof(ApiKeys) &&
                    api_keys_valid(out);
    p.end();
    if (!ok) std::memset(&out, 0, sizeof(out));
    return ok;
}

bool save_keys(const ApiKeys& k) {
    if (!api_keys_valid(k)) return false;
    Preferences p;
    if (!p.begin(kNs)) return false;
    const size_t put = p.putBytes(kKeyKeys, &k, sizeof(k));
    p.end();
    return put == sizeof(k);
}

}  // namespace config
}  // namespace nb

#endif  // ARDUINO
