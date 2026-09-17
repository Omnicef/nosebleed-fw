// SPDX-License-Identifier: GPL-3.0-only
//
// Injectable local-calendar time. lib/render must not depend on platform
// state, but clock/game cards need local hour/minute and local date bounds.
// The caller supplies the conversion, so host tests can fake an exact TZ.

#pragma once

#include <cstdint>
#include <ctime>

namespace nb {
namespace render {

struct LocalTime {
    int wday = 0;    // 0 = Sunday, matching struct tm
    int month = 1;   // 1-12
    int day = 1;     // 1-31
    int hour = 0;    // 0-23
    int minute = 0;  // 0-59
};

using LocalTimeFn = bool (*)(void* ctx, int64_t now_utc, LocalTime& out);

inline bool system_local_time(void*, int64_t now_utc, LocalTime& out) {
    const time_t t = static_cast<time_t>(now_utc);
    struct tm local;
    if (localtime_r(&t, &local) == nullptr) return false;
    out.wday = local.tm_wday;
    out.month = local.tm_mon + 1;
    out.day = local.tm_mday;
    out.hour = local.tm_hour;
    out.minute = local.tm_min;
    return true;
}

}  // namespace render
}  // namespace nb
