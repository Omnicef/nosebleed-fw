# WORKLOG.md — nosebleed-fw

Running log for unattended work. One entry per task: what happened, the
acceptance evidence, anything surprising. Newest at the bottom.

Phase 0 and Phase 1 are committed and the gate is CLOSED (GO) on hardware.
This session's scope: **T-2.1 … T-2.7** (stop at the T-2.7 parity gate). Only
if T-2.7 passes with a clean zero-pixel diff may it continue into **T-3.1 …
T-3.3** (host-only logo pipeline). No hardware, no flashing.

---

## Session start (2026-09-15)

- Instructed to first "commit and push the outstanding PLAN.md edit". Checked:
  `git status` clean, `origin/main..HEAD` empty, no stash. The PLAN.md edit is
  already committed and pushed as **3d3d049** ("Correct T-1.2 partition overlap
  …"). Nothing outstanding. Proceeding.

## Phase 2 render core — T-2.1 … T-2.7 (2026-09-15)

Built the framework-agnostic render layer and the host parity harness, all under
`env:native`. `pio test -e native` → **5/5 passed**.

- **T-2.1** `canvas.{h,cpp}` — `Canvas16` RGB565, bounds-checked, injectable
  allocator. Allocates/frees a 2520×32 strip cleanly.
- **T-2.2** `primitives.{h,cpp}` — point/line/rect/fill_rect/ellipse/fill_ellipse/
  polygon (scanline fill, portable `round_down`, no `__builtin_floorf`).
- **T-2.3/2.4/2.5** `font.{h,cpp}` + `tools/build_fonts.py` → `font_data.h` —
  BDF → fixed-width glyph tables (spleen-5x8/6x12, tom-thumb); `draw_text`,
  `text_width = len*advance`, `draw_text_outlined` (8-halo).
- **T-2.6** `test/test_native/` Unity harness + stored-deflate PNG writer →
  PNGs to `test/out/`.
- **T-2.7** `tools/gen_parity_golden.py` renders the clock card with Pillow
  (fixed 2025-12-25 15:30), quantises to RGB565, emits `golden_clock.h`. The
  firmware render is compared elementwise.

### Acceptance evidence

- `bash tools/check_render_purity.sh` → `purity: OK`.
- `pio test -e native` → `test_clock_parity [PASSED]`, **0 px differ** of 64×32.
  Time row (spleen-6x12) and date row (spleen-5x8, "Thu 12/25") both match the
  Pillow golden exactly.

### The bug that the parity gate caught

Pixel parity initially failed by 23 px, all in the spleen-5x8 date row. Root
cause was **not** the blitter — it was `build_fonts.py`. The comment for glyph
92 ended in a backslash (`// 92 \`); a trailing backslash in a `//` comment is a
C **line-continuation**, so the preprocessor spliced the following `]` glyph
initializer into the comment. Every glyph after code 92 shifted up by one — so
`draw_text` rendered 'h' with 'i's bitmap, etc. Fixed by not emitting a bare
backslash in the comment (`disp = "bs"`). This would have shipped a silently
corrupted font to the real firmware; the T-2.7 parity gate earned its keep.

