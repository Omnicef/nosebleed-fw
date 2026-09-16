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

## Phase 3 logo pipeline — T-3.1 / T-3.2 (host-only, 2026-09-15)

`tools/build_logos.py` — ports the processing half of Marquee's
`logo_pipeline.py` (`_process`, verbatim) and serialises the PLAN §3 `logos.bin`
format (12 B header + sorted 20 B index + RGB565/1-bit-mask blobs). Pure host
Python; no device code, no fetching. **Stopped here** — T-3.3 (144-team ESPN
fetch), T-3.4 (mmap reader) and the rest await hardware/direction.

Validated against the 50 real cached logos in
`Marquee/marquee/assets/logos` (`tools/test_build_logos.py`, run directly):

- **T-3.1 parity** — runs Marquee's *real* `_process` (only `httpx`/`models`
  imports stubbed) and diffs it pixel-for-pixel against our port on a 7-logo
  sample incl. the collision pair. Identical.
- **T-3.2 round-trip** — build → read: 50 keys, every decoded blob == a fresh
  encode, offsets ascending and gap-free from `HEADER + 50*ENTRY_SIZE`. Atlas
  for the 50-logo corpus is 89,888 B (~1.8 KB/logo at height 32, matches §3).
- **Collision keying** — `eng.1` and `epl` are duplicate Premier League slugs
  sharing club abbreviations: 50 `league+abbr` keys vs 39 bare-abbr. A bare-abbr
  index would silently drop 11 logos; the `(league, abbr)` key keeps both
  `eng.1:liv` and `epl:liv` as distinct, bisect-resolvable rows. (Their cached
  artwork is identical, so the key defends the *index entry*, not the pixels.)

### Font fix hardened

The Phase 2 backslash fix was specific (`disp = "bs"`). Generalised to
alnum-only labels (space→`sp`, other punctuation→`?`) so the whole
backslash-in-comment class can't recur, not just glyph 92. Regenerated
`font_data.h`; `pio test -e native` still **0 px**.

### Generated-C escaping discipline (audit of every emitter)

Both generators — `build_fonts.py`, `gen_parity_golden.py` — emit **only `//`
line comments**, never `/* */` and never interpolate into C string literals.
The full inventory of interpolated content is:

| Emitter | Interpolated into generated C | Hazard |
|---|---|---|
| `build_fonts` glyph line | `{code}` (int), `{disp}` (label), numeric byte array | `disp` was the sole hazard (glyph 92 `\`); now alnum-only |
| `build_fonts` font/array decl | ints and `ident.upper()` (alnum idents) | none |
| `gen_parity_golden` grid line | numeric `0x%04X` only | none |
| `gen_parity_golden` banner | `{time_str}`, `{date_str}` in a `//` line — `%H:%M AM/PM`, `%a %m/%d` | none (never a backslash) |

**Rules for any future generator** (esp. logo/zone-name tables that embed
arbitrary text):
- A backslash at **end of a `//` comment line is a line-continuation** — it
  eats the next line. This is the bug that shipped a corrupted font table.
- Inside a C **string literal** `"…"`, escape `\` and `"`; a trailing `\` there
  too swallows the closing quote.
- Inside a **block comment** `/* … */`, guard against a `*/` sequence in data.
- Preferred and cheap: emit **numeric-only** (byte/int arrays, hex), so data
  can never carry C syntax. Do this whenever the value isn't human-meaningful.
- When a label must be human-readable, restrict to `[A-Za-z0-9_]`.

## Phase 3 — T-3.4/T-3.5/T-3.6 (host complete; T-3.4/T-3.7 device proof pending)

### T-3.4 — logos.bin reader (`lib/logos/logos.h` pure + `logos_esp.h` mmap)

`init_mmap()` (renamed from `init` — collides with Arduino's global
`init(void)`), binary-search `find()`, zero allocation, byte-wise decode.
**Finding: `struct "<8s4sIHHH"` is 22 B, not the 20 B every doc/summary
claimed** — the trailing rsvd u16 is part of the stride; at 20 the binary
search desyncs on every lookup past entry 0. Caught by the new host test
against the real atlas (synthetic-only tests passed at both strides —
real-artifact coverage earned its keep immediately).

Host proof now: `test_logos_parse_host` — synthetic 2-entry atlas
(hit/hit/miss/miss) + real `logos.bin` (count 144, FNV-1a of px+mask matches
host-computed values for mlb:BOS / nba:LAL / epl:LIV / nhl:BOS, nhl:BOS is
the cross-league duplicate-abbreviation case; `mlb:ZZZ` misses; soft-skips
when the gitignored artifact is absent so CI stays green).

Device proof pending (board disconnected mid-session): `logostest` env →
heap free + largest-block deltas 0/0 across init+lookups, hash PASS line,
then deliberate FRAME-phase trap. One flash + serial capture closes T-3.4
and T-3.7 together.

### T-3.5 — masked blit (`lib/render/logo.{h,cpp}`)

`blit_logo(canvas, x, y, LogoArt)` — alpha-test blit of RGB565 px through a
MSB-first 1-bit mask, canvas-clipped, degenerate refs return false. Parity
against Pillow's `card.paste(logo, box, mask=logo)` is exact (0/2048 px)
because atlas alpha is binary by construction (threshold 128 at build, so
Pillow's blend degenerates to the alpha test). Golden generated from the
**real atlas bytes** (`tools/gen_logo_golden.py` reads `logos.bin` via
`build_logos.read`): `epl:LIV` 17×32 — thin strokes punish any mask
off-by-one — pasted on non-black bg so masked-out pixels must show it.
Also asserts partly-offscreen blits clip without smearing.

### T-3.6 — fallback

`draw_abbr_fallback` — `abbr[:3]` in team colour, spleen-5x8 vertically
centred in the logo box (`y + (logo_h-8)/2`, `game_strip._paste_logo`),
returns painted width. `parse_hex565` handles ESPN `color` (leading `#`
optional, malformed rejected). Golden parity 0/2048 px; null-art blit is
safe (false, canvas untouched — never crash/blank, T-3.6 acceptance).

`pio test -e native` 8/8; purity guard clean; `esp32s3` build green.

### T-3.4/T-3.7 — device proof (board reconnected)

`logostest` firmware + `logos.bin` @ 0x810000 (`write_flash`, hash verified).
Serial capture, identical across three auto-reboot cycles:

```
init=1 count=144 logo_h=32
mmap internal free delta=-104 largestblk delta=0 (one-time mapping)
mlb:BOS OK w=23 h=32 hash=a917f7d6   nfl:KC OK w=32 h=21 hash=2b7038d9
nba:LAL OK w=32 h=20 hash=eacc63aa   nhl:VGK OK w=24 h=32 hash=1621bb28
epl:LIV OK w=17 h=32 hash=1b0feca6   nhl:BOS OK w=32 h=32 hash=6322ff39
mlb:ZZZ OK (miss)
lookups: internal free delta=0 largestblk delta=0 (must be 0/0)
RESULT: PASS
trap expected now: FRAME-phase lookup
Guru Meditation Error: Core 1 panic'ed — Debug exception reason: BREAK instr
```

- **T-3.4 accepted**: all six hashes match host values computed from the
  same file on the laptop; lookup heap delta **exactly 0/0**. The −104 B is
  `esp_partition_mmap`'s one-time page-table allocation, measured separately
  from the lookups (first draft mixed them and read −104 against a 0/0 gate —
  measurement split, not code, was wrong).
- **T-3.7 mechanism accepted**: FRAME-phase `lookup()` traps (`__builtin_trap`
  emits `break` on Xtensa → BREAK instr, EXCCAUSE 1) and the test firmware
  reboot-loops it deliberately. "Never fires during a 60 s scroll" is deferred
  to Phase 6/7 with the scroll engine; the guard itself is proven live.
- Core dump prints (`No core dump partition`) are noise — no coredump
   partition in our table; harmless.

## Phase 4 — T-4.1 config structs (2026-09-16)

`lib/config/config.h` + `config.cpp` — POD mirrors of the four Marquee
SQLModel tables plus magic/schema, one fixed-size NVS blob. Pi fields
dropped (hardware_mapping, gpio_slowdown, pwm_bits, pwm_dither_bits,
pixel_mapper_config, led_rgb_sequence); ESP32 panel-timing fields added
(lsb_msb_transition_bit, clkphase, latch_blanking, i2sspeed, double_buff).
`options_json` → typed `char league[26]` (only ever held `{"league": ...}`).

Sizes: HW 60, Widget 92 ×16, League 32 ×12, Favorite 85 ×16 →
**`sizeof(Config)` = 3292 B < 4096** (`static_assert`, T-4.1 accept — the
20 KB NVS partition has 4× headroom even with NVS page overhead).

`set_defaults` ports `seed_defaults()` exactly: boot_splash + clock +
scoreboards in `_SCOREBOARD_LEAGUES` order (mlb first, rest sorted — the
widget *id* uses that same order, `scoreboard_college-football` at idx 3);
LeagueConfig row per slug in dict order, only mlb enabled, 20/120 s.

Acceptance: `pio test -e native` **9/9** (`test_config_defaults` checks
sizes, widget order/ids/flags, league cadence, longest-id fit). The header's
own `static_assert` caught a miscount of `kLeagueSlugs` (8, not 9) while
writing it.


## T-4.2 — NVS store (2026-09-16)

`lib/config/store.{h,cpp}` — one namespace `"nb"`, one blob `"cfg"`.
`load()` = getBytesLength == sizeof(Config) && getBytes && validate, else
`set_defaults` + persist (absent/corrupt self-heals). API verified against
installed `Preferences.h` (getBytes returns 0 on absent OR len>maxLen —
size-mismatch corruption lands in the fallback path for free).

**Hardware proof (env:configtest, /dev/ttyACM0).** Boot A: absent→defaults,
brightness 55 round-trip, deliberate garbage (0xFF×200)→defaults no crash,
valid-prefix-100 B→defaults. Then self-`esp_restart`; boot B reads
persisted 55 back → PASS, blob reset, halts.

**Caught the real failure mode first:** five 3.3 KB `Config` locals blew
loopTask's 8 KB stack (CORRUPT HEAP, then stack-canary panic reboot loop).
Test instances are `static` now. Rule for Phase 8: **`Config` never goes on
a task stack** — the web `cfg` handlers and every task must pass pointers to
static/heap instances. The render task stack note in `AGENTS.md` is 8 KB too.
