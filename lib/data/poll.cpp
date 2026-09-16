// SPDX-License-Identifier: GPL-3.0-only
#include "poll.h"

namespace nb {
namespace data {

void PollScheduler::reset(int64_t now) {
    for (int i = 0; i < kLeagues; ++i) due_[i] = now + i * kBootStaggerS;
}

int64_t PollScheduler::next_wake(int64_t now) const {
    int64_t best = -1;
    for (int i = 0; i < kLeagues; ++i) {
        if (!cfg_[i].enabled) continue;
        if (best < 0 || due_[i] < best) best = due_[i];
    }
    if (best < 0 || best < now) return now;  // nothing enabled / overdue: caller sleeps its floor
    return best;
}

}  // namespace data
}  // namespace nb
