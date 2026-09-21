// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.3 — ticker decoding, framework-free (ArduinoJson + ticker.h only),
// same split as weather_json: host tests run the exact device code.
//
// Shapes (captured 2026-09-20):
//  * CoinGecko /simple/price — LIVE capture: {"bitcoin":{"usd":81268,
//    "usd_24h_change":1.147…}}. Tiny prices arrive as "4.0e-06" — JSON
//    number, ArduinoJson reads it as double. Depth 3.
//  * GNews /api/v4/top-headlines — vendor docs (live shape needs a key):
//    {"totalArticles":N,"articles":[{"title":…,"source":{"name":…}}]}.
//    Depth 4. (LIVE 2026-09-20: /everything 404s — top-headlines is the
//    real path; a bogus key gets {"errors":[…]} with HTTP 400.)
//  * Finnhub /quote — vendor docs: {"c":cur,"d":chg,"dp":"chg%","pc":prev,
//    "t":ts}, flat. Depth 2.

#pragma once

#include <cstddef>
#include <utility>

#include <ArduinoJson.h>

#include "ticker.h"

namespace nb {
namespace data {

// One filtered-parse entry point for all three payloads (same
// NestingLimit(20) discipline project-wide).
template <typename TStream>
DeserializationError parse_ticker(TStream&& in, JsonDocument& filter,
                                  JsonDocument& doc) {
    return deserializeJson(doc, std::forward<TStream>(in),
                           DeserializationOption::Filter(filter),
                           DeserializationOption::NestingLimit(20));
}

void build_coin_filter(JsonDocument& filter, const char* ids);
bool to_coins(JsonVariantConst root, const char* ids, TickerList& out);

void build_news_filter(JsonDocument& filter);
bool to_news(JsonVariantConst root, TickerList& out);

void build_stock_filter(JsonDocument& filter);
bool to_stock(JsonVariantConst root, const char* symbol, TickerItem& item);

// URL builders, each an injection guard like weather_url: anything that
// rides in the query string is validated first or the builder returns
// false. ids: comma-separated [a-z0-9-] tokens, ≤10, ≤48 chars total.
// symbols: comma-separated [A-Za-z0-9._-], ≤4, ≤48 total. category must be
// one of GNews' fixed category names; country two lowercase letters. Keys
// must pass api_keys_valid before any builder is called.
bool coin_url(char* url, size_t cap, const char* ids);
bool news_url(char* url, size_t cap, const char* category, const char* country,
              const char* key);
bool stock_url(char* url, size_t cap, const char* symbol, const char* key);

#ifdef ARDUINO
// Transports (tls_take → http_get → ReadBufferingStream → filtered parse),
// mirroring weather_fetch. False on transport/parse failure or when nothing
// parsed — callers keep last-good. stocks_fetch walks the CSV (≤4 requests,
// sequential — single TLS session) and succeeds if any symbol decoded.
bool coins_fetch(const char* url, const char* ids, TickerList& out);
bool news_fetch(const char* url, TickerList& out);
bool stocks_fetch(const char* symbols, const char* key, TickerList& out);
#endif

}  // namespace data
}  // namespace nb
