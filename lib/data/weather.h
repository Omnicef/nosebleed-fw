// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.1 — weather data struct. Open-Meteo current + today's high/low,
// already rounded to whole degrees and with the WMO condition code
// resolved to text by the decoder, so the widget draws strings and nothing
// else. POD with fixed char[] like game.h — copyable with one memcpy,
// published through the seqlock InfoCache. No Arduino/ESP includes.

#pragma once

#include <cstdint>

#include "game.h"  // kNoInt

namespace nb {
namespace data {

struct Weather {
    int64_t fetched_utc;     // publish stamp; 0 = never
    char cond[20];           // "Partly cloudy" — WMO weather_code text, "" unknown
    int16_t temp, high, low; // whole degrees in `unit`; kNoInt when absent
    char unit;               // 'C' or 'F' (from current_units.temperature_2m)
};

}  // namespace data
}  // namespace nb
