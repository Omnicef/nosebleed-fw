// SPDX-License-Identifier: GPL-3.0-only
//
// T-10.1 — InfoCache: the weather sibling of DataCache (T-10.3 adds the
// ticker slots). Same seqlock protocol as cache.h — deliberately a copy of
// the hardware-proven one rather than a refactor of it: whole-value publish
// by the poll task, wait-free validated snapshot for render, bounded
// retries, false = keep the last strip. A fetch that never publishes leaves
// the slot exactly as it was: last-good degradation for free.
//
// Header-only template; the value type must carry `int64_t fetched_utc`.
// No Arduino includes — host-testable.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "weather.h"

namespace nb {
namespace data {

template <typename T> class SeqSlot {
  public:
    T* writable() {  // poll task only
        // Always land odd: if a previous fill was abandoned mid-way (a
        // fetch that died after writable(), which the "never publish a
        // failed fetch" rule otherwise forbids), a plain ++ would make the
        // NEXT refill read as stable. Single writer, so read-store is safe.
        const uint32_t v = seq_[alt_].load(std::memory_order_relaxed);
        seq_[alt_].store(v + (v & 1u) + 1u, std::memory_order_release);
        T* b = &bufs_[alt_];
        std::memset(b, 0, sizeof *b);
        return b;
    }

    void publish(int64_t now_utc) {  // poll task only
        const unsigned a = alt_;
        bufs_[a].fetched_utc = now_utc;
        seq_[a].fetch_add(1, std::memory_order_acq_rel);
        alt_ = a ^ 1;
        cur_.store(&bufs_[a], std::memory_order_release);
    }

    bool snapshot(T* dst) const {  // any task, lock-free
        for (int attempt = 0; attempt < 8; ++attempt) {
            const T* src = cur_.load(std::memory_order_acquire);
            if (!src) return false;
            const int i = (src == &bufs_[0]) ? 0 : 1;
            const uint32_t v = seq_[i].load(std::memory_order_acquire);
            if (v & 1u) continue;  // buffer mid-refill; cur will move next commit
            std::memcpy(dst, src, sizeof *dst);
            if (cur_.load(std::memory_order_acquire) == src &&
                seq_[i].load(std::memory_order_acquire) == v)
                return true;
        }
        return false;
    }

  private:
    T bufs_[2];
    std::atomic<const T*> cur_{nullptr};
    std::atomic<uint32_t> seq_[2]{};
    unsigned alt_ = 0;
};

class InfoCache {
  public:
    Weather* writable_weather() { return weather_.writable(); }
    void publish_weather(int64_t now_utc) { weather_.publish(now_utc); }
    bool snapshot_weather(Weather* dst) const { return weather_.snapshot(dst); }

  private:
    SeqSlot<Weather> weather_;
};

}  // namespace data
}  // namespace nb
