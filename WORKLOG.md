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

## T-4.3 — change notification (2026-09-16)

`bind_render_task()` + `save()` fires `xTaskNotifyGive` on success — the
Python's `settings_event.set()` analogue, one writer (web) one listener
(render). Render task now blocks in `ulTaskNotifyTake(pdTRUE, 5 s)`:
wake → reload `g_cfg` (file-scope; the T-4.2 stack rule holds even here),
timeout → heartbeat.

**Hardware proof (boot B of env:configtest):** after the T-4.2 persisted
check, a render-shaped listener task binds, test sets brightness=40 and
saves → listener woke from the notification and read 40 back → PASS, no
restart. Normal boot now prints the loaded (or seeded) config.

## T-4.4 — structural-change detection (2026-09-16)

`hw_structural_changed(old, now)` — pure, two structs in, one bool out, so
"field present in payload" *cannot* flag anything; only value diffs can
(the Marquee fix, enforced by the signature itself). Field set: Marquee's
minus the Pi five, plus their ESP32 equivalents (lsb_msb_transition_bit,
clkphase, latch_blanking, i2sspeed, double_buff). `reserved` deliberately
excluded. Native 11/11; caught two self-inflicted traps on the way (Config
vs Config.hw at the call site; default `lsb_msb_transition_bit` is already
1 — the "change" test must use 0).

## T-4.5 — IANA→POSIX timezone table (2026-09-16)

`tools/build_tzmap.py` samples 2026 tzdata transitions per zone and emits
`lib/config/tzmap.h`: 484 zones, 10,871 B (< 20 KB budget), deduped name +
posix pools, binary-search lookup, static_assert on size. Header committed
so device and CI agree regardless of builder's tzdata.
Accept cases: `America/New_York` → `EST5EDT,M3.2.0,M11.1.0` **exact**;
`Australia/Sydney` → `AEST-10AEDT,M10.1.0,M4.1.0/3` (southern hemisphere).
Findings: this box's tzdata makes **America/Vancouver permanently MST(-7)
from Nov 2026** (BC adopted permanent DST) — one transition, never back, so
the tool detects "settled to fixed offset" zones and emits `MST7` instead of
an impossible M-rule. Numeric abbreviations ("+03", Chile's "-03") aren't
valid POSIX names → `XXX` (offset preserved; the clock only needs the
offset). Rule time is `t + prev_offset` — using `t-1` produced a system-
atic `/1:59` off-by-one. Native 12/12.

## T-4.6 — timezone resolution (2026-09-16)

`lib/config/timezone.{h,cpp}`: `apply_timezone(iana)` → `tz_lookup` →
`setenv("TZ")/tzset()`. Empty → UTC0 + `kUtcEmpty` (Phase 6 clock draws the
indicator); unknown name → UTC0 + `kUtcUnknown`, never a crash. Host and
device share the code path; the device just has no tzdata, so the POSIX
string IS the zone — which the host test verifies by construction.
Ported Marquee test_timezone.py: section A (helper semantics) and the
NY-vs-UTC local-date-of-now discriminator from section C, plus NY spring/
fall and Sydney southern-hemisphere DST boundaries at second resolution.
Sections B/D are ESPN date-window and scoreboard-visibility tests — Phase 5
and 6 territory, port there. Three self-inflicted wrong expectations caught
by Python cross-check before trusting a red test (06:59 UTC is Mar 8 not 7;
Sydney's Oct 4 02:00 local is Oct 3 UTC; the discriminator is now's local
date, not game X's). Native 13/13.

## T-4.7 — phase done (2026-09-16)

Wired `apply_timezone` into boot (after config load) and into the
task_render live-reload path — a timezone edit now applies without restart,
same notification as brightness. Normal esp32s3 build re-flashed (board back
off the configtest image); boot serial shows
`config: loaded (brightness=40)` / `timezone: '' -> UTC fallback` and all
four heartbeats. Native 13/13. Accept criteria all met in T-4.1..T-4.6;
nothing deferred to later phases. Phase 4 complete.

## Phase 2 (late) — T-2.8 panel wrapper + both hardware faults (2026-09-16)

`lib/panel/panel.{h,cpp}`: `MatrixPanel_I2S_DMA` behind an
`ARDUINO`-guarded namespace (outside the render-purity guard's grep list —
purity still clean). Config from `g_cfg.hw`, nothing hardcoded; `blit`,
`splash`, `width/height/refresh_rate`, brightness 0-100→0-255. New
`env:paneltest` builds `src/main.cpp` with `-DNB_PANEL_TEST`. Device proof:
`begin()` OK 64×32, refresh=110 Hz, **internal heap -61,328 B at begin()**
— the projected 32 KB was wrong: double-buffered fb is 32 KB, the driver's
DMA task stack and structs add ~29 KB. §6 and the AGENTS budget table
corrected to 61 KB.

**FAULT 1 — green rendered blue (G/B transpose).** Isolated with a colour-
*name* card (words RED/GREEN/BLUE in their own colours, static, one read —
not fills, not timing). Consistent across all three words ruled out the
colour constants; then every software link was checked against the
installed v3.0.15 headers and cleared: our `rgb565` pack and the driver's
`color565to888` unpack are textbook-inverse R15:11/G10:5/B4:0; the cfg has
**no colour-order field** and the bus cfg hardwires `pin_d0..d5 = r1,g1,
b1,r2,g2,b2` matching `BIT_R1/G1/B1` at word bits 0-2; the brightness LUT
macro is per-channel with no crossing. The decisive argument: **LAT/OE/
A-E ride bits 6-12 of the same DMA word** — any word-bit→pin remap that
could transpose G/B would also corrupt blanking and scan, and both are
proven good (rows decoded exactly in the first report). So `rgb565()`→GPIO
is provably channel-ordered and the swap is **physical**: the SEENGREAT
V2.x wiring transposes G↔B on both halves vs. its own silkscreen
(G1 is really GPIO17, B1 GPIO8). One-place fix: `kPinsV2` is now rig truth
`{18,17,8,16,15,1,...}`, comment says hardware-proven ≠ silkscreen.
Confirmed true across resets. T-0.2's "fills render coherently" was true
and insufficient — coherent ≠ correct; colour-NAME tests are now the
acceptance criterion for any panel/adapter (AGENTS.md, README, and an
amendment box in SPIKE_RESULTS T-0.2). `test_rgb565_pack` asserts
R/G/B/W bit patterns + pack/unpack round-trip so CI catches the packing
half of this class (the rig half is unfakeable).

**FAULT 2 — garbled splash, repeated two-dot run past ".123.65".** Not an
out-of-bounds read — the blit's index math is in-bounds by construction and
`drawPixel` is bounds-checked on top. Two real bugs: (a) the splash canvas
was sized from the IP line alone (86 px) so the 105 px version line was
centred at x=-9 and lost both ends; (b) **stale-column smear** — the old
blit wrote only the clipped canvas rect into a *persistent* DMA framebuffer,
so during the traverse every panel column past the canvas edge kept the
previous frame's pixels: frozen period/dot fragments repeating on the
right. Fix: canvas = max of both lines (107 px), and blit repaints all
2048 cells every frame, off-canvas reading black through the bounds-checked
accessor. Class warning recorded at PLAN T-7.3 — the Python's fresh
`new_frame()` per iteration made this bug unportable; host tests can't see
it (no persistent framebuffer in the comparison). Confirmed on panel: full
version line, clean black tails.

## T-2.9 — boot splash (2026-09-16)

Colour card holds 3 s (every boot re-proves the transpose fix), then WiFi,
then splash: IP 6×12 white + version 5×8 green on the max-width canvas,
hold→1 px/frame traverse→hold, forever (the Python's behaviour); IP on the
panel ~2 s after connect. Native 14/14.

## T-2.10 — native live preview (2026-09-16)

Real render-module walk in `env:native`: 272 px strip (outlined text via
`draw_text_outlined`, `fill_polygon` diamond, `ellipse`, logo-blit colour
bars, tom-thumb), 64×32 window, half-block ANSI truecolour, 30 fps,
panel-width black lead-in wrapped like the Phase 7 engine, Ctrl-C quits.
Gotcha: `pio run -e native -t exec` **strips ANSI escapes** (PlatformIO's
console wrapper) — run `.pio/build/native/program` directly after
`pio run -e native`. Verified live: all strip colours present in the
output, clean loop.

## T-2.11 — phase done (2026-09-16)

Both hardware faults closed on the panel; deferred set (T-0.2 was already
closed at spike level with the caveat above) empty; purity clean; native
14/14; esp32s3 + paneltest builds green. Phase 2 complete.

## T-5.1 — data structs (2026-09-16)

`lib/data/game.h`: `Team`/`Situation`/`Game`/`GameList` POD mirrors of
Marquee's models.py dataclasses. Fixed `char[]` everywhere, `int16_t` with
`kNoInt` (INT16_MIN) for every Python `Optional[int]`, `Status` enum for the
state string. Two Python fields dropped on purpose: per-Game `league` (the
cache slot *is* the league tag) and `logo_url` (logos come from the atlas by
league+abbr, never a URL). `copy_str` truncates, never leaves unterminated,
nullptr-safe. `static_assert` trivially-copyable — the cache memcpy's whole
GameLists. Measured: Game=224 B, GameList=3600 B (16 games), whole DataCache
(8 leagues × 2 buffers) = 57.6 KB PSRAM. Native 14/14.

## T-5.2 — DataCache with pointer swap (2026-09-16)

`lib/data/cache.{h,cpp}`. PLAN wording "swap a pointer atomically" is *not
safe with two recycled buffers*: a reader holding the old pointer is still
copying it when the writer refills that buffer two commits later. Cure =
per-buffer monotonic change counter (seqlock): `writable()` bumps before a
single byte is touched (acq_rel full barrier — the bump's visibility gates
every fill store), `publish()` flips the counter even then swaps `cur` with
release. `snapshot()` = memcpy + validate counter-and-pointer, ≤8 retries,
then false → render keeps the last strip (never blocks: no mutex, no wait).
A buffer refill can only start after two publishes, so any interleaved
recycle changes the counter value, not just parity — stale matches
impossible.
Stress test does what the accept criteria say ("verify explicitly"):
50 Hz×4-league writer (200 Hz per league) vs 30 Hz reader for 1.5 s, every
game's fields a pure function of a generation number → mixed-generation read
is *exactly* detectable. First runs the CONTROL — the forbidden pattern
(mutate the current list in place, reader holds the pointer) — and asserts it
VIOLATES (≈70,000 caught/3 runs): the detector demonstrably has teeth. Then
the real cache: 0 torn, 0 retries, in 3/3 runs. Whole DataCache = 57.6 KB
PSRAM (static on device, never a task stack). -pthread added to env:native.
Native 17/17.

## T-5.3 — HTTP client wrapper (2026-09-16)

`lib/data/http.h` — header-only (both entry points are templates, so a .cpp
would be decoration): esp_http_client + `esp_crt_bundle_attach`, 15 s timeout,
`Accept-Encoding: identity`, UA `python-requests/2.31` (Akamai allowlist,
T-0.4), `HttpStream` copied from the proven T-0.5 spike view, and Marquee's
`_get()` retry profile — 3 retries, 0.5/1.0/2.0 s + uniform 0–300 ms jitter
(`esp_random`). Exactly one `close+cleanup` per `init` on every path
including mid-body failure; "one TLS session at a time" is caller discipline
(T-5.7's sequential poll task). Every ESP call verified against the installed
esp32s3 headers — one was almost wrong: `esp_http_client_get_content_length`
returns **int64_t**, not int; `HttpStat::clen` matches.
Proof = `env:httptest` on hardware, boot order WiFi → SNTP → TLS:
- forced DNS failure (`.invalid`): all 4 attempts fail, attempt gaps
  0.74/1.22/2.04 s = the backoff + jitter exactly; 5,026 ms total; internal
  heap delta +588 B; client then works normally afterwards.
- NFL scoreboard over TLS: 200, 280,458 B in 8,667 ms, internal heap delta
  **784 B after cleanup** — a ~50 KB session fully reclaimed, no leak.
  RESULT: PASS.
Finding for T-5.4/T-5.6: ESPN serves the scoreboard **chunked** —
`Content-Length: -1`, so the spike's "bytes == Content-Length" check can't
apply; the self-counted `HttpStream::bytes()` is the substitute. Also: the
NFL scoreboard is now ~280 KB, not the 3 KB the old fixture suggested —
date-window narrowing (T-5.6) matters for every league, not just MLB.
`ReadBufferingStream(Stream&, capacity)` signature re-verified in the main
project's own `.pio/libdeps`. Deps added: ArduinoJson 7.4.3 (registry) +
StreamUtils 1.9.2 — **not resolvable from the registry**, pinned by v1.9.2
commit hash in `lib_deps`; licence doc updated. esp32s3 + httptest both build.

## T-5.4 — filter documents (2026-09-16)

Split the decoder from the transport: `espn_json.{h,cpp}` (framework-free —
ArduinoJson + game.h only) carries `build_scoreboard_filter()` and
`parse_scoreboard()`, which bakes in **NestingLimit(20)** on every call so no
caller can forget it; `espn.{h,cpp}` (device) adds `scoreboard_url()`,
`make_psram_doc()` (official `SpiRamAllocator`, `reallocate` override
included) and `espn_fetch_scoreboard()` = `http_get` →
`ReadBufferingStream(s, 512)` → filtered parse. The `.cpp` transport body is
`#if defined(ARDUINO)`-guarded because the chain LDF compiles every source in
a used lib dir — native builds the decoder, never the sockets. Filter fields
= Marquee `norm_game`'s reads exactly, plus `team.name` (its displayName
fallback — the spike's filter missed it; that is +1 key of retention).
Array-filter semantics verified in the installed `JsonDeserializer.hpp`:
elements resolve their filter via `filter[0UL]`, i.e. the `[0]` element is the
template for every element. `DeserializationError` has no `UnknownError` in
v7 (enum is Ok/EmptyInput/IncompleteInput/InvalidInput/NoMemory/TooDeep) —
first esp32s3 build caught it; native never parses that file.
Fixtures copied from the Python into `test/fixtures/` (six files, 1.77 MB —
was T-5.5 prep, landed here because the filter test needs one).
Accept — host: filtered parse of `mlb_scoreboard.json` via `std::istream`
retains **6,742 B raw of 1,457,268** (limit 8 KB), 15 events, `venue`/`odds`
provably gone; native 18/18. Device (httptest phase 3, live MLB off TLS):
ok, 15 events, 6,916 B kept, **internal heap delta -156 B across the whole
fetch+parse** — the parse costs what it should: nothing internal; first game
spot-checked `id=401816960 state=pre away=CLE`. Elapsed 9.8 s unfiltered
(full default date window) — expected pre-T-5.6, and today's NFL leg also ran
50 s for the same 280,458 B as T-5.3's 8.7 s: wire time is network-weather,
T-5.6's measurement must be taken over several runs. `ReadBufferingStream`
was unexercised in the spike — it ran here, on a real stream. RESULT: PASS.

## T-5.5 — normalisation (2026-09-16)

`to_games()` in `lib/data/espn_json.{h,cpp}` — faithful port of the
Python's `norm_game`/`norm_team_from_competitor`/`_norm_situation`/
`_safe_int`/`_parse_date`. Framework-free (ArduinoJson + `game.h` only).
Epoch math is Hinnant's `days_from_civil` (no `timegm` portability
roulette); ESPN dates are whole-minute so `%4d-%2d-%2dT%2d:%2d` suffices;
unparseable date -> `start_utc=0` (Python uses `now()` — the goldens never
hit it). `_safe_int` accepts ints **and** numeric strings — ESPN scores
are strings. `possession` accepts object-with-id, bare string or bare int.
Placeholder `AWY`/`HME` fires only when the competitor entry itself is
absent, matching the Python's falsy-dict check (a present competitor with
no `team` still gets id `0` / abbr `???`).

Goldens are generated by `tools/gen_games_golden.py`, which imports the
Python's own `norm_game` (httpx stubbed, MARQUEE_REPO read-only) over each
fixture — the C++ test never sees the Python at test time.
Accept — host: all six fixtures match the Python **field-for-field**,
29 games total (epl 10, mlb 15, live/nba/nfl/nhl 1 each); goldens
committed under `test/golden/`. Checked field lengths against the struct
caps first (max name 23 < 32, clock 7 < 8) so plain equality is exact, no
truncation masking. esp32s3 builds; native 19/19.

## T-5.6 — narrow the date window (2026-09-16, REQUIRED)

`lib/data/date_window.{h,cpp}`: `local_day()` (IANA TZ via `setenv`+`tzset`,
`strftime %Y%m%d`) and `filter_yesterday_today()` — the `_filter_by_date`
port, compacting in place, keeping local yesterday+today (`tm_mday-1`+
`mktime` normalises across DST, tested), keeping `start_utc==0` (the
unparseable-date cases the Python stamps with `now()`). `scoreboard_url()`
grew a `dates` param; `espn_fetch_scoreboard()` grew a wire-bytes out-param.
Host: `test_data_date_window` (TZ set to America/New_York for determinism)
covers windowing, DST edge and the keep-list; native 20/20.

**Two ESPN API changes found while measuring** (verified with curl, same
UA, decompressed): `dates=YYYYMMDD-YYYYMMDD` — the Python's exact
yesterday-tomorrow window — and comma lists now return
`400 Failed to get events endpoint`; only **single days** are accepted.
Default (no `dates`) is already ~one day: 297,882 B vs 297,962 B for
`dates=<today>`. The 1.46 MB three-day era is over upstream — yesterday's
finals now *require* a second single-day fetch
(`dates=<yesterday>`), which T-5.7 merges via `filter_yesterday_today`.

**Device (httptest phases, ~298 KB single-day MLB):** filtered fetch+parse
per run: 7.9 / 8.0 / 7.8 s, and a later boot 14.1 / 15.0 / 10.1 s — while
the identical-size NFL body (transport only) took 1.35 / 1.51 / 1.94 / 6.7 s
across the same runs. **Wire time is pure network-weather: 40–200 KB/s
swings, same payload.** So a parse-vs-wire diagnostic was run: one body
fetched to a PSRAM buffer (2,849 ms wire) then re-parsed from memory eight
times across allocator placements: **90–92 ms every time** (psram-filter+
psram-doc = int-filter+int-doc; internal doc peak 23,360 B). The filtered
parse is *not* the budget problem and never was — the 6 s figure is
dominated by ESPN/WiFi transfer of ~300 KB. Accept: wire is already at the
single-day minimum; best-case measured 2.8 s + 0.09 s parse ≪ 6 s; worst
observed 17 s is transfer weather that gzip (T-11.3) would cut ~5×, and
the measured duty cycle during a live-game poll says revisit T-11.3 if
on-site RSSI keeps wire above ~4 s. The parse-diag phase stays in
`NB_HTTP_TEST` as a permanent one-command split of wire vs parse.

## T-5.7 — poll scheduler + device proof (2026-09-16)

`lib/data/poll.{h,cpp}` — pure `PollScheduler`: per-league deadlines,
`kBootStaggerS=5` so N enabled leagues spread over N·5 s, `done()` arms
live (20 s) or idle (120 s) per `LeagueConfig` (Marquee's values),
`next_wake()` for the sleeper. `config.h` gains `kLeagueApiPaths[]`
(the Python's `LEAGUE_SLUGS` dict as an array); `date_window` gains
`local_yesterday()` (DST-safe, for the separate yesterday leg). Native
suite 21/21, new `test_data_poll_scheduler` covers stagger, both cadence
flips, next-wake min and the all-disabled fallback.

Device proof (`NB_POLL_DEMO`, tasks on the real topology — poll core 0
prio 2, 30-fps probe core 1 prio 3, five pro leagues enabled, 150 s):

    poll-cycle: 5 leagues in 46042 ms
    poll-demo: cycle=1 sess_max=1 fps_min=30.3 heap_min=243796 B fails=0
    POLL RESULT: PASS

Live/idle cadence both observed for real: MLB had live games → re-polled
every ~20 s (4×); the idle four came due again only at the 120 s mark.
Per-poll internal delta ≤ 832 B, returning to ~0. MLB wire 17 s on the
home AP (312 KB ≈ 18 KB/s — transport weather, not cadence). The 46 s
"full cycle" is that weather; the sequential-one-session design is what
the accept criteria asked for and `sess_max=1` proves it.

**Finding — WiFi modem sleep aborts the boot.** Reproducible (6/6 boots):
`ESP_ERROR_CHECK ... esp_timer_create ... phy_track_pll_init` NO_MEM in
`ppTask`/`pm_dream` on core 0 — the modem-sleep PHY-wake path re-creates
its PLL timer on every wake and the alloc fails under TLS/parse churn.
Fixed at both connect sites with `WiFi.setSleep(false)`: the panel is
wall-powered, modem sleep buys nothing and costs the whole board.
Production env also carries the fix.

## T-5.8 — parser test corpus (2026-09-16)

No code written — the corpus landed with T-5.4/T-5.5. Verified: all six
Marquee fixtures are committed in `test/fixtures/` byte-identical to
`tests/fixtures/` upstream (`cmp` clean), and `test_data_norm_golden`
parses every one from a file stream through the same `parse_scoreboard`
(stream + filter + NestingLimit 20) the device runs on a socket.
Accept: 6/6 parse in `pio test -e native` (21/21); no test opens a
socket.

## T-5.2 device proof — cachetest (2026-09-16)

Host stress proved the protocol on x86; Xtensa ordering is weaker, so
`env:cachetest` re-runs it on device: identical generation payload and
detector, writer core 0 prio 2 (50 Hz x 4 leagues), reader core 1 prio 3
(30 Hz), DataCache in PSRAM where the shipped instance will sit.

Result (serialcap):
  control (in-place mutation): torn=100 last_gen=2 after 57 ms — detector bites
  hammer (max-rate league 0, 10 s): retries=49 torn=0 — retry branch fires on Xtensa
  stress 180 s: reads=5455 retries=0 torn=0 str_gen=36001 (reader tracked
  200 publishes/s to the last one — no stale-cache freeze)
  CACHE RESULT: PASS
Actual-cadence retries=0 matches the arithmetic (50 Hz/s x ~100 us copy
x 180 s = ~0.9 expected overlaps), hence the max-rate hammer phase.
Two harness bugs fixed en route: a prio-3 hammer reader must yield
*unconditionally* (retry storm starved the core-1 monitor), and the
stress loop needs a fixed end anchor.

## T-5.9 — Phase 5 close (2026-09-16)

Phase 5 (data layer) is done and device-proven:
- `Game`/`GameList`/`Situation` PODs, `DataCache` seqlock (T-5.2) with
  the stress test re-run on Xtensa hardware (`env:cachetest`,
  ac84c60): control bites in 57 ms, max-rate hammer 49 retries / 0 torn,
  real-cadence 180 s run 5455 snapshots / 0 torn, reader tracks
  publishes to the last one.
- `esp_http_client` + `esp_crt_bundle` transport with jittered backoff,
  `ReadBufferingStream`, filtered ArduinoJson v7 parse with
  `NestingLimit(20)`; ESPN single-day reality + separate yesterday leg
  (T-5.3–T-5.6); live poll ~298 KB, filtered parse ~90 ms, fetch time
  is wire weather.
- `PollScheduler` on the real task topology with both cadences observed
  on device, `sess_max=1`, PASS (T-5.7).
- Fixture corpus: 6/6 byte-identical to Marquee, all parsed in CI (T-5.8).
- Boot-loop diagnosis (`phy_track_pll_init` abort under WiFi modem
  sleep → `WiFi.setSleep(false)`, production) and its follow-up: the
  Phase 0 "1-in-3 silent boot" finding is resolved by the same fix —
  18/18 clean EN resets after (SPIKE_RESULTS amended).

## Atlas v2 — per-card-height rows (2026-09-16, Phase 6 prerequisite)

Phase 6 cards draw logos at five sizes (12/13/19/24/30 — game_strip's
`_NFL_LOGO_H`/`_LOGO_H_LIVE`/`_LOGO_H_POST`/`_LOGO_H_PRE`/`_LOGO_H_BLEED`).
The Python resizes the 32 px art per size with LANCZOS; the ESP32 has no
resampler, so v1's single nominal row could not reproduce card art. v2 bakes
every card size at build time:

- one index row per `(league, abbr, h)` — `h` is the display height **and**
  the exact blob height, so the reader never resamples; index sorted with the
  height tiebreak (v1 sorted by league+abbr only).
- every row is composited over black in Pillow and carries
  `mask = source alpha > 0`: `card.paste(logo, box, mask=logo)` onto a black
  card *is* premultiply, and re-quantising an already-RGB565 value is the
  identity — so the device's masked blit reproduces the Python's soft-alpha
  blend exactly.
- the v1 nominal-32 source-art row is gone (cards never request it; every
  `_paste_logo` call resizes first). `find()` at a non-card height misses by
  design.
- writer/reader/tests all assert `key h == blob h` and the
  `_paste_logo` resize width math (`round(ow * h / oh)`).

Measured: 144 logos × 5 heights = 720 rows, **759,650 B** (v1 was 258,992 B)
— still inside the 2 MB partition. College at v2 (~5.3 MB) will **not** fit;
T-11.7 needs a wider `logos` partition. PLAN §3/§6 updated.

Checks:
- `tools/test_build_logos.py`: processing parity + round-trip (every row's
  dims match the strip math) + premul re-encode + collision keying. Green.
- `pio test -e native` 21/21: synthetic v2 header, key-h tiebreak,
  wrong-height miss, real `logos.bin` 720 rows, FNV-1a of PRE rows from the
  host table, variant hits at 13/30, miss at 32. `golden_logo.h` regenerated
  at `epl:LIV@24` (13×24), blit parity clean.
- Device (`env:logostest` + `logos.bin` @ 0x810000):
  `init=1 count=720`; all six PRE rows hit with host dims and hashes
  (mlb:BOS 17×24 8294c099 … nhl:BOS 24×24 8f68df91); misses at h=32 and
  `mlb:ZZZ`; lookup heap delta 0/0, mmap −104 B; T-3.7 frame-phase trap
  fires as designed. RESULT: PASS.

## T-6.1 — CardProducer interface (2026-09-16)

Added `lib/render/card_producer.h`: CARD_W=64, virtual `id`, `cards_key`,
`cards`, optional `is_visible` and `has_live_priority_games`, plus
`compose_cards()` concatenation over visible producers. This mirrors
Marquee's `CardProducer` protocol while keeping the render layer bounded
and framework-free.

`pio test -e native` now 22/22 with a stub two-producer composition test.
Purity guard passes.

## T-6.2 — Clock widget (2026-09-16)

Added `lib/render/clock_widget.h`: CardProducer with injected local-time
callback, 12h/24h formatting, and the Marquee clock layout (time y=2,
date y=22). The callback keeps lib/render framework-free and lets tests fake
the exact timestamp used by the existing Pillow golden.

Native suite is 23/23: the widget renders the golden clock with zero
differing RGB565 pixels, and the key stays constant within the minute but
changes across minute/format changes.

## T-6.3 — PRE game card (2026-09-16)

Extracted the local-time helper into `lib/render/local_time.h` and added
`lib/render/game_card.h` with Marquee's `_paste_logo` behavior plus the PRE
card layout: 24 px corner logos, centered `VS`, and the local start time
bottom-centered. The logo resolver is injected, keeping the render layer
pure and testable.

Fixed the fallback width to match `game_strip._paste_logo()`: `textlength + 1`
for the abbreviation box. Added `tools/gen_game_pre_golden.py`, which renders
the PRE fallback card with Pillow directly, and `test_game_card_pre`.

Checks:
- `pio test -e native` 24/24, including zero-diff PRE card parity.
- `bash tools/check_render_purity.sh` → `purity: OK`.

## T-6.4 — FINAL game card (2026-09-16)

Added `render_game_card_post()` to `lib/render/game_card.h`: 19 px corner
logos, centered spleen-6x12 score at y=11, and the dim status line at y=22.
The status line matches Marquee: same-day `FINAL`, extra-innings `F/N`, and
previous-local-day `M/D`. The renderer takes `now_utc` plus the injected local
time callback so the day boundary uses local calendar fields, not UTC date
rollover.

`LocalTime` gained `year` for that date comparison. Added
`tools/gen_game_post_golden.py`, generating both the same-day and
previous-day Pillow goldens, and `test_game_card_final` to diff them.

Checks:
- `pio test -e native` 25/25, zero-diff for both FINAL variants.
- `bash tools/check_render_purity.sh` → `purity: OK`.

## T-6.5 — generic LIVE game card (2026-09-16)

Added `render_game_card_live()`: 13 px four-corner fallback/logo boxes with
away and home scores beside them. The Python's returned `_paste_logo` width
drives both score offsets, so the no-logo path keeps the score clear of the
abbreviation box. MLB/NFL situation overlays remain for later tasks.

Added `tools/gen_game_live_golden.py` and `test_game_card_live`.

Checks:
- `pio test -e native` 26/26, zero-diff generic LIVE card parity.
- `bash tools/check_render_purity.sh` → `purity: OK`.

## T-6.6 — MLB live situation indicator (2026-09-16)

Added `lib/render/situation.h` and wired it into the generic LIVE MLB card.
The indicator draws the base diamond, outs, inning arrow/number, and ball-strike
count using Marquee's diamond/indicator coordinates.

Ported Pillow's filled polygon scanline convention into
`fill_polygon()`, including its `ROUND_UP`/`ROUND_DOWN` behavior, horizontal
edge handling, and corner adjustment, so the small triangle and base fills are
pixel-exact. The 2x2 out-dot outline uses Pillow's exact 5x5 ellipse pattern.

Added `tools/gen_game_live_situation_golden.py`, `golden_game_situation.h`, and
`test_game_situation` for missing, empty, partial, and loaded base states.

Checks:
- `pio test -e native` 27/27, zero-diff MLB situation parity for all cases.
- `bash tools/check_render_purity.sh` → `purity: OK`.

## T-6.7 — NFL live gridiron card (2026-09-16)

Routed `nfl` and `college-football` LIVE cards to the Marquee stacked layout:
12 px logos, centered quarter/clock and down/distance rows, possession football,
and the 8 px bottom field strip. Possession follows the situation's team ID, and
`is_red_zone` changes no field-strip pixels.

Added `draw_gridiron()` and its exact goalpost/ball pixel patterns to
`lib/render/situation.h`, plus `tools/gen_game_live_nfl_golden.py` and
`test_game_card_live_nfl` for away, home, missing-situation, and red-zone cases.

Checks:
- `pio test -e native` 28/28, zero-diff NFL live parity for all cases.
- `bash tools/check_render_purity.sh` → `purity: OK`.

## cardtest follow-up — hardware confirmation (2026-09-17)

Extended the hardware `cardtest` cycle from four cards to the eight current
Phase 6 variants: PRE, FINAL, generic LIVE, MLB diamond, and the four NFL
gridiron states. The user confirmed the panel showed all cards correctly,
including gridiron logo colors, legibility, and no smearing.

Checks:
- `pio test -e native` → 28/28.
- `pio run -e esp32s3` → SUCCESS.
- `pio run -e cardtest` → SUCCESS.
- `bash tools/check_render_purity.sh` → `purity: OK`.

## T-6.8 — period-clock live cards (2026-09-17)

Added NHL, NBA/college-basketball, and soccer period-clock LIVE routing to
`render_game_card_live()`, matching Marquee's bleeding-edge 30 px logos and
centered outlined score/period/clock layout. The league check is passed into
the renderer because `data::Game` intentionally does not carry its league.

Ported the period/clock formatting: NHL `P1/P2/P3/OT/SO`, NBA `Q1-Q4/OT/2OT`,
soccer `1ST/2ND/HT` plus minute-only clock normalization. The renderer ignores
situation for these leagues, matching the Python route.

Added `tools/gen_game_live_periodclock_golden.py`, `golden_game_live_periodclock.h`,
and `test_game_card_live_periodclock` for NHL, NBA/OT, soccer minute, and soccer
halftime cases. Updated hardware `cardtest` to 11 cards: generic LIVE is now MLB
no-situation, and NHL/NBA/soccer period-clock variants cycle separately.

Checks:
- `pio test -e native` → 29/29, zero-diff period-clock parity for all cases.
- `bash tools/check_render_purity.sh` → `purity: OK`.
- `pio run -e esp32s3` → SUCCESS.
- `pio run -e cardtest` → SUCCESS.

## T-6.9 — scoreboard widget (2026-09-17)

Added `lib/render/scoreboard_widget.h`: a `CardProducer` that snapshots one
league cache, hides itself when disabled or empty, renders one PRE/LIVE/POST
card per cached game, and stable-sorts priority/favourite teams first.
`has_live_priority_games()` honors the enabled flag so disabled scoreboards do
not preempt the carousel.

The rebuild key hashes league/id order, scores, and the Python-compatible
situation fingerprint only. Clock ticks do not change the key. Added
`test_scoreboard_widget`, covering N-card output, blank-card checks, priority
priority/live visibility, score/situation key changes, and clock-only
non-changes.

Checks:
- `pio test -e native` → 30/30.
- `bash tools/check_render_purity.sh` → `purity: OK`.
- `pio run -e esp32s3` → SUCCESS.
- `pio run -e cardtest` → SUCCESS.

## T-6.10 — Phase 6 close-out (2026-09-17)

Phase 6 is complete: card producers, PRE/FINAL/LIVE game-card states, MLB
situation indicators, NFL gridiron cards, NHL/NBA/soccer period-clock cards, and
the scoreboard widget.

Final phase checks after the T-6.9 scoreboard commit:
- `pio test -e native` → 30/30.
- `bash tools/check_render_purity.sh` → `purity: OK`.
- `pio run -e esp32s3` → SUCCESS.
- `pio run -e cardtest` → SUCCESS.

## T-7.1–T-7.5 — strip, scroll, paging, preemption, publication (2026-09-17)

Added `lib/render/strip.{h,cpp}` (mega-strip composer: cards concatenated with
`card_gap` into one PSRAM canvas, `compute_pages` for static mode,
`order_producers` for favourite-team preemption) and
`lib/render/scroll.{h,cpp}` (`strip_w + panel_w` period with black lead-in,
`scroll_rewrap` on rebuild, full-repaint `blit_window` host mirror of
`panel::blit`, `page_window_x` 5 s dwell, `StripHolder` double-buffer with
atomic pointer swap + generation counter).

Wired normal boot in `src/main.cpp`: `task_net` (WiFi + SNTP supervision),
`task_poll` (PollScheduler from `LeagueConfig`, fetch+publish, cards_key
change → rebuild on core 0, 1 s tick so clock-minute keys land promptly),
`task_render` (core 1: front()/generation only, scroll or static paging,
`panel::blit(canvas, -w0, 0)` full-panel repaint, live config on notify).
Setup inits panel (brightness clamped ≤50 % on the bench until the 4 A PSU
is confirmed), logos mmap, PSRAM DataCache, carousel-ordered producers from
`widgets[]` with favourites per league.

**Verified on hardware same day** — see the T-7.6/T-7.7 section below:
no stale columns, correct wrap, sustained frame pacing measured through a
poll cycle. T-7.3 and T-7.6 both VERIFIED ON HARDWARE.

Checks:
- `pio test -e native` → 37/37 (7 new Phase 7 cases).
- `bash tools/check_render_purity.sh` → `purity: OK`.
- `pio run -e esp32s3` + all six device test envs → SUCCESS.

## CI red-X investigation + stress re-audit under load (2026-09-17)

Red run 35241559376 (T-6.9) was **not a test failure**: the host-test job
was green in that run; the X was `Build esp32s3` **cancelled** by the next
push under `cancel-in-progress: true` ("Canceling since a higher priority
waiting request exists"). Recorded in AGENTS.md so a superseded X is read
as superseded, not broken. `cancel-in-progress` deliberately kept.

`test_data_cache_stress` re-audited under adverse scheduling — the
hypothesis for the (nonexistent) flake: `pio test -e native` × 25 back-to-back
while busy-loops pinned all 14 cores. **25/25 pass**, control bites, zero
torn reads. That is stronger evidence for the publish-by-pointer-swap +
seqlock discipline than the original single 180 s run: the detector holds
even when threads are preempted to starvation. No suite flake to chase.

## T-7.6 + T-7.7 — hardware verification, phase close (2026-09-17)

First full-pipeline hardware session: panel, logos mmap, WiFi, SNTP, one
TLS poll cycle, strip build and scroll, all in the normal-boot firmware.

**T-7.3 stale-column check — VERIFIED ON HARDWARE.** Continuous scroll,
no smearing, correct behaviour through the wrap: the full-panel repaint
holds on the *persistent* DMA framebuffer, which is the exact condition
host tests cannot create (they allocate a fresh canvas).

**T-7.6 frame pacing — VERIFIED ON HARDWARE.** `[render] fps` window
logging (10 s) added to `task_render`, plus a serial-keypress injector
for the forced 200 ms stall. Panel confirmed: forced stall **hitches,
then resumes holding the image — never blanks** (autonomous DMA refresh,
as the architecture promises). Sustained **30.3 fps** through a poll
cycle (`[poll] mlb ok=1 wire=268,057 B` + strip rebuild inside the fps
windows), steady-state **worst frame gap 34 ms**, zero render-task heap
drift. Pre-fix, the fixed 33 ms notify timeout paced at **27.8 fps** —
just under the 28 target; pacer now budgets `33 ms − busy time` (the
first attempt subtracted the whole period and ran the loop at 55 fps —
feedback errors are measurable, not debatable).

**Finding — a blocking log call was stalling the render task.** Two
~2035 ms worst-frame gaps appeared in the fps log with no corresponding
work. Cause: `Serial.printf` from `task_render` blocks in the S3 USB-CDC
TX path while USB state churns (host open/close, detach); the default
per-write timeout is seconds. Only surfaces with a terminal attached or
during USB events — miserable to reproduce later, so recorded here:
the render task's hard "never block on I/O" rule was being violated by
its own logging. Fix is one line in `setup()`:
`Serial.setTxTimeoutMs(0)` — writes drop instead of blocking (confirmed
in `USBCDC.cpp`: `non_blocking` path returns when the FIFO is full).
Re-verified by straddling a 10 s port detach inside an fps window:
worst-frame-gap stayed at **33 ms** while logs dropped.

Stacks: render min-free 5,952 B of 8 KB, web 6,484 B of 8 KB — both
inside the >25 % headroom rule.

**T-7.7 — Phase 7 closed.** All seven tasks verified: T-7.1/2/4/5 host,
T-7.3 host + hardware, T-6 side untouched, T-7.6 hardware. PLAN §6 strip
and per-frame blit figures promoted from projection to measured.

Checks:
- `pio test -e native` → 37/37; purity OK; `esp32s3` SUCCESS.
- Hardware: boot → WiFi → SNTP → poll ok → `[strip] rebuilt: 8 cards,
  w=576, pages=8` → sustained 30.3 fps; visual checks above.

## Split test envs out of main.cpp

`src/main.cpp` had grown to 2,039 lines carrying six `#ifdef NB_*_TEST` device
test mains plus normal boot. They are now one file each under `src/envs/`,
selected per build by PlatformIO `build_src_filter` — the `#ifdef` dispatch
inside one translation unit is gone. Purely mechanical: each block is verbatim
(diff-checked against the pre-split file); behaviour and the boot banner are
unchanged.

- `src/envs/common.h` — the pre-dispatch sequence every build shared (banner,
  sdkconfig/partition snapshots, PSRAM canvas allocator, the one global `Config`)
  plus `boot_prologue()` / `boot_load_config()`. Its `static` definitions land
  once per binary because exactly one of `main.cpp` / the env files compiles.
- `src/envs/{panel,logos,config,http,cache,card}_test.cpp` — each holds its old
  block verbatim (guard kept, so a stray compile stays inert) and a `setup()`
  that runs `boot_prologue()` then the test entry, matching the old dispatch
  order exactly: logos/http/cache before config load, config runs its own
  reset/load, panel/card load `g_cfg` first via `boot_load_config()`.
- `platformio.ini`: `esp32s3`/`native` get `+<*> -<envs/>`; each test env
  overrides with `-<*> +<envs/<x>_test.cpp>` so `main.cpp` and the five siblings
  are excluded. (Lesson: a bare `-<envs/>` filters an empty include-set — the
  default `+<*>` is not implied, and `main.cpp` silently stops being built.)

Checks:
- `pio run -e esp32s3` → SUCCESS; all six test envs → SUCCESS.
- `pio test -e native` → 37/37; `pio run -e native` (live preview) SUCCESS.
- `bash tools/check_render_purity.sh` → OK.

## T-8.1 — async web server bring-up (2026-09-18)

Pinned the ESP32Async v3 stack and brought the server up from `task_web`:

- `AsyncTCP` `v3.5.0` and `ESPAsyncWebServer` `v3.12.1`, both LGPL-3.0 and
  recorded in `docs/DEPENDENCY-LICENCES.md`.
- `lib/web/web.cpp` creates one `AsyncWebServer` on port 80 and registers
  `GET /ping` as the T-8.1 static-route proof.

The first hardware attempt reboot-looped in `AsyncServer::begin()`:
`xQueueSemaphoreTake((pxQueue))` asserted because `task_web` raced `task_net`
and bound before LwIP's `tcpip` thread existed. The stack is now initialised
once in `setup()`, before task creation, and `task_web` waits for a Wi-Fi
lease before starting the server.

Measured internal heap cost (hardware, after Wi-Fi connect):

```text
server=244 B route=160 B begin=17,504 B total=17,908 B
free=112,404 B largest block=71,668 B
```

That is comfortably within the AGENTS internal-RAM budget. HTTP proof:

```text
GET /ping     -> 200, body "pong"
GET /missing  -> 404
```

Checks:
- `pio run -e esp32s3` → SUCCESS; RAM 15.8 %, Flash 28.6 %.
- `pio test -e native` → 37/37; `pio run -e native` → SUCCESS.
- `bash tools/check_render_purity.sh` → OK.
- `pio run -t compiledb` → refreshed after adding `lib/web`.

## T-8.2 — serve the gzipped SPA (2026-09-18)

Copied Marquee's SPA unchanged from `marquee/web/ui/index.html` into
`assets/web/index.html`. `tools/pack_web.py` gzips it to
`data/index.html.gz`, and PlatformIO runs the packer before firmware builds or
filesystem uploads:

```text
21,358 B -> 4,930 B (23.1%)
```

The device mounts the `web` SPIFFS partition and serves the compressed file at
`/` and `/index.html`:

```text
mounted web partition (5,271 / 956,561 B)
server=244 B fs=4,328 B routes=480 B begin=17,504 B total=22,556 B
free=107,668 B largest block=67,572 B
```

HTTP proof:

```text
GET /          -> 200, Content-Encoding: gzip, ETag "DCF51D4B", 4,930 B
GET /index.html -> 200, same gzipped SPA
GET /ping      -> 200, body "pong"
GET /missing   -> 404
```

The first upload used `data/web/index.html.gz`, but PlatformIO uploads the
contents of `data/` as the filesystem root. The pack target therefore became
`data/index.html.gz`, matching the virtual path requested by
`AsyncFileResponse`.

Checks:
- Firmware and SPIFFS filesystem uploaded over `/dev/ttyACM0`.
- `pio test -e native` → 37/37; `pio run -e native` → SUCCESS.
- `bash tools/check_render_purity.sh` → OK.
- `pio run -e esp32s3` → SUCCESS; RAM 15.8 %, Flash 29.5 %.

## T-8.3 — /api/settings, /api/system (2026-09-18)

Added to `lib/web/web.cpp` against the SPA's exact shapes (no redesign):

- `GET /api/settings` — the ten fields the Display screen binds
  (`rows…clock_24h`; `display_mode` is the `"scroll"`/`"static"` string).
- `PUT /api/settings` — `AsyncCallbackJsonWebHandler` (v3 header:
  `AsyncCallbackJsonWebHandler(AsyncURIMatcher, ArJsonRequestHandlerFunction)`,
  `setMethod`, `setMaxContentLength`). Absent keys keep stored values,
  numbers clamped (`brightness 0–100`, `scroll_speed 10–200`, `card_gap
  0–32`, geometry sane bounds), non-object body → 400. Response
  `{"restart_required": …}` drives the SPA banner.
- `GET /api/system` — `ip`, `uptime_s`, `free_heap` (internal), `psram_free`,
  `version` (`-DNB_FW_VERSION`, now in platformio.ini), `restart_required`
  (T-4.4 flag, RAM-sticky; a reboot applies config and clears it).

Routes cost 456 B of the 936 B total routes figure (was 480). Each PUT
`config::save()` notifies the render task (T-4.3) — brightness/timezone
apply live, verified.

Hardware proof (`192.168.123.54`):

```text
GET  /api/system            -> ip/uptime/free_heap 35,120 B internal/psram/version 0.8.0
PUT  /api/settings (full form)   -> {"restart_required":false}; GET reads brightness 70 + tz back
PUT  /api/settings {"rows":16}   -> {"restart_required":true}; /api/system flag true
PUT  /api/settings body [1,2]    -> 400
```

Bench config restored afterwards (rows 32, tz ""). Note the flag is sticky
until reboot by design — 32→16→32 within one session leaves it set; a
boot-time snapshot comparison would clear it but the boot-time hw struct is
not visible from lib/web. Marquee behaved the same way.

Checks:
- `pio run -e esp32s3` → SUCCESS (RAM 15.8 %, Flash 29.8 %); flashed.
- `pio test -e native` → 37/37.
- `bash tools/check_render_purity.sh` → OK.
- clangd diagnostics on lib/web are xtensa-flag noise (`machine/endian.h`
  chain fail); pio build is the arbiter.

## T-8.4 — /api/sports/*, /api/favorites (2026-09-18)

SPA shapes, no redesign (`/api/sports/leagues`,
`/api/sports/leagues/{id}`, `/api/sports/{league}/teams`,
`/api/favorites` GET/POST/DELETE):

- Leagues: GET lists the seeded `LeagueConfig` rows; PUT `{enabled}`
  persists. Widget consequences land at restart — the SPA's own copy says
  "New sport widgets appear after restart", parity kept.
- Teams: `nb::data::espn_teams_json(path, &len)` in `espn.cpp` — filtered
  ESPN `/teams` fetch (one PSRAM doc, `NestingLimit(20)` reused via
  `parse_scoreboard`), serialized `[{id,name,abbreviation}]` into a growing
  PSRAM buffer, single-slot 24 h cache. The web handler copies into a String
  before `send()` so an async flush can never outlive a later cache refresh.
- **TLS mutex is now enforced, not just disciplined**: `tls_take(10000)` /
  `tls_release()` wrap every `http_get` inside `espn_fetch_scoreboard` and
  `espn_teams_json` (AGENTS one-session rule; poll vs team-picker contention
  point). Handler blocks on the async_tcp thread ≤10 s worst case — the
  picker is a user click, and 502 on timeout.
- Favorites: index-as-id (SPA uses `fav.id` only as a key), dup POST → 409,
  full list → 409, unknown league → 400/404. `task_poll` now calls
  `boot_apply_favorites()` every pass and folds favourite `team_id`s into
  the strip key — favourites take effect on the next poll pass, no reboot
  (T-7.5 preemption).
- Route order: exact `/api/sports/leagues` before prefix `/api/sports/`
  (AsyncWebServer is first-`canHandle`-wins). `pathArg()` was not used — it
  needs `ASYNCWEBSERVER_REGEX`; URL tails are parsed by hand instead.

Hardware proof:

```text
GET/PUT leagues                -> epl enable/disable round-trips; unknown -> 404
POST /api/favorites NYY 33     -> {"id":0,...}; dup -> 409; DELETE -> {"removed":true}
GET /api/sports/mlb/teams      -> 30 teams (ARI first); epl 20; cached second call instant
GET /api/sports/nba2/teams     -> 404
favorites after real reboot    -> intact; poll+strip rebuild clean (8 cards, w=576)
free_heap after teams fetch    -> 98,408 B internal
```

Checks:
- `pio run -e esp32s3` → SUCCESS; flashed; T-8.3 CI green.
- `pio test -e native` → 37/37.
- `bash tools/check_render_purity.sh` → OK.

## T-8.5 — /api/widgets, reorder, producer order wiring (2026-09-18)

- `GET /api/widgets` (carousel order: `{id,type,order,enabled,
  dwell_seconds[,league]}`), `PUT /api/widgets/{id}` (`enabled` and/or
  `dwell_seconds`, clamped 1–120, returns the updated widget — the SPA's
  `Object.assign` contract), `POST /api/widgets/reorder {ids:[…]}`
  (positional; unlisted ids keep relative order at the tail).
- **Producer order is now the carousel order.** Phase 7 built the strip
  clock-first-then-league-slug; `boot_create_producers` (construct once)
  and `boot_order_producers` (widgets sorted by `order`, refreshes the
  stable `g_wen[]` enabled flags) replace `boot_build_producers`, and
  `task_poll` re-orders every pass — drag-reorder lands without restart.
- **Root-cause fix — poll never saw config writes.** `g_cfg` is reloaded
  by the *render* task on save-notify (T-4.3); poll read boot-time values
  forever. Poll now owns its own copy (`config::load(pc)` once per pass)
  and the order/favourites/strip-key/rebuild path takes `const Config&` —
  no cross-task mutation of `g_cfg`.
- Static-mode dwell: `render_dwell_s()` reads the first enabled widget's
  `dwell_s` (was hardcoded 5). ponytail: pages carry no widget
  attribution, so per-widget dwell is stored but page-global; comment in
  main.cpp names the upgrade path.

Hardware proof:

```text
GET /api/widgets              -> 10 seeded widgets in order
PUT scoreboard_mlb enabled=false/true -> widget JSON back, 200
PUT clock dwell_seconds=9            -> persists
POST reorder (clock last)     -> next poll pass rebuilt the strip
poll fetched MLB              -> [strip] rebuilt: 8 cards, w=576 (clock now last;
                                   order visual check rides on T-8.6 /preview)
```

Found while testing: **opening the USB CDC serial port chip-resets the S3**
(`rst:0x15 USB_UART_CHIP_RESET`) regardless of DTR state — a second pyserial
open looked like a poll stall. Keep one long-lived serial session.

Checks:
- `pio run -e esp32s3` → SUCCESS; flashed.
- `pio test -e native` → 37/37; purity OK.

## T-8.6 — /preview BMP, show-ip splash (2026-09-18)

- `GET /preview` + `/api/system/preview` (the SPA dashboard binds the
  latter): the current front strip as a 24-bit bottom-up BMP — 14+40-byte
  header, rows padded to 4, `unpack565` → BGR, no zlib.
- Memory model: `send(uint8_t*,len)` is NOT copy-safe (v3's
  `AsyncProgmemResponse` stores the pointer — it is built for PROGMEM), and
  a static PSRAM buffer would race a rebuild/realloc mid-flush. So: the BMP
  renders into a response-owned PSRAM block (`shared_ptr<BmpBuf>` captured
  by the `sendChunked` filler — freed exactly when the response dies).
  Strip is only touched during the synchronous header+rows build (~ms),
  the same rebuild race the render task already accepts.
- `POST /api/system/show-ip` → render task scrolls the IP for 8 s
  (T-2.9 hold-scroll-hold behavior, 20 px/s, amber 6x12 outlined, wrap-safe
  deadline). Wiring is two boot setters in `web.h` — web never sees main's
  globals.

Hardware proof:

```text
GET /preview  -> 55,350 B = 54 + 576*32*3 exactly; BM/54/40/24bpp, dims 576x32
                 lit-px/card [428 304 415 379 374 322 323 182] — last card is
                 the clock: the T-8.5 reorder is visible end-to-end
GET /api/system/preview -> 200 image/bmp (SPA dashboard path)
POST /api/system/show-ip -> {"ok":true}, device alive, fps unaffected
6 throttled concurrent previews + reorder mid-flush -> no crash, no reboot
```

Checks:
- `pio run -e esp32s3` → SUCCESS; flashed.
- `pio test -e native` → 37/37; purity OK.

## T-8.7 — vendored onboarding page (2026-09-18)

`assets/web/onboard.html` — the AP-mode provisioning page with **zero
external requests**: hand-written inline `<style>` (system monospace stack,
dark, phone-sized form), plain HTML POST to `/api/net/connect`. Served at
`/onboard` (+ `.html`) from the web partition; `pack_web.py` now packs the
`PAGES` tuple (index 21,358→4,930 B, onboard 2,289→1,182 B).

Scope kept honest: T-9.1 owns the SoftAP/captive portal and the
`/api/net/connect` handler — this task is the vendored page itself, which
T-9.1 serves instead of the CDN-bound SPA. `grep cdn|tailwind|alpine` on
the served body: 0 hits. The form 404s until T-9.1 lands — build order,
not a defect.

Checks: firmware + SPIFFS flashed; `GET /onboard` → 200 gzip ETag
"B2566E67", body renders the form.

## T-8.8 — Appropriate Legal Notices in the SPA (2026-09-18)

Added an About tab to the nav (one click, always visible) satisfying
GPLv3 §5d on all four points:

- Copyright — "Copyright © 2026 Anthony Fenech" (matches the licence's
  apply-section).
- No warranty — the §16 disclaimer paragraph verbatim in prose.
- Redistributability — §0/§2 statement, GPLv3 (not "or later"), matching
  GPL-3.0-only.
- How to view — link to gnu.org's gpl-3.0.html + the source URL
  (github.com/Omnicef/nosebleed-fw), and the firmware version fetched
  live from `/api/system` so it can't drift.

Also states the interface itself is GPL-3.0-only (the §5 work-licence
point for the bundled JS/CSS). index.html 21,358→23,624 B (5,707 B gz).
Verified: gzipped body served with the new ETag "1AF1C259" contains all
four notices.

Open (not §5d): the SPA chrome still says MARQUEE. Branding pass is
Phase 12 territory.

## T-8.9 — Phase 8 close-out (2026-09-18)

Phase 8 done: async server (T-8.1), gzipped SPA (T-8.2), `/api/settings` +
`/api/system` (T-8.3), `/api/sports/*` + `/api/favorites` with enforced
one-TLS-session (T-8.4), `/api/widgets` with live reorder and the poll-side
config-copy fix (T-8.5), `/preview` BMP + show-ip splash (T-8.6), vendored
onboarding page (T-8.7), GPLv3 §5d notices (T-8.8).

The SPA runs against the firmware unmodified: dashboard preview, display
screen, sports picker, drag-reorder carousel, About — all served from the
device. Heap: full route table costs 2,464 B (was 480 B), server total
24.5 KB after begin, ~102 KB internal free / 61 KB largest block with
WiFi + panel + poll + web all live — inside the 20–40 KB web budget.

Deferred by build order, not by omission: `/api/net/connect` + captive
portal (T-9.1) and the structural-change restart banner's boot-accurate
snapshot (RAM-sticky flag matches Marquee).

CI: T-8.3..T-8.8 green on main (T-8.7 run cancelled-in-progress by the
T-8.8 push, its checks ran inside T-8.8's run).

## FIX — preview BMP magic was byte-swapped (2026-09-18)

Dashboard showed the broken-image icon. `curl -i /api/system/preview` →
**200**, byte-perfect except bytes 0–1: `put16(b+0, 0x424D)` is little-
endian, so the signature came out `4D 42` — "MB". Every other field
(bfSize, dims, offsets) was right, which is why the size matched and the
browser still refused it. The original T-8.6 proof printed
`magic b'MB'` — it was caught and waved through; the test asserted
everything except the two bytes that broke it.

Fix exactly as signature-as-characters:

```cpp
b[0] = 'B';
b[1] = 'M';
```

Not `put16(0x4D42)` — the trap must be impossible to re-spring. The
header writer (with `put16`/`put32`/`bmp_row`) moved to
`lib/web/bmp.h`, host-compilable; web.cpp calls `write_bmp_header`.

New host test `test_bmp_header` (native 37 → 38 cases): asserts
`b[0]=='B' && b[1]=='M'` and `bfSize == 54 + bmp_row(w)*h` for both the
live 1152×32 strip and a padding-width case (10×7, row 30→32). Exactly
the bug class eyeballing misses.

Live proof after reflash:

```text
magic b'BM'  bfSize 6966 == computed 6966 == actual 6966
```

## FIX — SPA said "Marquee" (2026-09-18)

Wrong project name in three places of assets/web/index.html: `<title>`,
the header span (now `NOSEBLEED`), the restart banner. Not a redesign —
just the rename the project carries. `grep -i marquee` on the source and
the served page: zero. index.html 23,630 B → 5,705 B gz, web partition
reflashed.

## T-9.3 — Arduino-as-ESP-IDF-component (2026-09-18)

Phase 9's structural prerequisite, no hardware. The firmware build of record is
now **ESP-IDF 5.5.5 with arduino-esp32 3.3.11 as a managed component**
(`main/idf_component.yml`); PlatformIO survives only as `env:native`.

**Toolchain.** ESP-IDF v5.5.5 installed at `~/esp/esp-idf-v5.5.5` — the exact
version the current Arduino prebuilts were built with
(`CONFIG_IDF_INIT_VERSION="5.5.5"` in the shipped sdkconfig), so kconfig
semantics match the proven build 1:1.

**What the conversion is** (build system only — no lib/ or src/ rewrites):
`CMakeLists.txt` (project + `pack_web` → `spiffs_web_bin` ordering + USB-CDC
global defines), `main/CMakeLists.txt` (registers `src/` + all `lib/` sources;
`NB_TARGET=` cache var picks one of the seven mains, replacing the six pio
test envs — `tools/idf_build.sh <env>` wraps build-dir/sdkconfig layering),
`sdkconfig.defaults` **regenerated from the Arduino core's own IDF-5.5.5
sdkconfig** so the IDF build starts from the exact configuration the pio build
ran, plus the qio_opi board overlay + the T-9.3 deltas. `AUTOSTART_ARDUINO=y`:
the component supplies `app_main` and links `-u _Z5setupv -u _Z4loopv` — the
Arduino entry-point pattern, `src/main.cpp` untouched.
Deps pinned to the same artifacts pio used: core 3.3.11 (same release tag),
ArduinoJson 7.4.3 (registry), ESPAsyncWebServer v3.12.1 / AsyncTCP v3.5.0
(git-pinned — the component registry lags the GitHub tags), HUB75 at
`cf09801` (git-pinned). `dependencies.lock` committed for the full graph.
StreamUtils 1.9.2 is header-only and its upstream `CMakeLists.txt` is a
host-test harness, so it is **vendored** into
`components/nb_streamutils` (md5-verified against the `v1.9.2` tag, MIT licence
carried). Retired: the pio esp32s3 + six test envs, the `pack_web.py`
extra_script (now an IDF custom target), `.pio` paths in `wokwi.toml` (now
`build/nosebleed-fw.*`).

**Builds (all from clean build dirs).** firmware + all six device-test targets
(logostest via `sdkconfig.logostest` for the debug layer) — **7/7 green**.
`pio test -e native` 38/38. Purity script green.

**Accept 1 — lib/render.** No framework include was added anywhere; the T-1.6
guard has no hole — the conversion itself demanded **zero** changes. One
one-line edit rides along, unrelated to the guard: `situation.h` count buffer
8→14 bytes because IDF's `-Werror=all` (it comes from the Arduino component's
own compile flags) turns gcc's `format-truncation` fatal on the theoretical
int16 full range. Values never change; goldens byte-identical.

**Accept 2 — mbedTLS (T-0.6).** They bite now, provably: `libmbedtls.a` is a
fresh **from-source** artifact (`build/esp-idf/mbedtls/mbedtls/library/`),
`sdkconfig.h` carries `MBEDTLS_SSL_IN_CONTENT_LEN=16384`,
`MBEDTLS_SSL_OUT_CONTENT_LEN=2048`, asymmetric=y, and `KEEP_PEER_CERTIFICATE`
absent. Static size A/B against a baseline build (same tree, Arduino's
symmetric/keep-cert settings): ±150 B — as expected, the ~14 KB recovery is
**runtime heap per handshake**, not flash. The 53 KB → ~39 KB re-measure
needs the board (or a WOKWI token for the heap-only half); boot log
(`sdkconfig_snapshot`) will show the effective values.

**Accept 3 — data cache (T-1.3).** `CONFIG_ESP32S3_DATA_CACHE_64KB=y` and
`LINE_64B=y` resolve to `ESP32S3_DATA_CACHE_SIZE=0x10000` — reachable from
source, logged at boot for runtime confirmation.

**Surprises.**
- HUB75's `Kconfig.projbuild` defaults `ESP32_HUB75_USE_GFX=y`, which steers
  its CMakeLists at component names (`arduino`, `Adafruit-GFX-Library`) that
  don't exist in component mode — FATAL_ERROR until pinned `=n`, which puts it
  on its esp_lcd/driver IDF path and exports `NO_GFX` itself. The pio build's
  `-DNO_GFX` flag is now redundant (verified via compile_commands: NO_GFX on
  both the driver sources and panel.cpp; driver .cpp builds in pure-IDF mode,
  and its headers have zero Arduino-conditional layout — no ODR seam).
- ESPAsyncWebServer v3.12.1 still ships the legacy `register_component()`
  CMakeLists and IDF 5.5.5 builds it fine.
- IDF 5.5's `ESPTOOLPY_FLASHMODE_QIO` deliberately maps the image header string
  to `"dio"` (bootloader limitation, comment in `esptool_py/Kconfig.projbuild`)
  — the T-0.1 "reports DIO" note stands in IDF too.
- The core puts `-Werror=all` on every component, which surfaced four genuine
  latent issues outside lib/render: `config.cpp` snprintf writing `w.id` from
  `w.league` in one call (gcc's restrict analysis — routed through a local),
  a `%s` fed an `int` in card_test's parse-fail print (was swallowing the real
  reason — now `err.c_str()`), and `%u` vs `uint32_t` in two test envs (xtensa
  `uint32_t` is `unsigned long`; Serial.printf never warned because pio didn't
  use `-Werror=format`). `maybe-uninitialized` from StreamUtils inlining stays
  a warning, not an error — project-wide `-Wno-error=maybe-uninitialized`, the
  only warning demotion.
- ESPAsyncWebServer's transitive deps forced namespace-exact git pins
  (`esp32async/*`); an unnamespaced alias downloads as a second parallel
  component and the manager refuses the ambiguity.

**Deferred to hardware (unchanged scope).** Runtime TLS-session heap
re-measure, data-cache runtime log, 53 KB → ~39 KB confirmation, and the two
open items (static-mode flicker capture, T-9.1/T-9.2 provisioning). Wokwi
heap-half of the TLS re-measure needs `WOKWI_CLI_TOKEN` — not set this session.

## T-9.1 + T-9.2 — SoftAP provisioning + NVS credentials (2026-09-19)

Built: `Creds` + validation and a separate NVS blob (ns `nb`, key `net` — never
inside the `Config` blob, so no GET path can carry a password); `lib/net/net.{h,cpp}`
supervision state machine (assoc window 10 s, backoff 1 s→30 s cap, ≥3 failures →
open SoftAP `nosebleed-<last-3-MAC-bytes>` + captive DNSServer, saved creds beat the
`secrets.h` built-in, re-probe every 5 min while the AP idles with no station,
dirty-reload on `/api/net/connect`); main.cpp task_net/edge wiring with AP-SSID and
IP splashes; web: `POST /api/net/connect` (values never logged, no GET counterpart)
and a captive `onNotFound` 302 → `/onboard` while the AP owns the radio.

**Acceptance, all on hardware:** erased-NVS boot → `[net] wifi target: none
(provisioning)` + AP up + `[web] up (nosebleed-7A8F20)`; host joined it;
`dig test.example.com @192.168.4.1` → 192.168.4.1 (captive DNS);
`curl -H "Host: connectivitycheck.gstatic.com" http://192.168.4.1/generate_204` →
302 → `/onboard`; GET `/onboard` byte-identical to `assets/web/onboard.html`
(gz-served); `POST /api/net/connect` → `{"ok":true,"ssid":"HomeAuto"}` → board left
AP, joined the network, AP down; chip reset → `[net] wifi target: saved
credentials` → up in 1 s (persistence, and saved-beats-built-in proven);
creds-cleared boot → `[net] wifi target: built-in (secrets.h)` → connect; POST
while linked → `[net] creds changed, restarting link` → reconnect inside 3 s;
password string absent from every serial capture of the battery and from all four
GET API payloads. Backoff→AP fallback proven live (bogus creds → attempts
1..3 logged → `AP nosebleed-7A8F20 open`).

**Five bugs, all found by running it, none visible in review:**
1. `__has_include("secrets.h")` is per-TU — net.cpp never saw the built-in
   credentials until it included the header itself.
2. `tick()` returned early while LINKED — the dirty flag was never seen, so a
   POST to a *connected* board saved creds and silently did nothing. (The first
   AP-mode test hid this: the board wasn't linked.)
3. The AP reprobe gate `softAPgetStationNum()==0` also gated the user's own
   submission — and the provisioner is by definition the attached station.
   One-shot `s_force` bypass for POSTs; the gate stays for unattended reprobes.
4. 30 s reprobe cadence scan-stormed the radio: the AP SSID went undetectable
   during probes (nmcli confirmed misses). Re-probe → 5 min.
5. `PUT /api/settings` blew the `async_tcp` stack canary (panic +
   reboot, decoded backtrace → `handle_settings_put`, web.cpp:80): two ~4 KB
   `Config` PODs on the 8 KB default `async_tcp` stack under the middleware
   chain — handlers have no executor hook in ESPAsyncWebServer v3. Dropped the
   full-Config copy (only `.hw` is compared) and raised
   `CONFIG_ASYNC_TCP_STACK_SIZE=12288`. Note the IDF trap this exposed:
   `sdkconfig.defaults` does not override an existing `sdkconfig` key — the
   value only landed after editing `build/sdkconfig`; new symbols ride on fresh
   sdkconfigs or manual edits.

**Environment, not firmware:** for ~1 h the board's link went on-paper-dead
(status CONNECTED, ARP alive, ~92 % ping loss, TLS `getaddrinfo` 202 /
`-0x7280` mid-stream). A git-stash control build of the *pre-provisioning* net
loop reproduced it identically, so the new state machine is exonerated; it was
RF/peer degradation on the test SSID (which itself has two APs). The residual
weakness is real and predates this phase: both old and new supervision key
exclusively on `WiFi.status()`, which can persist on a dead data path — logged
as PLAN §4a D-5.

User config wiped by the acceptance erases was restored via the API
(static, brightness 40, nfl/nhl/mlb) and verified across a reboot.

## T-9.6 — factory reset (2026-09-20)

Built: `config::reset()` widened from two named keys to a whole-namespace
`Preferences::clear()` on `nb` (covers creds and every future key — logo OTA
— with no edit; component namespaces like `nvs.net80211` are untouched);
`POST /api/system/factory-reset` with a `confirm=1` guard arg (anything else
400s with the resend hint); the physical path is the DevKitC-1 BOOT button
(GPIO0 — free per the T-2.8 pin map) held **5 s while running**, polled in
`loop()` at 100 ms, which was already idle at a 10 s tick. Web path defers
`esp_restart()` ~700 ms via a `set_reboot_hook` deadline (same pattern as
show-ip) because handlers run on `async_tcp` and blocking it would skip the
response flush. Firmware, `logos` and `web` partitions are never in scope.

**Acceptance, all on hardware (panel disconnected — see note):** guard → 400
(absent arg and `confirm=0` both rejected); confirmed POST → 200
`{"ok":true,"rebooting":true}` → serial shows `factory reset: NVS cleared` →
reboot → `cfg NOT_FOUND` / `net NOT_FOUND` → defaults reseeded
(brightness 80, scroll, mlb-only vs the live 40/static/3-league) → board
rejoins on the build-time fallback, proving the saved-credentials blob died
with the rest; BOOT 5 s hold → `[reset] BOOT held 5 s: NVS…` → same wipe.
Rebuilt **without** gitignored `secrets.h` (the shipping shape): the
post-reset state comes up as **`[net] AP nosebleed-7A8F20 open`** — portal
serves SPA (5,705 B) and `/onboard`, re-provision over the AP joins the
network, factory-reset over LAN returns it to the AP again. `logos` FOUND at
0x810000 with no `partition missed` and 16 game cards rebuilt across the
whole battery; user config restored via API afterwards.

**Panel-less render note (ribbon-loose state):** with the ribbon off,
`panel::init` still succeeds (the DMA path doesn't detect the load), render
held **30.3 fps, worst steady-state gap 34 ms** through fetches and strip
rebuilds, and nothing anywhere errors — a detached panel is a dark panel,
not a degraded one. The T-7.6 "hitch, never blank" contract is unchanged by
this task; full verification still wants eyes on real glass.

**Traps.** (1) GCC depfiles do not track headers probed via
`__has_include("secrets.h")` — restoring the file did not retrigger
net.cpp/main.cpp; `touch` the includers after toggling secrets or you will
flash a stale binary (happened twice before I checked strings on the .bin).
(2) `pkill`-ing the serial logger before `idf.py flash` — esptool cannot win
the port. (3) The confirmed-POST client may see a connection reset instead of
the JSON if the restart wins the flush race; the serial record, not the curl
exit code, is the evidence (700 ms makes it rare, not impossible).

## T-9.4 — firmware OTA (hardware-verified 2026-09-20)

`PUT /api/update`, raw body via `AsyncWebHandler::handleBody` → arduino
`Update` lib (`begin(total, U_FLASH)` / `write` / `end(true)`), deferred
reboot through the existing T-9.6 hook. Good-image round-trip on the bench:
`{"ok":true,"rebooting":true}` [200], boot flips slots, `[ota] running from
appX, state=N` on every boot, USB flashing untouched (GPLv3 §6).

**The rollback story is not the PLAN's story.** `CONFIG_BOOTLOADER_APP_ROLLBACK_
ENABLE` and the `PENDING_VERIFY → mark-valid/ABORTED` dance are live in
`sdkconfig`, and a hand-written otadata entry (esptool, `state=NEW`) proves
the *bootloader* half works — it converts, excludes and falls back exactly
as `bootloader_utility.c` says. But the **app-side staging never enters the
machine here**: a raw otadata dump at the top of `setup()` (TEMP debug build)
showed the staged slot already `VALID` at boot#1 (`entry1 seq=2 state=02`),
so a bad image crash-loops every bootloader pass (`abort() was called at PC
0x4200efd2` ×N). Whatever in the app_update→otadata write path produces that
on this toolchain is still unexplained and out of T-9.4's scope; the
mechanism was measured broken on-device, so nothing here depends on it.

**What ships instead:** `NbBootGuard` in `main.cpp` — an `RTC_NOINIT_ATTR`
counter keyed to the running image's ELF-sha256 (a fresh flash can never
inherit an armed counter). Armed at the top of `setup()`, disarmed at the
bottom; 3 consecutive boots of the same image without reaching the bottom
call `esp_ota_mark_app_invalid_rollback_and_reboot()`, and the bootloader —
which does exclude INVALID entries, that half works — falls back. Verified
end-to-end: bad image (same tree + `abort()`) crash-looped twice, on the 3rd
boot printed `[ota] 3rd consecutive boot without completing setup — rolling
back`, and the board returned `running from app0, state=2` fully healthy at
30.3 fps with the panel still disconnected.

**Traps.** (1) `canHandle` matching on the **bare** `HTTP_PUT` compiles but
resolves to the global `http_parser` `enum http_method` (value 4), not the
`AsyncWebRequestMethod` bit the request reports — the 404/204 flapping before
was this, not the server; match on the namespaced constant. (2) `Update.begin`
returns false after an aborted upload (`already running`) — `Update.abort()`
before `begin` (its `_reset` clears `_size`; `begin` clears the sticky
`UPDATE_ERROR_ABORT`), so retry works without a reboot. (3) curl's default
`Expect: 100-continue` handshake against ESPAsyncWebServer: send
`-H "Expect:"` explicitly for scripted uploads; (4) the board takes ~12 s to
answer HTTP after a reset — a "dead" board in a serial capture at t<12 s is
the boot, not a fault.

## T-9.5 — logo OTA (hardware-verified 2026-09-20)

`POST /api/logos/update url=<https…>` stores the atlas URL (NVS "nb",
factory-reset-clearable) and flags the poll task; the poll task fetches
(boot + daily + on demand — keeping every TLS session single-threaded) into
a 2 MB PSRAM buffer, validates the whole atlas (magic/version, every index
row's blob inside the downloaded bytes) **before touching flash**, erases
the `logos` partition and writes the blob in chunked, yielded ops,
re-mmaps, and `memcmp`s the mapped bytes against the buffer. Success forces
a strip rebuild (`g_strip_key = 0`), so new art is on the panel within one
poll pass. A mid-write failure forfeits the atlas to the T-3.6 abbreviation
fallback until the next successful check — deliberate: erase happens only
after full validation, and there is no second copy of the atlas in flash.

Verified on the bench against a real HTTPS host (scratch GitHub repo →
raw.githubusercontent): **update** — a structurally-valid modified atlas
(inverted MLB h=13 blobs) downloaded, swapped, and the `/preview` strip
diffed in 612 bytes; **no-op** — re-trigger on the same source came back
via **304 in 0.5 s** against a 38 s cold download (git-blob ETag, quoted
64-hex); **self-heal** — same-URL trigger keeps the stored ETag (a fresh
URL clears it, a re-saved one must not — first cut cleared it every time,
killing the 304 forever); restore round-trip ~7 s.

**Traps.** (1) The response ETag needs `cfg.event_handler` +
`HTTP_EVENT_ON_HEADER` — `esp_http_client_get_header()` reads *request*
headers only. (2) GitHub's ETag is 66 chars; the first `char etag[64]`
truncated it — the resend then never matched, silently defeating 304 (the
compare-200-body fallback still caught it, so the verdict stayed right
while the bandwidth doubled: keep the field ≥ 80). (3) raw.githubusercontent
caches a blob for ~5 min, so a repush can serve a stale ETag — a 304 right
after a real atlas push is the CDN, not the client. (4) The scratch test
repo `Omnicef/logos-test` is still up — `gh repo delete` needs the
delete_repo scope; delete by hand. *(Deleted by hand 2026-09-21 — 404 +
GraphQL "could not resolve" confirm it's gone; the raw-URL configured on
the device was only ever a test fixture.)*

---

## Session start (2026-09-20) — Phase 10

Scope: T-10.1 … T-10.6 (weather, ticker, scheduling). Panel DISCONNECTED —
render-path acceptance is host-side (PNGs/goldens); on-panel confirmation
defers to the bench. Correction found mid-T-10.1 and confirmed against the
source: the Marquee repo IS on this box (`/home/anthony/VSCode/Marquee`,
only `$MARQUEE_REPO` was unset) — but its weather and ticker widgets AND
its data clients are all **Phase-4 placeholders, never implemented**
(`__all__ = []` stubs). There is no Python layout to port; PLAN's "port
the Python's layout" was aspirational. Owner was (correctly, it turns out)
asked and authorised a **fresh minimal design** for both widgets and the
provider trio **Open-Meteo** (keyless — matching the stub's stated intent
"Defaults to Open-Meteo (keyless)") / **GNews** + **Finnhub** (keys →
NVS, never GET, never logged) / **CoinGecko** (keyless). The ticker stub
named "ESPN news headlines, stock quotes, crypto prices" — ESPN headlines
are the keyless alternative for the news leg if the GNews key ever stings.
Every API shape was captured live (or from the vendor docs for the two
keyed ones) — not from memory.

## D-1 — frozen clock on period/clock cards (fixed before Phase 10)

`cards_key` hashed `period`/`clock`/`status_display` only inside
`if (has_situation)` — MLB/NFL kept a live-looking key while NHL/NBA/soccer
cards drew a clock that only moved when something else changed the key.
All three now hash for every game; the situation fields stay gated. A live
game with a ticking clock rebuilds the strip once per poll (20 s live
cadence) — rebuild is core-0, the render task never blinks; per PLAN the
alternative is "a wrong clock", which is worse. The old host test *pinned
the bug* ("clock tick changed cards_key" asserted stable) — inverted.

## T-10.1 — weather client (host-verified; firmware builds)

Open-Meteo `/v1/forecast`, live-captured shape 2026-09-20:
`current.{temperature_2m,weather_code}` + `current_units.temperature_2m`
("°F"/"°C" — UTF-8, take the LAST char) + `daily.{max,min}[0]`,
`forecast_days=1`, `timezone=auto`, unit via `temperature_unit=`.
Nesting depth 3 — checked per the AGENTS rule; `NestingLimit(20)` kept
anyway. Split like ESPN: `weather_json.*` pure (filter, WMO→text table,
`to_weather`, `weather_url`), `weather.cpp` the ARDUINO-only transport
(tls_take → http_get → ReadBufferingStream, same chain as espn.cpp).

* `InfoCache` (`lib/data/info_cache.h`): a `SeqSlot<T>` copy of the
  proven DataCache protocol — deliberately not a refactor of
  hardware-verified code. One real bug found by the new stress test:
  `writable()` without a following `publish()` (an abandoned fill) flips
  the seqlock parity, so the NEXT refill reads as stable mid-fill.
  `writable()` now always lands odd. (DataCache carries the same latent
  quirk — its callers uphold writable⇒publish by discipline, main.cpp
  calls writable only after a successful fetch; left untouched.)
* Poll task: `InfoScheduler` (PollScheduler templatized on N — leagues
  keep `PollScheduler`, four info slots get `InfoScheduler`) runs the
  weather slot on the same task/clock: 900 s cadence, armed only when the
  weather widget is enabled AND lat/lon are set; fetch lands in a local
  `Weather` and the slot is touched only on success ⇒ last-good by
  construction. Sleep = min(league wake, info wake).
* `weather_url` is an injection guard, not a formatter: `[0-9+.-]` max
  15 chars with ≥1 digit or nothing is built (`39.9&key=`-style values
  are tested red).
* Config schema 1→2 (one bump now so Phase 10 never re-wipes NVS):
  `Services` block (weather lat/lon/unit now; ticker lists and quiet
  hours fields declared for T-10.3/10.5) + a disabled `weather` widget
  row by default. `/api/settings` gained `weather_lat/lon/imperial` —
  stored verbatim, re-validated at URL-build time, junk = feature off.
* Host: decode of the live payload (72/75/66 °F, "Overcast"), filter
  drops `timezone`/`generationtime_ms`, degraded payload → false, URL
  guard, cache last-good + 50 000-snapshot writer stress. 40/40 native;
  purity OK; `idf_build.sh firmware build` green.

**Traps.** (1) `std::atomic<uint32_t>::store(memory_order_acq_rel)` is an
*invalid* order (only RMWs take acq_rel) — libstdc++ asserts it only
under assertions/TSan; a plain build silently misorders. The stress test
found this, but the first red was my own test: the pre-thread fixture
publish legitimately satisfies "last-good", so its values must obey the
same-generation invariant the thread checks.

## T-10.2 — weather widget (fresh design, pixel-parity golden)

Marquee's `weather.py`/`ticker.py` are `__all__ = []` stubs — the first
widgets with no Python pixel parity to prove against. Layout: cond
(5x8, white, y=1, truncated to 12 glyphs — "Freezing drizzle" (16) is the
longest WMO string and clips at 13), temp (6x12, y=9) + degree ring + unit
(5x8, y=13), H/L (5x8, gray, y=24). No '°' exists in the baked ASCII 32–126
glyph tables, so it's drawn with `ellipse(r=2)` — the pixel-proven
Pillow-parity special case; the golden draws the identical PIL
`draw.ellipse`. Generator `tools/gen_weather_golden.py`, golden + parity
test `test_weather_widget` (also: hidden-before-fetch, hidden-when-disabled
— which caught cards() ignoring `enabled`, a ScoreboardWidget invariant the
new widget didn't share — and key-moves-on-temp-change). Wired into the
producer set: `kMaxProducers = leagues + 5` sized once for the T-10.4
tickers. 41/41 native; purity OK; firmware green (`hl[16]` → `hl[24]` for
the int16-min H/L strings under -Werror=format-truncation).

## T-10.3 — ticker sources: GNews / Finnhub / CoinGecko

Data layer split like weather: `ticker.h` (display-ready POD — 12-glyph
fields = the card width, so the widget can't clip), `ticker_json.*` pure
decoders + URL guards, ARDUINO tail inside the same file (one
`fetch_filtered` chain). CoinGecko shape **live-captured** (incl. its
`4.0e-06` micro-cap → `%g`); `/everything` 404s — the live path is
`top-headlines`, which a bogus key answers `{"errors":[…]}` 400. GNews and
Finnhub success shapes are **vendor-docs-only fixtures** (a real capture
needs the owner's keys) — flagged here deliberately; the decoders must be
re-verified the day a key goes in.

* `InfoCache` grew three `SeqSlot<TickerList>` (news/stocks/crypto).
  Independence is structural: every slot publishes only after its own
  fetch succeeds; a `c:0` Finnhub symbol, a dead feed or a keyless source
  skips its own publish and leaves its last-good list (host-tested).
* Poll: `widget_row_enabled(cfg,type)` gates ALL info polls now — including
  weather, whose gate was "lat set": configuring a location while the card
  is hidden no longer keeps a third party polled forever. Stocks run
  sequential per-symbol GETs (≤4) inside the poll task — still one TLS
  session at a time. `stocks_fetch` partial success keeps the good symbols.
* `ApiKeys` NVS blob "keys" with the T-9.2 Creds discipline: `POST
  /api/keys` is the sole writer, `GET /api/settings` answers presence
  flags only, nothing prints values, `api_keys_valid` allowlists
  `[A-Za-z0-9._-]` (the keys ride in query strings — the charset is an
  injection guard first, hygiene second). URL builders re-check anyway.
* Schema 2→3 (three disabled default rows: news/stocks/crypto; ≤4 stock
  default to match the per-cycle request cap). SPA settings screen grew
  weather + ticker fields and a keys form (blank-clears; values never
  round-trip).
* Traps: `DeserializationError::c_str()` on success is "Ok" not "";
  gcc `-Werror=format-truncation` audits `%.0f`-into-13 (raw[16]→raw[13]).
* 42/42 native (new: live CoinGecko decode incl. comma format + micro-cap,
  doc-shape news wrap/finnhub fallback + c:0 reject, 12 URL-guard cases,
  key charset, slot isolation); purity OK; firmware green.
