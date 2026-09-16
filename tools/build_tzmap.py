#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T-4.5 — generate lib/config/tzmap.h: IANA name -> POSIX TZ string.

newlib (ESP32 and this host alike) needs POSIX TZ strings; config stores
IANA names. We derive each zone's POSIX rule by sampling its real tzdata
transitions in 2026 and encoding them as Mm.w.d[/hh].

Known approximations, deliberately accepted for a clock widget:
  * Ramadan-style moving rules (Africa/Casablanca, Africa/Cairo) are frozen
    to 2026's dates; the clock can be off days around them.
  * Numeric tz abbreviations ("+03") become "XXX" — POSIX names must not
    start with a digit/sign. Offset is preserved, that's what the clock needs.

Deterministic given the host tzdata; the output is committed so CI and
device always agree regardless of the builder's tzdata version.
"""

import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo, available_timezones

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "lib" / "config" / "tzmap.h"

# main IANA dirs only; posix/, right/ and top-level files are dups/synonyms
MAIN_DIRS = ("Africa", "America", "Antarctica", "Arctic", "Asia",
             "Atlantic", "Australia", "Etc", "Europe", "Indian", "Pacific")

Y0, Y1 = 2026, 2031  # sampling window; 2026's rule is the one we encode


def offset_at(z: ZoneInfo, t: int) -> int:
    return int(datetime.fromtimestamp(t, z).utcoffset().total_seconds()) // 60


def transitions(z: ZoneInfo) -> list[int]:
    """Offset-change instants (epoch s) in [Y0, Y1), found by 1-day scan
    then a second-level binary refine."""
    t0 = int(datetime(Y0, 1, 1, tzinfo=timezone.utc).timestamp())
    t1 = int(datetime(Y1, 1, 1, tzinfo=timezone.utc).timestamp())
    out, prev, x = [], offset_at(z, t0), t0 + 86400
    while x < t1:
        o = offset_at(z, x)
        if o != prev:
            lo, hi = x - 86400, x
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if offset_at(z, mid) == prev:
                    lo = mid
                else:
                    hi = mid
            out.append(hi)
            prev = o
        x += 86400
    return out


def ttinfo(z: ZoneInfo, ts: int):
    """(abbreviation, utc-offset minutes) in effect at epoch seconds ts."""
    dt = datetime.fromtimestamp(ts, timezone.utc).astimezone(z)
    return dt.tzname(), int(dt.utcoffset().total_seconds() // 60)


def clean_abbr(abbr: str) -> str:
    return abbr if (abbr[0].isalpha() or abbr[0] == "_") else "XXX"


def fmt_offset(total_min: int) -> str:
    """UTC offset minutes -> POSIX sign-inverted offset (west positive)."""
    w = -total_min
    h, m = abs(w) // 60, abs(w) % 60
    return ("-" if w < 0 else "") + str(h) + (f":{m:02d}" if m else "")


def mwd(dt_local: datetime) -> str:
    """Local date -> POSIX Mm.w.d. w=5 means 'last'."""
    import calendar
    week = (dt_local.day - 1) // 7 + 1
    if dt_local.day + 7 > calendar.monthrange(dt_local.year, dt_local.month)[1]:
        week = 5  # next week would leave the month
    return f"M{dt_local.month}.{week}.{(dt_local.weekday() + 1) % 7}"  # 0=Sun


def posix_for(name: str) -> str:
    z = ZoneInfo(name)
    jan = datetime(2026, 1, 15, 12, tzinfo=timezone.utc)
    jul = datetime(2026, 7, 15, 12, tzinfo=timezone.utc)
    jan_off = int(jan.astimezone(z).utcoffset().total_seconds()) // 60
    jul_off = int(jul.astimezone(z).utcoffset().total_seconds()) // 60
    trans = transitions(z)

    if not trans or jan_off == jul_off:  # fixed offset all year
        abbr, off = ttinfo(z, int(jul.timestamp()))
        return f"{clean_abbr(abbr)}{fmt_offset(off)}"

    # Zones that settle onto one offset and stop shifting (America/Vancouver
    # goes permanent-MST from Nov 2026 in current tzdata): no M-rule can
    # express that — emit the terminal fixed offset, correct from now on.
    end_epoch = int(datetime(Y1, 1, 1, tzinfo=timezone.utc).timestamp())
    if end_epoch - trans[-1] > 400 * 86400:
        abbr, off = ttinfo(z, end_epoch - 86400)
        return f"{clean_abbr(abbr)}{fmt_offset(off)}"

    std_when, dst_when = (jan, jul) if jan_off < jul_off else (jul, jan)
    std = ttinfo(z, int(std_when.timestamp()))
    dst = ttinfo(z, int(dst_when.timestamp()))

    # POSIX rule time = wall clock read just before the jump: t + previous
    # offset. Earlier transition = DST start (offset increased).
    starts, ends = [], []
    for t in trans:
        before_min = ttinfo(z, t - 1)[1]
        after_min = ttinfo(z, t)[1]
        pre = datetime.fromtimestamp(t, timezone.utc) + timedelta(seconds=before_min * 60)
        (starts if after_min > before_min else ends).append(pre)
    s = next(p for p in starts if p.year == 2026)
    e = next(p for p in ends if p.year == 2026)

    def rule(pre: datetime) -> str:
        r = mwd(pre)
        hh, mm = pre.hour, pre.minute  # POSIX default is 02:00 local
        if hh != 2 or mm:
            r += f"/{hh}" + (f":{mm:02d}" if mm else "")
        return r

    dst_off = "" if dst[1] == std[1] + 60 else fmt_offset(dst[1])
    return (f"{clean_abbr(std[0])}{fmt_offset(std[1])}"
            f"{clean_abbr(dst[0])}{dst_off},{rule(s)},{rule(e)}")


def main() -> int:
    zones = sorted(z for z in available_timezones()
                   if z.split("/")[0] in MAIN_DIRS and "/" in z)
    zones += [z for z in ("GMT", "UTC", "UCT", "Zulu") if z in available_timezones()]
    entries = []
    for z in sorted(set(zones)):
        try:
            entries.append((z, posix_for(z)))
        except Exception as e:  # tzdata oddity (e.g. Factory) — skip
            print(f"  skip {z}: {e}", file=sys.stderr)

    name_pool, posix_pool = bytearray(), bytearray()
    posix_off, rows = {}, []
    for name, p in entries:
        noff = len(name_pool)
        name_pool += name.encode() + b"\0"
        if p not in posix_off:
            posix_off[p] = len(posix_pool)
            posix_pool += p.encode() + b"\0"
        rows.append((noff, posix_off[p], name, p))

    total = len(name_pool) + len(posix_pool) + 4 * len(rows)
    if total >= 20480:
        print(f"FAIL: table {total} B >= 20 KB budget", file=sys.stderr)
        return 1

    def pool_bytes(pool):
        return "".join(
            '    "' + "".join(f"\\x{b:02x}" for b in pool[i:i + 12]) + '"\n'
            for i in range(0, len(pool), 12))

    body = f"""// SPDX-License-Identifier: GPL-3.0-only
//
// T-4.5 — generated by tools/build_tzmap.py; DO NOT EDIT (rerun the tool).
// IANA name -> POSIX TZ string, pools deduped, {len(rows)} zones, {total} B
// (budget 20 KB). Sampled from tzdata {Y0} transitions; see the tool header
// for the accepted approximations. Lookup is a binary search over kTzTable.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nb {{
namespace config {{

struct TzEntry {{
    uint16_t name_off;
    uint16_t posix_off;
}};

inline constexpr char kTzNamePool[] =
{pool_bytes(name_pool)};
inline constexpr char kTzPosixPool[] =
{pool_bytes(posix_pool)};

inline constexpr TzEntry kTzTable[] = {{
"""
    for noff, poff, name, p in rows:
        body += f"    {{{noff}, {poff}}},  // {name} -> {p}\n"
    body += """};

inline constexpr size_t kTzCount = sizeof(kTzTable) / sizeof(kTzTable[0]);

// nullptr = unknown zone; "" input = caller's UTC-with-indicator case.
inline const char* tz_lookup(const char* iana) {
    if (!iana || !*iana) return nullptr;
    size_t lo = 0, hi = kTzCount;
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        const int r = std::strcmp(kTzNamePool + kTzTable[m].name_off, iana);
        if (r == 0) return kTzPosixPool + kTzTable[m].posix_off;
        if (r < 0) lo = m + 1; else hi = m;
    }
    return nullptr;
}

static_assert(sizeof(kTzNamePool) + sizeof(kTzPosixPool) < 20480,
              "tzmap must stay under 20 KB (PLAN T-4.5)");

}  // namespace config
}  // namespace nb
"""
    OUT.write_text(body)
    print(f"{OUT}: {len(rows)} zones, {total} B "
          f"(names {len(name_pool)} + posix {len(posix_pool)} + index {4 * len(rows)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
