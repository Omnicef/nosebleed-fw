// SPDX-License-Identifier: GPL-3.0-only
// T-10.1 — pure half of the weather client: filter, decode, WMO text,
// URL builder. Host-compiled by env:native; the device links the same code.

#include "weather_json.h"

#include <cmath>
#include <cstring>

#include "game.h"  // copy_str

namespace nb {
namespace data {

void build_weather_filter(JsonDocument& filter) {
    filter["current_units"]["temperature_2m"] = true;
    JsonObject cur = filter["current"].to<JsonObject>();
    cur["temperature_2m"] = true;
    cur["weather_code"] = true;
    filter["daily"]["temperature_2m_max"][0] = true;  // [0] = element template
    filter["daily"]["temperature_2m_min"][0] = true;  // (forecast_days=1)
}

namespace {

// WMO 4677 interpretation codes, as Open-Meteo reports weather_code.
struct Wmo {
    int code;
    const char* text;
};
constexpr Wmo kWmo[] = {
    {0, "Clear sky"},      {1, "Mainly clear"},   {2, "Partly cloudy"},
    {3, "Overcast"},       {45, "Fog"},           {48, "Freezing fog"},
    {51, "Light drizzle"}, {53, "Drizzle"},       {55, "Heavy drizzle"},
    {56, "Freezing drizzle"}, {57, "Freezing drizzle"},
    {61, "Light rain"},    {63, "Rain"},          {65, "Heavy rain"},
    {66, "Freezing rain"}, {67, "Freezing rain"}, {71, "Light snow"},
    {73, "Snow"},          {75, "Heavy snow"},    {77, "Snow grains"},
    {80, "Light showers"}, {81, "Showers"},       {82, "Heavy showers"},
    {85, "Snow showers"},  {86, "Snow showers"},  {95, "Thunderstorm"},
    {96, "Thunderstorm"},  {99, "Severe thunder."},
};

const char* wmo_text(int code) {
    for (const Wmo& w : kWmo)
        if (w.code == code) return w.text;
    return "";
}

// Whole-degree read: ArduinoJson stores 72.1 as double and 72 as long.
bool read_deg(JsonVariantConst v, int16_t* out) {
    if (v.is<float>() || v.is<long>()) {
        *out = static_cast<int16_t>(std::lround(v.as<float>()));
        return true;
    }
    return false;
}

bool plain_decimal(const char* s) {
    bool digit = false;
    size_t n = 0;
    for (; s[n] != '\0'; ++n) {
        if (n >= 15) return false;
        const char c = s[n];
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '-' && c != '+' && c != '.') return false;
    }
    return digit;
}

}  // namespace

bool to_weather(JsonVariantConst root, Weather& out) {
    const JsonObjectConst cur = root["current"].as<JsonObjectConst>();
    const JsonVariantConst temp = cur["temperature_2m"];
    Weather w = {};
    if (!read_deg(temp, &w.temp)) return false;  // no current temp = no card

    const JsonVariantConst unit = root["current_units"]["temperature_2m"];
    const char* u = unit.is<const char*>() ? unit.as<const char*>() : nullptr;
    const size_t ul = unit.is<const char*>() ? std::strlen(u) : 0;
    w.unit = (ul > 0 && (u[ul - 1] == 'C' || u[ul - 1] == 'F')) ? u[ul - 1] : 'C';

    const JsonVariantConst code = cur["weather_code"];
    if (code.is<int32_t>()) copy_str(w.cond, sizeof w.cond, wmo_text(code.as<int32_t>()));

    const JsonArrayConst hi = root["daily"]["temperature_2m_max"].as<JsonArrayConst>();
    const JsonArrayConst lo = root["daily"]["temperature_2m_min"].as<JsonArrayConst>();
    if (hi.size() == 0 || !read_deg(hi[0], &w.high)) w.high = kNoInt;
    if (lo.size() == 0 || !read_deg(lo[0], &w.low)) w.low = kNoInt;

    out = w;
    return true;
}

bool weather_url(char* url, size_t cap, const char* lat, const char* lon,
                 bool imperial) {
    if (lat == nullptr || lon == nullptr || !plain_decimal(lat) || !plain_decimal(lon))
        return false;
    const int n = snprintf(url, cap,
                           "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                           "&current=temperature_2m,weather_code"
                           "&daily=temperature_2m_max,temperature_2m_min"
                           "&forecast_days=1&timezone=auto&temperature_unit=%s",
                           lat, lon, imperial ? "fahrenheit" : "celsius");
    return n > 0 && (size_t)n < cap;
}

}  // namespace data
}  // namespace nb
