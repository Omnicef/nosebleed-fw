// SPDX-License-Identifier: GPL-3.0-only

#include "scroll.h"

namespace nb {
namespace render {

void blit_window(const Canvas16& strip, Canvas16& dst, int w0) {
    for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) dst.set(x, y, strip.get(x + w0, y));
}

int page_window_x(const Strip& s, PageState& st, int64_t now_utc, int dwell_s) {
    if (s.page_count <= 0) return 0;
    if (st.page < 0 || st.page >= s.page_count)
        st.page = 0;  // a rebuild shrank the page list
    const int dwell = dwell_s > 0 ? dwell_s : 5;
    if (st.next_change_utc == 0) {
        st.next_change_utc = now_utc + dwell;  // first page gets its dwell too
    } else if (now_utc >= st.next_change_utc) {
        st.page = (st.page + 1) % s.page_count;
        st.next_change_utc = now_utc + dwell;
    }
    return s.page_x[st.page];
}

}  // namespace render
}  // namespace nb
