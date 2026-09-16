// SPDX-License-Identifier: GPL-3.0-only
//
// T-4.2 — NVS wrapper. Verified against
// ~/.platformio/packages/framework-arduinoespressif32/libraries/Preferences/
// src/Preferences.h. PlatformIO's LLD compiles the whole lib/config dir for
// both envs (same #ifdef ARDUINO pattern as src/main.cpp).

#include "store.h"

#ifdef ARDUINO

#include <Preferences.h>

namespace nb {
namespace config {

namespace {
constexpr char kNs[] = "nb";
constexpr char kKey[] = "cfg";
}  // namespace

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
    return put == sizeof(c);
}

bool reset() {
    Preferences p;
    if (!p.begin(kNs)) return false;
    const bool had = p.getBytesLength(kKey) > 0;
    const bool gone = !had || p.remove(kKey);
    p.end();
    return gone;
}

}  // namespace config
}  // namespace nb

#endif  // ARDUINO
