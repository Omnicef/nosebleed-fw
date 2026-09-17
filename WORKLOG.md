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
