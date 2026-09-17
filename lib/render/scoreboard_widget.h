// SPDX-License-Identifier: GPL-3.0-only
//
// T-6.9 — scoreboard CardProducer. One 64 px card per cached game for one
// league. Cards are ordered with priority (favourite) games first, matching
// Marquee's ScoreboardWidget.update(), and the rebuild key changes only when
// the ordered game set, scores, or situation fingerprint changes.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../data/cache.h"
#include "card_producer.h"
#include "game_card.h"
#include "local_time.h"

namespace nb {
namespace render {

class ScoreboardWidget : public CardProducer {
  public:
    static constexpr int kMaxPriorityIds = data::kMaxGamesPerLeague;

    ScoreboardWidget(const data::DataCache* cache, int league, const char* league_slug, const bool* enabled,
                     const LogoResolver& logos, LocalTimeFn local, void* local_ctx)
        : cache_(cache),
          league_(league),
          league_slug_(league_slug ? league_slug : ""),
          enabled_(enabled),
          logos_(logos),
          local_(local),
          local_ctx_(local_ctx),
          priority_count_(0) {
        std::snprintf(id_, sizeof id_, "scoreboard_%s", league_slug_);
        std::memset(snapshot_.games, 0, sizeof snapshot_.games);
    }

    const char* id() const override { return id_; }

    void set_priority_team_ids(const char* const* ids, int count) {
        if (ids == nullptr || count < 0) count = 0;
        if (count > kMaxPriorityIds) count = kMaxPriorityIds;
        for (int i = 0; i < count; ++i) priority_ids_[i] = ids[i];
        priority_count_ = count;
    }

    uint32_t cards_key(int64_t now_utc) const override {
        (void)now_utc;
        if (!visible_enabled() || !load()) return 0;

        int order[data::kMaxGamesPerLeague];
        const int count = order_games(order);

        uint32_t h = 2166136261u;
        hash_str(&h, id_);
        hash_char(&h, '|');
        hash_int(&h, count);
        for (int i = 0; i < count; ++i) {
            const data::Game& g = snapshot_.games[order[i]];
            hash_str(&h, g.id);
            hash_char(&h, ':');
            hash_int(&h, g.away_score);
            hash_char(&h, ':');
            hash_int(&h, g.home_score);
            hash_char(&h, ':');
            if (g.has_situation) {
                const data::Situation& s = g.situation;
                hash_int(&h, s.balls);
                hash_char(&h, '/');
                hash_int(&h, s.strikes);
                hash_char(&h, '/');
                hash_int(&h, s.outs);
                hash_char(&h, '/');
                hash_int(&h, s.on_first);
                hash_char(&h, '/');
                hash_int(&h, s.on_second);
                hash_char(&h, '/');
                hash_int(&h, s.on_third);
                hash_char(&h, '/');
                hash_int(&h, g.period);
                hash_char(&h, '/');
                hash_str(&h, g.status_display);
            }
            hash_char(&h, '|');
        }
        return h;
    }

    int cards(Canvas16* out, int max_cards, int64_t now_utc) const override {
        if (out == nullptr || max_cards <= 0 || !visible_enabled() || !load()) return 0;

        int order[data::kMaxGamesPerLeague];
        const int count = order_games(order);
        int written = 0;
        for (int i = 0; i < count && written < max_cards; ++i) {
            Canvas16& c = out[written];
            if (!c.valid()) return written;
            const data::Game& g = snapshot_.games[order[i]];
            if (g.status == data::kStatusPre) {
                render_game_card_pre(c, g, league_slug_, logos_, local_, local_ctx_);
            } else if (g.status == data::kStatusPost) {
                render_game_card_post(c, g, league_slug_, logos_, local_, local_ctx_, now_utc);
            } else {
                render_game_card_live(c, g, league_slug_, logos_, true);
            }
            ++written;
        }
        return written;
    }

    bool is_visible(int64_t now_utc) const override {
        (void)now_utc;
        return visible_enabled() && load() && snapshot_.count > 0;
    }

    bool has_live_priority_games() const override {
        if (!visible_enabled() || priority_count_ == 0 || !load()) return false;
        for (int i = 0; i < snapshot_.count; ++i) {
            const data::Game& g = snapshot_.games[i];
            if (g.status == data::kStatusIn && is_priority(g)) return true;
        }
        return false;
    }

  private:
    bool visible_enabled() const { return enabled_ != nullptr && *enabled_; }

    bool load() const {
        if (cache_ == nullptr) return loaded_;
        if (cache_->snapshot(league_, &snapshot_)) {
            loaded_ = true;
            return true;
        }
        return loaded_;
    }

    bool is_priority(const data::Game& g) const {
        for (int i = 0; i < priority_count_; ++i) {
            const char* id = priority_ids_[i];
            if (id == nullptr || id[0] == '\0') continue;
            if (std::strcmp(g.away.id, id) == 0 || std::strcmp(g.home.id, id) == 0) return true;
        }
        return false;
    }

    int order_games(int* order) const {
        int n = 0;
        for (int i = 0; i < snapshot_.count; ++i) {
            if (is_priority(snapshot_.games[i])) order[n++] = i;
        }
        for (int i = 0; i < snapshot_.count; ++i) {
            if (!is_priority(snapshot_.games[i])) order[n++] = i;
        }
        return n;
    }

    static void hash_char(uint32_t* h, char c) {
        *h ^= static_cast<uint8_t>(c);
        *h *= 16777619u;
    }

    static void hash_str(uint32_t* h, const char* s) {
        if (s != nullptr) {
            while (*s != '\0') hash_char(h, *s++);
        }
        hash_char(h, '\0');
    }

    static void hash_int(uint32_t* h, int32_t v) {
        uint32_t u = static_cast<uint32_t>(v) ^ 0x80000000u;
        for (int i = 0; i < 4; ++i) {
            *h ^= (u & 0xffu);
            *h *= 16777619u;
            u >>= 8;
        }
    }

    mutable data::GameList snapshot_{};
    mutable bool loaded_ = false;
    const data::DataCache* cache_;
    int league_;
    const char* league_slug_;
    const bool* enabled_;
    const LogoResolver logos_;
    LocalTimeFn local_;
    void* local_ctx_;
    char id_[48];
    const char* priority_ids_[kMaxPriorityIds]{};
    int priority_count_;
};

}  // namespace render
}  // namespace nb
