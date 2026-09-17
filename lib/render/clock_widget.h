// SPDX-License-Identifier: GPL-3.0-only
//
// T-6.2 — clock/date card. Layout constants copied from Marquee
// ClockWidget.cards(): time at y=2 in spleen-6x12, date at y=22 in
// spleen-5x8, centred within CARD_W. The device supplies local time through
// an injected function so the render layer stays pure and host tests can fake
// a timestamp/timezone.

#pragma once

#include <cstdio>
#include <cstring>

#include "card_producer.h"
#include "font.h"
#include "font_data.h"
#include "local_time.h"

namespace nb {
namespace render {

class ClockWidget : public CardProducer {
  public:
    ClockWidget(LocalTimeFn local, void* ctx, bool clock_24h)
        : local_(local), ctx_(ctx), clock_24h_(clock_24h) {}

    const char* id() const override { return "clock"; }

    uint32_t cards_key(int64_t now_utc) const override {
        LocalTime now{};
        if (!local_(ctx_, now_utc, now)) return 0;
        // Python key is "%H:%M:12h" / "%H:%M:24h": one bucket per minute,
        // plus the format so a settings flip rebuilds immediately.
        return static_cast<uint32_t>(now.hour * 60 + now.minute) * 2u +
               static_cast<uint32_t>(clock_24h_);
    }

    int cards(Canvas16* out, int max_cards, int64_t now_utc) const override {
        if (max_cards < 1 || !out[0].valid()) return 0;
        LocalTime now{};
        if (!local_(ctx_, now_utc, now)) return 0;

        char time_str[16], date_str[16];
        if (clock_24h_) {
            std::snprintf(time_str, sizeof(time_str), "%02d:%02d", now.hour, now.minute);
        } else {
            const int h12 = (now.hour % 12) == 0 ? 12 : (now.hour % 12);
            std::snprintf(time_str, sizeof(time_str), "%d:%02d %s", h12, now.minute,
                          now.hour < 12 ? "AM" : "PM");
        }
        static const char* kWd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        std::snprintf(date_str, sizeof(date_str), "%s %02d/%02d",
                      kWd[now.wday < 0 || now.wday > 6 ? 0 : now.wday],
                      now.month, now.day);

        Canvas16& c = out[0];
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) c.set(x, y, 0);

        const int time_len = static_cast<int>(std::strlen(time_str));
        const int date_len = static_cast<int>(std::strlen(date_str));
        int tx = (CARD_W - text_width(FONT_SPLEEN_6X12, time_len)) / 2;
        if (tx < 0) tx = 0;
        int dx = (CARD_W - text_width(FONT_SPLEEN_5X8, date_len)) / 2;
        if (dx < 0) dx = 0;

        draw_text(c, FONT_SPLEEN_6X12, tx, 2, time_str, rgb565(255, 255, 255));
        draw_text(c, FONT_SPLEEN_5X8, dx, 22, date_str, rgb565(140, 140, 140));
        return 1;
    }

  private:
    LocalTimeFn local_;
    void* ctx_;
    bool clock_24h_;
};

}  // namespace render
}  // namespace nb
