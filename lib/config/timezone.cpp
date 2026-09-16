// SPDX-License-Identifier: GPL-3.0-only
#include "timezone.h"

#include <cstdlib>
#include <ctime>

#include "tzmap.h"

namespace nb {
namespace config {

TzResult apply_timezone(const char* iana) {
    if (!iana || !*iana) {
        setenv("TZ", "UTC0", 1);
        tzset();
        return TzResult::kUtcEmpty;
    }
    if (const char* posix = tz_lookup(iana)) {
        setenv("TZ", posix, 1);
        tzset();
        return TzResult::kApplied;
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    return TzResult::kUtcUnknown;
}

}  // namespace config
}  // namespace nb
