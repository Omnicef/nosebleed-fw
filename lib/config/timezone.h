// SPDX-License-Identifier: GPL-3.0-only
// T-4.6 — resolve stored IANA name to a POSIX TZ string (tzmap) and install
// it via setenv/tzset. Plain C library: compiles and behaves identically on
// host (glibc) and device (newlib); newlib has no tzdata, the POSIX string
// IS the zone, which is exactly what the generated table encodes.
#pragma once

namespace nb {
namespace config {

enum class TzResult {
    kApplied,     // named zone installed
    kUtcEmpty,    // config timezone "" -> UTC; UI must show the indicator
    kUtcUnknown,  // name not in kTzTable -> UTC fallback, restart-safe
};

TzResult apply_timezone(const char* iana);

}  // namespace config
}  // namespace nb
