// SPDX-License-Identifier: GPL-3.0-only
//
// T-5.6 — the REQUIRED date narrowing (PLAN §2 mitigation #1). ESPN's
// default scoreboard window is what put the fetch over the 6 s budget
// (6,081 ms unmitigated, SPIKE_RESULTS). Request one local day; filter
// like the Python to yesterday+today. Uses only <ctime>; the process TZ
// comes from setenv("TZ")+tzset() (T-4.6). time_t is 32-bit on the S3's
// newlib — fine until 2038; revisit then.

#pragma once

#include <ctime>

#include "game.h"

namespace nb {
namespace data {

// "YYYYMMDD" local calendar day of *now* (ESPN ?dates= param form).
// False on bad input/cap.
bool local_day(char* buf, size_t cap, time_t now);

// Marquee _filter_by_date: keep games whose LOCAL start date is yesterday
// or today; start_utc == 0 (unparseable date — the Python stamps those
// with now) is kept. Compacts list in place, returns the new count.
int filter_yesterday_today(GameList* list, time_t now);

}  // namespace data
}  // namespace nb
