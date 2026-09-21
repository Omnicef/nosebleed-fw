// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.3 — ticker decoders + URL guards. Pure code, shared byte-for-byte
// with the host tests. See ticker_json.h for the payload provenance.

#include "ticker_json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "game.h"  // copy_str

#ifdef ARDUINO
#include <StreamUtils.h>
#include "espn.h"  // tls_take / tls_release / make_psram_doc
#include "http.h"
#endif

namespace nb {
namespace data {

namespace {

// Comma-separated token walker. Returns the token, advances p; "" at end.
const char* next_tok(const char*& p, char* tok, size_t cap) {
    size_t i = 0;
    while (*p != '\0' && *p != ',' && i + 1 < cap) tok[i++] = *p++;
    tok[i] = '\0';
    if (*p == ',') ++p;
    return tok;
}

bool in_set(char c, const char* set) {
    if (set == nullptr) return false;
    for (const char* p = set; *p != '\0'; ++p)
        if (c == *p) return true;
    return false;
}

// "81268" -> "81,268" into dst (cap >= 13 covers the 9-digit cap).
void fmt_price(char* dst, size_t cap, double v) {
    if (v >= 1000.0) {
        char raw[12];  // 11 digits: leaves room for the caller's '$' prefix
        snprintf(raw, sizeof raw, "%.0f", v);
        const size_t n = strlen(raw), commas = n > 1 ? (n - 1) / 3 : 0;
        if (n + commas + 1 > cap) {  // tiny cap: plain digits still display
            snprintf(dst, cap, "%s", raw);
            return;
        }
        size_t j = n + commas;
        dst[j] = '\0';
        for (size_t i = n; i > 0;) {  // fill from the right
            dst[--j] = raw[--i];
            if (i > 0 && (n - i) % 3 == 0) dst[--j] = ',';
        }
    } else if (v >= 0.01) {
        const long cents = lround(v * 100.0);
        snprintf(dst, cap, "%ld.%02ld", cents / 100, labs(cents) % 100);
    } else {
        snprintf(dst, cap, "%g", v);  // micro-caps: "4e-06"
    }
}

constexpr int kLabelMax = 12;  // spleen-5x8: 12 glyphs = 60 px

// Greedy two-line word wrap, 12 glyphs per line, overflow dropped.
void wrap2(const char* s, char* a, size_t acol, char* b, size_t bcol) {
    while (*s == ' ') ++s;
    const size_t n = strlen(s);
    if (n <= kLabelMax) {
        copy_str(a, acol, s);
        b[0] = '\0';
        return;
    }
    size_t cut = kLabelMax;
    for (int i = kLabelMax; i > 0; --i)
        if (s[i] == ' ') {  // break on the LAST word boundary that fits
            cut = static_cast<size_t>(i);
            break;
        }
    char tmp[kLabelMax + 1];
    memcpy(tmp, s, cut);
    tmp[cut] = '\0';
    copy_str(a, acol, tmp);
    const char* rest = s + cut;
    while (*rest == ' ') ++rest;
    copy_str(b, bcol, rest);  // 12-glyph truncation is copy_str's job
}

bool key_ok(const char* k) {  // same class api_keys_valid gates on
    size_t n = 0;
    for (const char* p = k; *p != '\0'; ++p, ++n) {
        const char c = *p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '.' && c != '_' && c != '-')
            return false;
    }
    return n >= 8 && n < 64;
}

}  // namespace

// ---------------------------------------------------------------- CoinGecko

void build_coin_filter(JsonDocument& filter, const char* ids) {
    const char* p = ids;
    char tok[16];
    while (*p != '\0' && next_tok(p, tok, sizeof tok)[0] != '\0') {
        filter[tok]["usd"] = true;
        filter[tok]["usd_24h_change"] = true;
    }
}

bool to_coins(JsonVariantConst root, const char* ids, TickerList& out) {
    out.count = 0;
    const char* p = ids;
    char tok[16];
    while (*p != '\0' && next_tok(p, tok, sizeof tok)[0] != '\0' &&
           out.count < kTickerMaxItems) {
        const JsonVariantConst usd = root[tok]["usd"];
        if (!usd.is<double>() && !usd.is<int64_t>() && !usd.is<int32_t>()) continue;
        TickerItem& it = out.items[out.count];
        copy_str(it.label, sizeof it.label, tok);
        it.l1[0] = '$';  // crypto is USD-quoted
        fmt_price(it.l1 + 1, sizeof it.l1 - 1, usd.as<double>());
        const JsonVariantConst d = root[tok]["usd_24h_change"];
        if (d.is<double>()) {
            snprintf(it.l2, sizeof it.l2, "%+.1f%%", d.as<double>());
        } else {
            it.l2[0] = '\0';
        }
        ++out.count;
    }
    return out.count > 0;
}

bool coin_url(char* url, size_t cap, const char* ids) {
    if (ids == nullptr) return false;
    const size_t total = strlen(ids);
    if (total == 0 || total > 48) return false;
    int toks = 0;
    for (const char* p = ids; *p != '\0'; ++p) {
        if (*p == ',') {
            if (++toks >= 10 || p[1] == '\0' || p[1] == ',') return false;  // empty tail/adjacent
        } else if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '-') {
            return false;
        }
    }
    const int n = snprintf(url, cap,
                           "https://api.coingecko.com/api/v3/simple/price?ids=%s"
                           "&vs_currencies=usd&include_24hr_change=true",
                           ids);
    return n > 0 && static_cast<size_t>(n) < cap;
}

// -------------------------------------------------------------------- GNews

void build_news_filter(JsonDocument& filter) {
    filter["articles"][0]["title"] = true;
    filter["articles"][0]["source"]["name"] = true;
}

bool to_news(JsonVariantConst root, TickerList& out) {
    out.count = 0;
    for (JsonVariantConst a : root["articles"].as<JsonArrayConst>()) {
        const JsonVariantConst title = a["title"];
        if (!title.is<const char*>()) continue;
        TickerItem& it = out.items[out.count];
        const JsonVariantConst src = a["source"]["name"];
        if (src.is<const char*>()) {
            copy_str(it.label, sizeof it.label, src.as<const char*>());
        } else {
            it.label[0] = '\0';
        }
        wrap2(title.as<const char*>(), it.l1, sizeof it.l1, it.l2, sizeof it.l2);
        if (it.l1[0] == '\0' && it.label[0] == '\0') continue;
        if (++out.count >= kTickerMaxItems) break;
    }
    return out.count > 0;
}

bool news_url(char* url, size_t cap, const char* category, const char* country,
              const char* key) {
    static const char* const kCats[] = {
        "general", "business", "entertainment", "health", "science", "sports", "technology"};
    if (category == nullptr || country == nullptr || key == nullptr) return false;
    bool known = false;
    for (const char* c : kCats)
        if (strcmp(category, c) == 0) known = true;
    if (!known) return false;
    if (!((country[0] >= 'a' && country[0] <= 'z') && (country[1] >= 'a' && country[1] <= 'z') &&
          country[2] == '\0'))
        return false;
    if (!key_ok(key)) return false;
    const int n = snprintf(url, cap,
                           "https://gnews.io/api/v4/top-headlines?category=%s&country=%s"
                           "&lang=en&max=%d&apiKey=%s",
                           category, country, kTickerMaxItems, key);
    return n > 0 && static_cast<size_t>(n) < cap;
}

// ------------------------------------------------------------------ Finnhub

void build_stock_filter(JsonDocument& filter) {
    filter["c"] = true;
    filter["d"] = true;
    filter["dp"] = true;
    filter["pc"] = true;
}

bool to_stock(JsonVariantConst root, const char* symbol, TickerItem& item) {
    const JsonVariantConst c = root["c"];
    // Finnhub answers {"c":0,...} for symbols it does not know.
    if ((!c.is<double>() && !c.is<int64_t>() && !c.is<int32_t>()) || c.as<double>() <= 0.0) {
        item.label[0] = '\0';
        return false;
    }
    copy_str(item.label, sizeof item.label, symbol);
    fmt_price(item.l1, sizeof item.l1, c.as<double>());
    const JsonVariantConst dp = root["dp"];
    if (dp.is<const char*>() && dp.as<const char*>()[0] != '\0') {  // pre-signed, e.g. "-0.25"
        snprintf(item.l2, sizeof item.l2, "%s%%", dp.as<const char*>());
    } else {
        const JsonVariantConst d = root["d"], pc = root["pc"];
        if (d.is<double>() && pc.is<double>() && pc.as<double>() > 0.0) {
            snprintf(item.l2, sizeof item.l2, "%+.2f%%",
                     100.0 * d.as<double>() / pc.as<double>());
        } else {
            item.l2[0] = '\0';
        }
    }
    return true;
}

bool stock_url(char* url, size_t cap, const char* symbol, const char* key) {
    if (symbol == nullptr || key == nullptr) return false;
    size_t n = 0;
    for (const char* p = symbol; *p != '\0'; ++p, ++n) {
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    if (n == 0 || n > 12 || !key_ok(key)) return false;
    const int w = snprintf(url, cap, "https://finnhub.io/api/v1/quote?symbol=%s&token=%s",
                           symbol, key);
    return w > 0 && static_cast<size_t>(w) < cap;
}

#ifdef ARDUINO
namespace {

// One buffered filtered GET inside the shared TLS mutex, exactly the
// weather_fetch chain.
bool fetch_filtered(const char* url, JsonDocument& filter, JsonDocument& doc) {
    if (!tls_take(15000)) return false;
    DeserializationError err;
    const bool ok = http_get(url, [&](HttpStream& s) {
        ReadBufferingStream rs(s, 512);
        err = parse_ticker(rs, filter, doc);
        return err == DeserializationError::Ok && !doc.overflowed();
    });
    tls_release();
    return ok;
}

}  // namespace

bool coins_fetch(const char* url, const char* ids, TickerList& out) {
    JsonDocument filter;
    build_coin_filter(filter, ids);
    JsonDocument doc = make_psram_doc();
    return fetch_filtered(url, filter, doc) && to_coins(doc, ids, out);
}

bool news_fetch(const char* url, TickerList& out) {
    JsonDocument filter;
    build_news_filter(filter);
    JsonDocument doc = make_psram_doc();
    return fetch_filtered(url, filter, doc) && to_news(doc, out);
}

bool stocks_fetch(const char* symbols, const char* key, TickerList& out) {
    out.count = 0;
    const char* p = symbols;
    char tok[16];
    while (*p != '\0' && next_tok(p, tok, sizeof tok)[0] != '\0' && out.count < 4) {
        char url[256];
        if (!stock_url(url, sizeof url, tok, key)) continue;
        JsonDocument filter;
        build_stock_filter(filter);
        JsonDocument doc = make_psram_doc();
        if (fetch_filtered(url, filter, doc)) {
            if (to_stock(doc, tok, out.items[out.count])) ++out.count;
        }
        // per-symbol failure just skips — one bad ticker never eats the rest
    }
    return out.count > 0;
}
#endif  // ARDUINO

}  // namespace data
}  // namespace nb
