// SPDX-License-Identifier: GPL-3.0-only
#include "date_window.h"

#include <cstdio>

namespace nb {
namespace data {
namespace {

long ymd(const struct tm& t) {
    return (t.tm_year + 1900L) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

}  // namespace

bool local_day(char* buf, size_t cap, time_t now) {
    struct tm t;
    if (!localtime_r(&now, &t) || cap < 9) return false;
    const int n = snprintf(buf, cap, "%04ld%02d%02d", t.tm_year + 1900L, t.tm_mon + 1,
                           t.tm_mday);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool local_yesterday(char* buf, size_t cap, time_t now) {
    struct tm t;
    if (!localtime_r(&now, &t) || cap < 9) return false;
    t.tm_mday -= 1;  // mktime normalises across month and DST
    t.tm_isdst = -1;
    const time_t y = mktime(&t);
    struct tm yt;
    if (!localtime_r(&y, &yt)) return false;
    const int n = snprintf(buf, cap, "%04ld%02d%02d", yt.tm_year + 1900L, yt.tm_mon + 1,
                           yt.tm_mday);
    return n > 0 && static_cast<size_t>(n) < cap;
}

int filter_yesterday_today(GameList* list, time_t now) {
    struct tm t;
    if (!localtime_r(&now, &t)) return list->count;  // no TZ yet — keep everything
    const long today = ymd(t);
    t.tm_mday -= 1;  // calendar yesterday — mktime normalizes across month and DST
    t.tm_isdst = -1;
    const time_t yesterday_time = mktime(&t);
    struct tm yt;
    if (!localtime_r(&yesterday_time, &yt)) return list->count;
    const long yesterday = ymd(yt);

    int w = 0;
    for (int i = 0; i < list->count; ++i) {
        struct tm gt;
        const time_t st = static_cast<time_t>(list->games[i].start_utc);
        const bool keep = list->games[i].start_utc == 0 ||
                          (localtime_r(&st, &gt) && (ymd(gt) == today || ymd(gt) == yesterday));
        if (keep) {
            if (w != i) list->games[w] = list->games[i];
            ++w;
        }
    }
    list->count = w;
    return w;
}

}  // namespace data
}  // namespace nb
