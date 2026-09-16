# PLAN.md — nosebleed-fw

ESP32-S3 firmware rewrite of the `Marquee` Python project. Phased build plan with numbered tasks.

**Read `AGENTS.md` first.** It carries the hard rules, memory budget and task topology that every phase below assumes.

> **On `Marquee`.** Nosebleed is a rewrite of a working Raspberry Pi implementation in Python, referred to throughout as *Marquee* or *"the Python"*. **That repo is private and not publicly available** — it is a design reference for the author and for agents working in this repo, not a dependency. Nothing here requires it: the ported card layouts, fonts and test fixtures are all committed. References to it are historical provenance, and can be read as "this decision came from a working prior implementation".
>
> Agents with local access: set `$MARQUEE_REPO` to its checkout path. It is **READ-ONLY**.

**Reference repo:** the Python `Marquee` project at `bcccc4f` (Phase 5 complete). Referred to throughout as *"the Python."* Its `PLAN.md`, `CARD_DESIGN_SPEC.md`, `docs/API_NOTES.md`, `tests/fixtures/` and `marquee/matrix/fonts/*.bdf` are inputs to this build.

**Rule:** do phases in order. Each de-risks the next. Phase 0 is a gate — if it fails, the project stops.

---

## §1 Stack decisions and why

| Decision | Choice | Rationale |
|---|---|---|
| Framework | Arduino-ESP32 via **pioarduino**, → Arduino-as-IDF-component at Phase 9 | Pure IDF is disqualified: **cJSON has no filtering**, and filtering is the only thing that makes a 1.46 MB response tractable. Arduino gets us ArduinoJson + the HUB75 library. IDF features (`esp_partition_mmap`, `esp_ota`, mbedTLS `menuconfig`) are needed later, so architect for the conversion from day one. |
| Platform source | `pioarduino/platform-espressif32` | Upstream PlatformIO `espressif32` stalled. pioarduino tracks Arduino 3.3.x / IDF 5.5.x and is what Tasmota builds on. |
| Panel driver | `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA` | Adafruit_GFX-compatible, DMA off the S3 LCD peripheral, chaining supported. No realistic alternative. |
| JSON | ArduinoJson v7, `DeserializationOption::Filter` | The single most important library choice in the project. See §2 and T-0.5. |
| Web server | `ESP32Async/ESPAsyncWebServer` v3.x | Maintained fork; the `me-no-dev` original is abandoned. `PsychicHttp` (ESPAsyncWebServer-shaped API over `esp_http_server`) is the fallback if stability bites — `esp_http_server` also runs ~8 KB lighter on heap. |
| Config store | NVS | Transactional with wear-levelling. Replaces ~120 lines of SQLite WAL + `config.json` snapshot + corruption recovery with about 40. |
| Fonts | Fixed-width bitmap blitter, **not** Adafruit GFX fonts | See T-2.4. Preserves pixel parity with the Python, which preserves every layout constant. |
| Logos | Build-time Pillow pipeline → `logos.bin` → `esp_partition_mmap` | See §3 and Phase 3. |

### Deliberately not doing

- **No on-device image processing.** No PNG decode, no LANCZOS, no unsharp mask. That work moves to a laptop.
- **No proportional fonts.** See T-2.4.
- **No SQLite, no filesystem-backed config.** NVS only.
- **No certificate pinning.** `esp_crt_bundle`.
- **No concurrent TLS sessions.**

---

## §2 ESPN payload — the risk that evaporated

**Superseded by measurement at T-5.6. Kept because the reasoning shaped the architecture.**

The plan was built around a 1,457,268 B MLB fixture (15 events, ~97 KB each) captured from a
`dates=yesterday-tomorrow` request. That framing drove the gate, the filter, the buffering and the whole of
Phase 0. Two things turned out to be false.

**ESPN no longer serves multi-day windows.** `dates=YYYYMMDD-YYYYMMDD` and comma-separated lists both return
**400** (verified on-device, T-5.6). Only single days work — and the default window is already one day. So the
1.46 MB three-day payload cannot be requested any more, by us or by anyone.

**The live single-day payload is ~298 KB**, five times smaller than the fixture the entire risk analysis assumed.

**And the parse was never the bottleneck.** A filtered parse of 298 KB costs **90 ms**, identical across every
allocator arrangement. The 6,081 ms that made T-0.5 read as an at-budget near-miss was **almost entirely wire
time**, which swings 1.35–6.7 s on identically-sized payloads. Best case measured 2.8 s, comfortably under the
6 s budget.

What survives from the original analysis:

- **Filtering is still right.** ~300 B retained per game out of 298 KB, and it costs 90 ms. No reason to stop.
- **`ReadBufferingStream` is still right.** Byte-at-a-time reads off TLS remain slow regardless of payload size.
- **Sequential polls, one TLS session** — unchanged, driven by memory not payload.
- **`NestingLimit(20)`** — unchanged and still mandatory.

What changed:

- **T-5.6 is no longer "narrow the window".** There is nothing to narrow. It became: single-day is the only
  option ESPN offers, and yesterday's finals require a **separate single-day fetch**, merged by
  `filter_yesterday_today`.
- **T-11.3 (gzip) is now conditional on wire time only** — reach for it if on-site transport stays above 4 s,
  not because of payload size.
- **The date-window mitigation is gone from the risk register.** Transport variance replaces it as the timing
  risk, and it is a network property we cannot optimise away.

> **Note for the Python predecessor:** Marquee issues exactly the `dates=yesterday-tomorrow` request ESPN now
> rejects. It is broken upstream and needs the same single-day + separate-yesterday treatment.

---

## §3 Logo asset format

```
logos.bin
├── header    magic "NBLG", version u16, count u16, logo_height u16, reserved
├── index[]   { league[8], abbr[4], offset u32, w u16, h u16, rsvd u16 }   22 B exactly
└── blobs     RGB565 pixels (w*h*2 B) + 1-bit alpha mask (w*h/8 B)
```

Worst case at 32 px (a full 32×32 square): 2048 B colour + 128 B mask = 2176 B. **Measured average is ~1.8 KB**
across the 50-logo corpus at T-3.1 — most marks are narrower than they are tall, so after the `getbbox()` trim and
resize-to-height the width is usually under 32.

| Scope | Teams | Projected @ 2.1 KB | **Measured @ ~1.8 KB** |
|---|---|---|---|
| NFL + NBA + MLB + NHL + EPL | 144 | ~306 KB | **~260 KB** |
| + college FB, MCBB, WCBB | ~1,000 | ~2.08 MB | ~1.8 MB |

Comfortably inside the 2 MB `logos` partition either way.

Mapped with `esp_partition_mmap()` and read directly — no decode, no RAM copy, no warm/cold cache distinction. Flash reads go through the cache, so a miss costs a real SPI read: **keep logo access in strip rebuilds, never in the per-frame path.**

> **Index stride is 22 B, and there is no padding.** 8 + 4 + 4 + 2 + 2 + 2 = 22, packed — `struct "<8s4sIHHH"` on the writer, `kEntrySize = 22` on the reader. This spec originally said `w u8, h u8` and "~20 B each"; T-3.4 hit the consequence — an assumed stride desynced **20 lookups**, silently returning the wrong logo rather than erroring. Derive the stride from the format struct on both sides and assert they agree; never hand-count it, and do not "optimise" w/h back to u8 on the assumption alignment will absorb it.
> **Heap cost: lookups are free, the mapping is not.** Measured at T-3.4 — per-lookup heap delta is **0 B in, 0 B out**, exactly as the architecture assumes. The one-time `esp_partition_mmap()` call itself costs **~104 B** of page-table overhead. Budget it once at init; it is not a per-access cost.
> **The atlas drifts.** At T-3.3, 46 of 47 keys shared with the Marquee corpus encoded byte-identical; `mlb:ATH` differed because ESPN changed the artwork upstream. Expect this — it is the reason T-9.5 makes `logos.bin` separately OTA-able.

---

## §4 Testing layers — what can and cannot be simulated

Three layers, in decreasing order of how much code they cover.

| Layer | Tool | Covers | Task |
|---|---|---|---|
| Render | `pio test -e native` | All 10 widgets, layout, fonts, strip builder, scroll arithmetic — pixel-exact PNG | T-2.6, T-2.7, T-2.10 |
| Logic | Wokwi | WiFi, TLS, filtered JSON, NVS, partition table, FreeRTOS task structure | T-1.8 |
| Reality | Hardware | Panel output, **all timing**, thermal, soak | T-0.2, T-0.5, T-11.2 |

**The render layer is the important one.** It is the majority of the code and all of the code you iterate on, which is
the entire reason for the `lib/render/` purity rule (T-1.6).

### Wokwi — what it actually gives us

Simulates the ESP32-S3-DevKitC-1 with configurable `flashSize`, `psramSize` and `psramType` (`"octal"` is supported, so
our N16R8 is reproducible exactly). WiFi, DMA, AES/SHA/RSA accelerators (real TLS works), custom `partitions.csv`, NVS
and GDB all work. CLI and GitHub Action available for headless CI.

### Wokwi — two hard limits ⚠

1. **No HUB75 output.** I2S is unimplemented on the S3 in Wokwi and there is no HUB75 part. The panel cannot be
   simulated at all. There is no RGBMatrixEmulator equivalent for this platform — hence T-2.10.
2. **Timing is meaningless.** Wokwi caps the simulated CPU near 8 MHz by default for speed. `cpuFrequency: "max"` exists
   but is still not real-time. **Never** use Wokwi to evaluate T-0.5's 6-second budget or the 30 fps render target.
   Heap figures are reasonably faithful; wall-clock figures are not.

### Working without hardware

The board and adapter are not required to make real progress. Most of this project is laptop work.

| Task | Without hardware | Note |
|---|---|---|
| T-0.1 toolchain, PSRAM/flash detect | ✅ Wokwi | `diagram.json` already configures the exact N16R8 |
| T-0.2 panel hello world | ❌ **blocked** | No HUB75 in Wokwi. Hardware only. |
| T-0.3 WiFi + SNTP | ✅ Wokwi | Both simulated |
| T-0.4 TLS smoke test | ✅ Wokwi | Real TLS — AES/SHA accelerators emulated |
| **T-0.5 the gate** | ⚠️ **half** | **Heap: yes. Timing: no.** See below. |
| Phase 1 | ✅ all | Skeleton, partitions, tasks, CI, licence audit |
| Phase 2 except T-2.8, T-2.9 | ✅ | Canvas, primitives, fonts, host harness, T-2.7 parity proof |
| Phase 3 | ✅ | `build_logos.py` and the atlas are pure host Python |
| Phases 5–7 | ✅ mostly | Data layer, all 10 widgets, scroll engine — host tests and fixtures |

**Splitting the gate.** Wokwi emulates real memory, so **peak heap during the filtered MLB fetch is meaningful
today**. Wall-clock is not — the simulated CPU is capped near 8 MHz (§4). Run the memory half now: if the filtered
parse blows past ~50 KB that is an architecture problem, and it is the half you cannot fix by waiting. The timing
half has four known mitigations in §2 and is re-run on hardware.

**Record a split verdict in `SPIKE_RESULTS.md`** — heap measured in Wokwi, timing pending — and treat the gate as
open until hardware closes it. Do not let a green heap number read as a full pass.

**Deferred set, to run the day the board arrives:** T-0.2, T-0.5 timing, T-2.8, T-2.9, and the on-device halves of
Phases 8–11.

**Caveat.** Wokwi's WiFi routes through their gateway. If a 1.46 MB fetch stalls or errors, suspect the simulator
before your design and re-test on hardware rather than redesigning around it.

---

### QEMU

Espressif's QEMU fork supports the ESP32-S3 via `idf.py qemu`, with a virtual framebuffer and eFuse emulation — useful
for exercising secure boot and OTA logic without bricking hardware. It wants an ESP-IDF project, so it only becomes
convenient after the Phase 9 conversion, and it does not model HUB75 either. Revisit at T-9.4.

---

## Phase 0 — Spike (GO / NO-GO gate)

Nothing here is production code. The goal is one number: peak heap during a filtered MLB fetch. Throw it all away afterwards if you like.

**T-0.1 — Toolchain up.** PlatformIO with `pioarduino/platform-espressif32`. Blink an LED.

> ⚠️ **Board ID matters and the obvious one is wrong.** Use `board = esp32-s3-devkitc1-n16r8`.
> The base `esp32-s3-devkitc-1` is an **N8 part with no PSRAM**, and pioarduino **ignores `board_build.flash_size`** (it reads `upload.flash_size`), so hand overrides silently yield an 8 MB, no-PSRAM build that appears to work until PSRAM allocation fails much later. Verified empirically at T-0.1 — the correct ID resolves to variant *ESP32-S3-DevKitC-1-N16R8V (16 MB Flash Quad, 8 MB PSRAM Octal)* with a 16 MB partition layout. Set `board_build.arduino.memory_type = qio_opi` as well.

*Accept:* **the running firmware prints its own evidence** — flash 16777216 B, PSRAM 8388608 B, `psramFound()` true. A correct build config is not sufficient; the boot log is the acceptance criterion.
*Note:* the platform may report flash mode **DIO** rather than QIO. Record it and move on — it is a flash read-bandwidth question only, not correctness. If mmap'd logo reads feel slow at T-3.4, revisit it there.

**T-0.2 — Panel hello world.** `ESP32-HUB75-MatrixPanel-DMA`, single 64×32, solid fills and a gradient.
*Accept:* no ghosting, no tearing, stable colour. Record measured refresh at `lsbMsbTransitionBit` 0 and 1 (expect ≈57 Hz and ≈110 Hz). Confirm 74HCT245 adapter works; if colours are unstable, this is a wiring/level-shift problem, not software.

**T-0.3 — WiFi + SNTP.** Connect, sync time, print local time.
*Accept:* wall-clock time correct within a second, obtained **before** any TLS attempt.

**T-0.4 — TLS smoke test.** `esp_http_client` + `esp_crt_bundle`, GET the NFL scoreboard (small).
*Accept:* HTTP 200, byte count matches `Content-Length`, no cert error. Log free heap before/after.

**T-0.5 — THE GATE: filtered MLB fetch.** GET the MLB scoreboard with an ArduinoJson `Filter` document. Parse straight off the stream, wrapped in `ReadBufferingStream` (StreamUtils, 512 B chunks) — unbuffered byte-at-a-time reads will make this gate fail for the wrong reason. Extract into a temporary `Game[]`.

> ⚠️ **Measure internal heap only.** `esp_get_minimum_free_heap_size()` is **wrong for this measurement** — it tracks the combined internal + PSRAM heap, so 8 MB of PSRAM masks the internal dip entirely (it returned ~8.5 M at T-0.4). Use the `MALLOC_CAP_INTERNAL`-scoped heap-caps API, or sample `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` at phase boundaries — pre-fetch, post-handshake, mid-parse, post-cleanup. Verify the exact signature against the installed header before use.

Log the internal-heap phase samples and elapsed ms.
*Accept:* **report the measurement decomposed — a single total is not a verdict.**

| Phase delta | What it measures | Expectation |
|---|---|---|
| pre-fetch → post-handshake | mbedTLS session | ~53 KB untuned, 26–36 KB after T-0.6. **Not what this gate is about.** |
| post-handshake → parse low-water | **the filtered parse itself — THE GATE** | small: stream buffer + ArduinoJson scratch, not tens of KB |
| PSRAM delta across the parse | retained `JsonDocument` | ~4.5 KB from a 1.46 MB input; ≤ 8 KB per T-5.4 |
| post-cleanup vs pre-fetch | leak check | < ~1 KB residual |

The `JsonDocument` lives in PSRAM via `SpiRamAllocator`, so a correct filtered parse costs almost nothing in **internal** heap. If the internal delta across the parse phase is large, the filter is not doing its job — that is the failure this gate exists to catch. A 53 KB total that is 53 KB of TLS and ~0 of parse is a **pass**, not a near-miss; TLS is a separate budget line with a known fix at T-0.6.
Games parse correctly — spot-check scores and team abbreviations against the fixture.
*Timing:* elapsed under ~6 s, **on hardware only**. Meaningless under Wokwi's CPU cap.
*If this fails, stop and reassess. Do not proceed to Phase 1.*

> **Result (Wokwi, conditional pass).** Filtered parse cost **1.6 KB internal**; retained document **5,676 B raw / 10.5 KB in PSRAM** from a 1.46 MB input; leak 0.5 KB; spot-check 6/6 against the fixture. Filter efficacy is **proven**.
> **What this run did NOT test.** ESPN blocks the Wokwi gateway and a raw 1.46 MB flash embed exceeds Wokwi's image limit, so the payload was zlib-embedded and inflated into PSRAM, then parsed **from memory rather than off a TLS stream**. Three things therefore remain unmeasured: `ReadBufferingStream` was never exercised; the **peak simultaneous** internal usage of an mbedTLS session and an active parse is unknown (they were measured separately, at T-0.4 and here); and there was no network backpressure. Close all three on hardware before treating Phase 0 as done.
> **Finding — `NestingLimit(20)` is mandatory.** The MLB payload nests to depth 15; ArduinoJson's default is 10, so the parse dies with `TooDeep` without it. Carried into T-5.4 and `docs/arduinojson-v7.md` §7.

> **ESPN blocks the Wokwi Public Gateway.** Measured at T-0.4: HTTPS handshake and `esp_crt_bundle` validation both succeed against ESPN's real certificate, but the CDN returns **403 with a 442-byte body** regardless of User-Agent. So T-0.5 cannot draw a real payload through Wokwi.
> **Workaround for the memory half:** publish `mlb_scoreboard.json` (1.4 MB, from the Marquee fixtures) to a public gist or public repo and fetch it from `raw.githubusercontent.com` — valid cert in the Mozilla bundle, real TLS, real 1.4 MB stream, real filtered parse. That measures peak internal heap under exactly the load that matters. It does **not** measure ESPN's own headers, chunking or throughput; those stay deferred to hardware along with timing.

**T-0.6 — mbedTLS tuning.** Apply `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` with `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048`, and disable *"keep peer certificate after handshake"*. Re-run T-0.5.
*Accept:* ~14 KB recovered. **Note:** `MBEDTLS_SSL_MAX_CONTENT_LEN` is widely reported not to take effect on the Arduino core — if these settings don't bite, that is the known issue and a reason to bring the Phase 9 IDF conversion forward.

**T-0.7 — Write `SPIKE_RESULTS.md`.** Record every measurement: heap, timings, refresh rates, PSRAM figures. Make and record the go/no-go call.
*Accept:* a committed document with real numbers, not estimates.

---

## Phase 1 — Skeleton

**T-1.1 — Repo init.** `.gitignore` (`.pio/`, `secrets.h`, `test/out/`), `platformio.ini` with two envs: `esp32s3` and `native`.
*Accept:* both envs build an empty program.

**T-1.2 — `partitions.csv`.** Two app slots for OTA, plus data partitions.
```
nvs       data nvs      0x9000   0x5000
otadata   data ota      0xe000   0x2000
app0      app  ota_0    0x10000  0x400000
app1      app  ota_1   0x410000  0x400000
logos     data 0x40    0x810000  0x200000
web       data spiffs  0xa10000  0x100000
```

> **Corrected at T-1.2.** This table originally gave `nvs` 0x6000, putting `otadata` at 0xf000 — and 0xf000 + 0x2000 =
> 0x11000, which **overlaps `app0` at 0x10000**. `nvs` is now 0x5000 (20 KB), which is ample: T-4.1 `static_assert`s the
> whole config under 4 KB. Verified on hardware — `esp_partition_find_first` locates `logos` at 0x810000 and `web` at
> 0xa10000.

*Accept:* flashes and boots; `esp_partition_find` locates `logos` and `web`.

**T-1.3 — Build configuration.** `sdkconfig.defaults` / `build_flags`: PSRAM enabled octal, mbedTLS settings from T-0.6, C++17. **Not** `CONFIG_ESP32S3_DATA_CACHE_64KB` — every Arduino-ESP32 prebuilt ships a 32 KB data cache and the setting is unreachable until the T-9.3 IDF conversion, the same class of limitation as the mbedTLS options at T-0.6. Log the actual cache size at boot rather than asserting it in a config file nobody reads.
*Accept:* settings verified present at runtime, not just in the file.

**T-1.4 — Directory skeleton.** Create the tree from `AGENTS.md` with placeholder headers.
*Accept:* both envs still build.

**T-1.5 — Task topology.** Create the four tasks with the exact cores, priorities and stack sizes in `AGENTS.md`. Each logs a heartbeat with its core ID and stack high-water mark.
*Accept:* log confirms `render` on core 1 and the other three on core 0. High-water marks leave >25% headroom.

**T-1.6 — Purity guard.** `tools/check_render_purity.sh` (run in CI and as a PlatformIO pre-action) greps `lib/render/` for `Arduino.h`, `esp_*.h`, `freertos/`, `WiFi.h`, `nvs*.h` and the Arduino `String` type, and fails the build on a hit. The script exists already and no-ops until `lib/render/` appears.
*Accept:* passes clean; deliberately adding `#include <Arduino.h>` to a render file fails the build.

**T-1.7 — Agent tooling.** Generate `compile_commands.json` via `pio run -t compiledb` so clangd can resolve include paths; without it LSP diagnostics on this project are noise. Add `opencode.json` with `"lsp": true` and `instructions` pointing at `docs/arduinojson-v7.md`. Regenerate compiledb whenever a library is added.
*Accept:* clangd resolves `ArduinoJson.h` and flags a deliberately wrong API call.

**T-1.8 — Wokwi simulation harness.** Add `wokwi.toml` (pointing at `.pio/build/esp32s3/firmware.bin` and `.elf`) and `diagram.json` with the board configured as our exact part:

```json
{ "type": "board-esp32-s3-devkitc-1",
  "attrs": { "flashSize": "16", "psramSize": "8", "psramType": "octal" } }
```

Wire up `wokwi/wokwi-ci-action@v1` so network, JSON, NVS and task-topology logic run headless on every push. **Read the scope limits in §4 before trusting any result.**
*Accept:* CI boots the firmware in Wokwi, connects WiFi, and asserts expected serial output. `partitions.csv` loads and `esp_partition_find` locates `logos` and `web` inside the simulator.

**T-1.9 — Dependency licence audit.** The project is **GPL-3.0-only**. Walk every entry in `.pio/libdeps/` and record name, version, licence and GPLv3 compatibility in `docs/DEPENDENCY-LICENCES.md`.
Known going in: ESPAsyncWebServer (ESP32Async fork) is **LGPL-3.0** — compatible, combines into GPLv3, and its §4 relinking obligation is satisfied by publishing source. ESP-IDF is Apache-2.0, which is GPLv3-compatible but **not** GPLv2-compatible — a concrete reason this project is v3. ArduinoJson and StreamUtils are MIT; Adafruit_GFX is BSD. `ESP32-HUB75-MatrixPanel-DMA` is MIT — compatible. The whole tree clears; this task is confirmation and record-keeping, not a gate. Fill in versions and tick off every row of `docs/DEPENDENCY-LICENCES.md`, paying attention to the one open question there: whether the Arduino-ESP32 core's LGPL-2.1 notice carries an "or any later version" clause. Also carry over `LICENSE.spleen.txt` and `LICENSE.tom-thumb.txt` from the Python into `assets/fonts/` — `tools/build_fonts.py` turns those glyphs into C tables and the attribution obligation follows them into the firmware.
*Accept:* every row in `docs/DEPENDENCY-LICENCES.md` has a version and a confirmation note; no incompatible licence present; font licences committed to `assets/fonts/`.

**T-1.10 — CI workflow.** `.github/workflows/ci.yml` exists with three jobs: **purity** (T-1.6 script, seconds, no toolchain), **native** (`pio test -e native`, uploads `test/out/` PNGs as artifacts so a pixel-parity failure can be eyeballed), and **firmware** (`pio run -e esp32s3`). Every step no-ops cleanly before the code it checks exists, so CI is green today and sharpens as phases land. Two follow-ons: add the Wokwi job at **T-1.8** once `WOKWI_CLI_TOKEN` is a repo secret, and turn the memory report into a hard gate once **T-0.7** supplies a real ceiling — the `TODO` and the commented-out check are already in place.
*Note:* PlatformIO's `RAM:` figure is `.data + .bss` — **static only, no runtime heap**. It cannot verify the `AGENTS.md` budget by itself; what it catches is slow erosion of internal DRAM. Runtime peak stays the job of on-device logging and the T-11.2 soak.
*Accept:* all three jobs green on `main`; deliberately adding `#include <Arduino.h>` to a `lib/render/` file turns **purity** red.

**T-1.11 — Commit.** `Phase 1: skeleton, partitions, task topology, render-purity guard, agent tooling, Wokwi CI, licence audit, CI workflow`

---

## Phase 2 — Render core

This phase builds the foundation everything visual sits on, **and the test harness that keeps it honest**. Do not skip T-2.6/T-2.7 to get to widgets faster — they are what make Phase 6 cheap.

**T-2.1 — Canvas.** `Canvas16`: width, height, `uint16_t*` pixels. PSRAM allocation helper. Bounds-checked `set_pixel`. No `Arduino.h`.
*Accept:* allocates a 2520×32 canvas in PSRAM (~158 KB) and frees cleanly; compiles under `env:native`.

**T-2.2 — Primitives.** `line`, `rect`, `fill_rect`, `ellipse`, `fill_ellipse`, `point`, and a **polygon fill** helper. The Python's `situation/` uses `draw.polygon()` four times; Adafruit_GFX only offers `fillTriangle`, so write a scanline fill once.
*Accept:* host tests render each primitive to PNG; polygon fill matches Pillow's output for the diamond and possession-arrow shapes.

**T-2.3 — `tools/build_fonts.py`.** Parse `spleen-5x8.bdf`, `spleen-6x12.bdf`, `tom-thumb.bdf` into C glyph tables (`uint8_t rows[N]` indexed by ASCII 32–126). Copy the BDFs and their licences into `assets/fonts/`.
*Accept:* generated header compiles; glyph bitmaps byte-match what Pillow's `ImageFont.load()` produces for the same characters.

**T-2.4 — Fixed-width text blitter.** `draw_text(canvas, font, x, y, str, colour)` plus `constexpr text_width(len) = len * advance`.

> **Why not Adafruit GFX fonts:** GFX fonts are proportional with baseline-relative placement; Spleen and tom-thumb are fixed-width and `CARD_DESIGN_SPEC.md` positions text on a fixed cell grid. More decisively, the Python calls `draw.textlength()` **27 times** against 31 `draw.text()` calls — nearly every label is centred or right-aligned against a computed width. Fixed-width makes that a one-line `constexpr`; proportional makes it 27 `getTextBounds()` calls whose results differ subtly from Pillow's, drifting every centred element.

*Accept:* rendering a string produces a **pixel-identical** PNG to the Python's output for the same font, string and position.

**T-2.5 — Outlined text.** Draw 4× at ±1 offset in `_COLOR_OUTLINE` black, then once in the fill colour — the same technique the Python uses.
*Accept:* pixel-identical to the Python's centre-overlay score text.

**T-2.6 — Host test harness.** `env:native` builds `lib/render/` for the laptop. PNG writer. Assertion helpers for canvas size and region content (solid/non-solid, dominant colour), mirroring `tests/test_render_widgets.py`.
*Accept:* `pio test -e native` runs and writes PNGs to `test/out/`.

**T-2.7 — Pixel-parity proof.** Port one non-trivial Python render test wholesale and prove parity end to end.
*Accept:* a committed side-by-side PNG showing zero differing pixels. **This is the checkpoint that validates the whole strategy** — if parity is unachievable here, revisit T-2.3/T-2.4 before building 10 widgets on a shaky base.

**T-2.8 — Panel driver wrapper.** `lib/` (not `lib/render/`) wrapper owning `MatrixPanel_I2S_DMA`. Config struct from `AGENTS.md`. Push a `Canvas16` window into the DMA framebuffer; `flipDMABuffer()`.
*Accept:* a canvas rendered by `lib/render/` appears correctly on the panel. DMA framebuffer confirmed in **internal** SRAM (~32 KB double-buffered).

**T-2.9 — Boot splash.** Device IP and version, with the Python's hold-scroll-hold-loop behaviour for overflowing text.
*Accept:* IP readable on the panel within 2 s of WiFi connect.

**T-2.10 — Native live preview.** Extend `env:native` with a real-time preview of the scrolling strip — SDL2 window, or a PNG sequence served over a local HTTP port. This is the replacement for the Python's RGBMatrixEmulator: **no ESP32 simulator can render HUB75** (§4), so this is the only way to iterate on card design without hardware.
*Accept:* the scroll engine runs on the laptop at 30 fps against fixture data, visually matching the panel. Card design work is possible with the device unplugged.

**T-2.11 — Commit.** `Phase 2: RGB565 canvas, primitives, fixed-width fonts, host test harness, native preview, panel bring-up`

---

## Phase 3 — Logo asset pipeline

**T-3.1 — `tools/build_logos.py`.** Port the *processing* half of the Python's `logo_pipeline.py` — unchanged Pillow logic: `getbbox()` trim → LANCZOS resize to target height → alpha threshold at 128 → UnsharpMask + saturation/contrast boost. Fetch team lists from ESPN's `/teams` endpoint per league.
*Accept:* produces the same processed images the Python caches today, for a sample of 10 teams.

**T-3.2 — `logos.bin` writer.** Emit the §3 format. Index sorted by `(league, abbr)` for binary search. **Key by league+abbreviation** — the Python has a fix specifically for cross-league abbreviation collisions (`eng.1_liv` vs `epl_liv`); preserve that. **Quantified at T-3.2: a bare-abbreviation key silently drops 11 of the 50 test logos**, because duplicate EPL slugs share abbreviations. Silently — no error, just missing art.
*Accept:* round-trips through a Python reader; index lookup returns correct offsets.

**T-3.3 — Generate the pro-league atlas.** NFL, NBA, MLB, NHL, EPL.
*Accept:* 144 logos, file size **~260 KB** (measured 258,992 B), fits the 2 MB partition with room for college later.

**T-3.4 — mmap reader.** `esp_partition_mmap()` the `logos` partition; binary-search the index; return a pointer + dimensions. No allocation, no copy.
*Accept:* lookup of a known team returns correct dimensions and non-null pixels. Heap usage before and after is **identical**.

**T-3.5 — Masked blit.** Blit RGB565 + 1-bit mask into a `Canvas16` with alpha test.
*Accept:* host test renders a logo pixel-identical to the Python's `paste(logo, box, mask=logo)`.

**T-3.6 — Fallback path.** Team abbreviation in team colour when a logo is absent.
*Accept:* unknown team renders the fallback, never a crash or blank. **This matters more than on the Pi** — a new team cannot self-heal by downloading.

**T-3.7 — Flash-access discipline.** Assert (debug build) that logo lookups only occur during strip rebuilds, never in the per-frame path.
*Accept:* assertion never fires during a 60 s scroll.

**T-3.8 — Commit.** `Phase 3: build-time logo pipeline, logos.bin, mmap reader`

---

## Phase 4 — Config store

**T-4.1 — Config structs.** Mirror the Python's five tables: `HardwareSetting`, `WidgetConfig`, `LeagueConfig`, `Favorite`, plus a schema version. Drop the dead Pi fields (`hardware_mapping`, `gpio_slowdown`, `pwm_bits`, `pwm_dither_bits`, `pixel_mapper_config`); add the ESP32 ones (`lsbMsbTransitionBit`, `clkphase`, `latch_blanking`, `i2sspeed`, `double_buff`). `options_json` becomes a small typed union — it only ever held `{"league": ...}`.
*Accept:* structs are POD, fixed-size, and `static_assert` under 4 KB total.

**T-4.2 — NVS store.** Load, save, seed defaults, reset-to-defaults. One namespace, blob per table.
*Accept:* survives power cycle; a corrupted/absent blob falls back to defaults without a crash loop.

**T-4.3 — Change notification.** Settings write signals `render` via task notification — the analogue of the Python's `asyncio.Event`.
*Accept:* a brightness change applies live without restart.

**T-4.4 — Structural-change detection.** Flag changes that need a restart (panel geometry) versus live-applicable ones, so the web UI can show its restart banner. Flag only on **changed** values, not field presence — the Python has a specific fix for this.
*Accept:* changing brightness sets no flag; changing `rows` does.

**T-4.5 — `tools/build_tzmap.py`.** Generate an IANA→POSIX TZ table (~450 zones) as a flash-resident C table.
*Accept:* `"America/New_York"` → `"EST5EDT,M3.2.0,M11.1.0"`. Table under 20 KB.

**T-4.6 — Timezone resolution.** Look up the stored IANA name, `setenv("TZ", ...)`, `tzset()`. Empty string = UTC with a visible indicator.
*Accept:* DST transition renders correctly in a host test with a faked clock; port the Python's `test_timezone.py` cases.

**T-4.7 — Commit.** `Phase 4: NVS config store, change notification, IANA→POSIX timezone table`

---

## Phase 5 — Data layer

**T-5.1 — Data structs.** `Team`, `Situation`, `Game` as POD with fixed `char[]` (`id[16]`, `name[32]`, `abbr[6]`, `colour` as `uint32_t`). `Optional[int]` → `int16_t` with `INT16_MIN` sentinel. Bound `MAX_GAMES_PER_LEAGUE`.
*Accept:* `sizeof(Game)` logged and budgeted; arrays statically allocated.

**T-5.2 — `DataCache` with pointer swap.** Two `Game[]` buffers per league. `poll` fills the inactive one and swaps a pointer atomically; `render` reads whichever is current. Carry over `last_fetch_age` and `is_stale(poll_interval, factor=2.5)`.
*Accept:* a stress test writing at 50 Hz while reading at 30 Hz produces **no torn reads**. Verify explicitly — this is the rule the Python never had to obey.

**T-5.3 — HTTP client wrapper.** `esp_http_client` + `esp_crt_bundle`. 3 retries, jittered exponential backoff (0.5/1.0/2.0 s ± 0–0.3 s) matching the Python's `_get()`. Send `Accept-Encoding: identity`. 15 s timeout. Wrap the response body in `ReadBufferingStream` (512–1024 B) before handing it to `deserializeJson()`.
*Accept:* recovers from a forced DNS failure; never leaks a session.

**T-5.4 — Filter documents.** One ArduinoJson filter per response shape, retaining exactly the fields `norm_game()` reads: `events[].id`, `.date`, `competitions[0].status.{period,displayClock,type.state,type.shortDetail}`, `competitors[].{homeAway,score,team.{id,displayName,abbreviation,color}}`, and `competitions[0].situation` whole.
Every call must pass `DeserializationOption::NestingLimit(20)` alongside the filter — the MLB payload nests to depth 15 and the default of 10 fails with `TooDeep` (measured, T-0.5). Check depth for any new endpoint before shipping its client.
*Accept:* filtered parse of `mlb_scoreboard.json` retains **≤ 8 KB raw** (measured 5,676 B; ~10.5 KB in PSRAM after ArduinoJson's ~1.9× slot overhead, which is fine).

**T-5.5 — Normalisation.** Port `norm_game`, `norm_team_from_competitor`, `norm_team_from_teams_endpoint`, `_norm_situation`, `_safe_int`, `_parse_date`. Missing fields degrade, never throw.
*Accept:* host tests over all six fixtures produce games matching the Python's output field-for-field.

**T-5.6 — Narrow the date window. REQUIRED, not an optimisation.** Request a **single day**, with a separate opportunistic fetch for yesterday only when the "show yesterday's finals" path needs it. Port `_filter_by_date` for the local-timezone day boundary.

> ⚠️ **T-0.5 on hardware measured 6,081 ms against a < 6 s budget — an 81 ms miss, unmitigated.** The gate was called GO on the basis that this mitigation exists, so shipping without it means the live-game poll path is over budget by construction. This is no longer a tuning task to reach for if things feel slow; it is the mitigation the go/no-go decision was predicated on. Do not defer it.

*Accept:* MLB response size and end-to-end fetch+parse time measured **on device** and recorded — expect ~3× smaller and ~2–3 s. If it does not land under 6 s with comfortable margin, escalate to T-11.3 (gzip) rather than accepting an at-budget number. *(Worth backporting to the Python too.)*

**T-5.7 — Poll task.** Iterate enabled leagues **sequentially, never concurrently**. Live cadence 15–30 s, idle far slower, per `LeagueConfig`. Stagger start times so two leagues never align.
*Accept:* only one TLS session ever open — verify with a heap watermark log. Full cycle across five leagues completes without the render task dropping below 28 fps.

**T-5.8 — Parser test corpus.** Copy `tests/fixtures/*.json` (6 files, 1.77 MB) from the Python. Host tests parse from a file stream exactly as the device parses from a socket.
*Accept:* all six parse; no test touches the network.

**T-5.9 — Commit.** `Phase 5: data structs, lock-free cache, filtered ESPN client`

---

## Phase 6 — Cards and widgets

Copy every layout constant from the Python verbatim. Do not re-derive; `CARD_DESIGN_SPEC.md` was tuned on real hardware. Each task lands with a host render test.

**T-6.1 — `CardProducer` interface.** `cards(cache, now, panel_h) -> Canvas16[]` and `cards_key(cache, now) -> hash`. `CARD_W = 64`.
*Accept:* two stub producers compose into a strip.

**T-6.2 — Clock widget.** 12/24h from config. `cards_key` = minute + format.
*Accept:* renders both formats; key changes only on the minute.

**T-6.3 — Game card: PRE.** 24 px corner logos, centred "VS", start time bottom.
*Accept:* matches the Python's PNG.

**T-6.4 — Game card: FINAL.** 19 px corner logos, centred "A-B", "FINAL" label. Previous-day games show the date (M/D) instead of FINAL — a local-timezone boundary case the Python fixed explicitly.
*Accept:* both variants match; the date-boundary case is tested.

**T-6.5 — Game card: LIVE generic.** 13 px four-corner logos + scores.
*Accept:* matches.

**T-6.6 — MLB situation: diamond.** Stacked rows, large diamond, OUT label with ellipse circles, ball/strike count. Situation-aware cache key.
*Accept:* matches for bases-loaded, empty, and partial states; a missing `situation` hides the graphic without erroring.

**T-6.7 — NFL situation: gridiron.** Stacked logos, possession football, 8 px bottom field strip with goalposts and thin grass line.
*Accept:* matches; possession indicator follows `situation.possession`; red-zone state renders.

**T-6.8 — Period/clock layout.** NHL/NBA/soccer: bleeding-edge 30 px logos, outlined centre score/period/clock. Route by the Python's `_PERIODCLOCK_LEAGUES` set, including the ESPN slug variants.
*Accept:* matches for all three sports.

**T-6.9 — Scoreboard widget.** One card per game; `is_visible` false when no games; `has_live_priority_games` for preemption.
*Accept:* produces N cards for N games; `cards_key` changes on score and situation updates only.

**T-6.10 — Commit.** `Phase 6: card producers, game card states, situation indicators`

---

## Phase 7 — Scroll engine

The Python's algorithm is correct as written. Port it faithfully rather than reinventing.

**T-7.1 — Strip builder.** Concatenate all producer cards with `card_gap` separators into one wide PSRAM canvas. Return block widths for paging.
*Accept:* 35 cards → ~2520 px, ~158 KB, allocated in PSRAM.

**T-7.2 — Double-buffered rebuild.** Build the new strip into a back buffer on **core 0**, then swap pointers atomically. Preserve `scroll_x` across rebuilds; wrap modulo `(panel_w + strip_w)` when the strip shrinks so the ticker never snaps to 0.
*Accept:* a score change mid-scroll causes no visible jump and no dropped frames.

**T-7.3 — Scroll renderer.** Per frame: advance `scroll_x` by `scroll_speed * dt`, compute the visible window, blit. Port the `src_x0` / `dst_x0` / `visible_w` clipping arithmetic exactly — it handles the wrap case correctly.
*Accept:* seamless loop with a `panel_w` black lead-in; 4 KB copied per frame, ~120 KB/s at 30 fps.

> ⚠️ **Write every panel cell every frame.** The Python allocates a fresh `new_frame()` each iteration, so unwritten pixels are black by construction. **This firmware blits into a persistent DMA framebuffer** — any cell you skip keeps the previous frame's value. T-2.8 hit this: the splash blit wrote only the clipped canvas rect, and every column past the canvas edge smeared frozen fragments of the last frame during the traverse.
> Host tests cannot catch it — they compare a freshly-allocated `Canvas16` against Pillow, where the bug does not exist. Blit the full `panel_w × panel_h`, reading off-canvas as black through the bounds-checked accessor. At 2048 cells × 30 fps this is 61 k writes/s — free.

**T-7.4 — Static paging mode.** `compute_pages` from block widths; 5 s dwell per page.
*Accept:* pages align to card boundaries, never splitting a card.

**T-7.5 — Favourite-team preemption.** Producers with live priority games sort to the front.
*Accept:* enabling a favourite with a live game floats their cards first within one rebuild.

**T-7.6 — Frame pacing.** 30 fps target with jitter measurement.
*Accept:* sustained ≥28 fps during a full five-league poll cycle; log worst-case frame time. Confirm a forced 200 ms stall produces a **hitch, not a blank panel** — the DMA refresh is autonomous.

**T-7.7 — Commit.** `Phase 7: unified strip, scroll and paging, preemption`

---

## Phase 8 — Web server and UI

**T-8.1 — Server bring-up.** ESP32Async ESPAsyncWebServer on the `web` task.
*Accept:* serves a static route; heap cost measured and inside budget.

**T-8.2 — `tools/pack_web.py`.** Gzip `assets/web/index.html` into the `web` partition.
*Accept:* served with `Content-Encoding: gzip`; the 505-line SPA compresses to a few KB.

**T-8.3 — `/api/settings`, `/api/system`.** Read/write hardware settings; system reports IP, uptime, free heap, PSRAM free, firmware version, restart-required flag.
*Accept:* the existing SPA's settings screens work unmodified.

**T-8.4 — `/api/sports`.** Leagues enable/disable, team lists, favourites CRUD.
*Accept:* team picker populates; favourites persist across reboot.

**T-8.5 — `/api/widgets`.** Carousel order and per-widget enable/dwell, including drag-to-reorder.
*Accept:* reordering changes card order within one rebuild.

**T-8.6 — `/preview`.** Serve the current strip as a **BMP** — no zlib needed, 54-byte header.
*Accept:* renders in a browser and matches the panel.

**T-8.7 — Onboarding CSS.** The SPA uses CDN Tailwind + Alpine, so the *client* needs internet. That fails during AP-mode provisioning. Vendor a minimal inline stylesheet for the onboarding page only.
*Accept:* the provisioning page is usable on a phone with no internet.

**T-8.8 — Appropriate Legal Notices (GPLv3 §5d).** The settings SPA is an interactive user interface, so it must display a copyright notice, the no-warranty disclaimer, a statement that it is redistributable under GPLv3, and how to view the licence. An "About" item in the nav satisfying all four is enough — this is a licence obligation, not a nicety.
*Accept:* the notice is reachable from the SPA in one click and names the version and source URL.

**T-8.9 — Commit.** `Phase 8: async web server, /api/*, gzipped SPA, BMP preview`

---

## Phase 9 — Provisioning and OTA

**T-9.1 — SoftAP captive portal.** On no stored credentials or repeated connect failure, start an AP and serve a network picker.
*Accept:* phone connects, sees the portal automatically, selects a network. Far simpler than the Pi's `hostapd`/`wpa_supplicant` path.

**T-9.2 — Credential storage and reconnect.** NVS-backed, with exponential backoff and a fallback to AP mode after sustained failure.
*Accept:* survives router reboot; recovers unattended.

**T-9.3 — Convert to Arduino-as-IDF-component.** Required for `esp_ota`, full `menuconfig` access, **and the mbedTLS tuning that T-0.6 proved impossible on the Arduino core** — a confirmed ~14 KB recovery (53 KB → ~39 KB) that is unreachable until this conversion happens. `lib/render/` should need **zero changes** — if it does, T-1.6 was not doing its job.
*Accept:* builds and runs identically; re-verify the T-0.6 mbedTLS settings now actually take effect.

**T-9.4 — Firmware OTA.** `esp_ota` against the two app slots, with rollback on boot failure.
*Accept:* a bad image rolls back automatically.

**T-9.5 — Logo OTA.** Fetch `logos.bin` from a static host on a slow schedule with an ETag; write to the `logos` partition; re-mmap. Verify checksum before swapping.
*Accept:* a rebranded team's logo updates without a firmware flash. This restores the auto-update property lost by moving the pipeline to build time.

**T-9.6 — Factory reset.** Long-press or web endpoint: clear NVS, keep firmware.
*Accept:* returns to first-boot provisioning.

**T-9.7 — Commit.** `Phase 9: SoftAP provisioning, IDF conversion, firmware and logo OTA`

---

## Phase 10 — Weather and ticker

**T-10.1 — Weather client.** Same filtered-fetch pattern; payloads are small, so low risk.
*Accept:* parses, caches, degrades to last-good.

**T-10.2 — Weather widget.** Port the Python's layout.
*Accept:* host render test matches.

**T-10.3 — Ticker sources.** News/stocks/crypto, keys in NVS.
*Accept:* each source fails independently without taking down the ticker.

**T-10.4 — Unified continuous ticker.** Integrate as `CardProducer`s so they join the same mega-strip.
*Accept:* no special-casing in the engine.

**T-10.5 — Scheduling and quiet hours.** Time-of-day enable/disable, brightness schedule.
*Accept:* panel blanks and restores on schedule.

**T-10.6 — Commit.** `Phase 10: weather, ticker sources, scheduling`

---

## Phase 11 — Hardening and stretch

**T-11.1 — Watchdogs and health.** Task watchdog on `render` and `poll`; brownout detector; heap and stack high-water reporting on `/api/system`.
*Accept:* a deliberately hung task triggers a clean reboot.

**T-11.2 — Soak test.** 72 hours with live games, logging frame rate, heap floor, and reconnects.
*Accept:* **no heap decline over 72 h** (the fragmentation test), no unexplained reboots.

**T-11.3 — gzip transport (conditional).** Only if T-5.6's measured poll duty cycle is still too high. Streaming miniz inflate between socket and ArduinoJson.
*Accept:* 3–5× less wire time, no memory regression. **Skip if not needed** — this is real complexity for a problem that may already be solved.

**T-11.4 — Score alerts.** Preempt the rotation on a favourite team scoring.
*Accept:* alert appears within one poll cycle and returns to rotation.

**T-11.5 — Standings widget.**

**T-11.6 — Play-by-play / win probability.** Each new data source needs its own filter document and struct — budget accordingly.

**T-11.7 — College leagues.** Extend `logos.bin` to ~1,000 teams (~2.08 MB), still inside the partition.

---

## §5 Risk register

| Risk | Phase | Severity | Mitigation |
|---|---|---|---|
| ~~MLB payload too slow even filtered~~ | 0 | **Closed** | Payload is ~298 KB not 1.46 MB; parse is 90 ms. Risk did not exist. |
| Wire-time variance (1.35–6.7 s on identical payloads) | 5 | Low | Not optimisable — a network property. T-11.3 gzip only if on-site transport stays >4 s. |
| ESPN changes its API shape without notice | any | **Medium→High** | Already happened once: multi-day windows removed mid-build. Filters degrade gracefully; fixtures catch drift in CI; expect more. |
| Pixel parity unachievable with the font blitter | 2 | High | T-2.7 checkpoint before building 10 widgets |
| mbedTLS settings ignored on Arduino core | 0/9 | Medium | Known issue; pull the T-9.3 IDF conversion forward if T-0.6 shows no effect |
| Torn reads in `DataCache` | 5 | Medium | Pointer swap, T-5.2 stress test |
| Heap fragmentation over days | 11 | Medium | Static allocation in hot paths; T-11.2 soak |
| ESPN schema change | any | Medium | Filters degrade gracefully; fixtures catch drift in CI |
| pioarduino lags an upstream release | any | Low | Small volunteer team, but well-adopted; pin the platform version |
| Logos stale without OTA | 3 | Low | T-9.5 |

## §6 Measurements

**M** = measured on hardware. **W** = measured in Wokwi. **P** = still a projection — treat with suspicion.

| Quantity | Value | |
|---|---|---|
| **Memory** | | |
| Internal heap ceiling, WiFi + SNTP up | **~262 KB** (268,652 B W; hardware ~6 KB lower) | M |
| mbedTLS session, peak | **~53 KB** (W) / **~56 KB** (M) — **untunable until T-9.3** | M |
| Peak simultaneous mbedTLS + active parse | **73,616 B (71.9 KB)** | M |
| Consumers vs ceiling | 141–165 KB used, **~97–121 KB headroom** | M |
| DMA framebuffer, 64×32 | **61 KB internal** measured at `begin()` (double-buffered: 32 KB fb + ~29 KB driver task stack/structs) — not the 32 KB projected | M |
| `esp_partition_mmap()` one-time cost | ~104 B page tables; per-lookup **0 B** | M |
| PSRAM total use | < 400 KB of 8 MB | P |
| **Payload and timing** | | |
| **Live MLB payload, single day** | **~298 KB** — multi-day windows now 400 (T-5.6) | M |
| **Filtered parse of 298 KB** | **90 ms** — the parse was never the bottleneck | M |
| **Fetch+parse wall clock** | **1.35–6.7 s, transport-dominated**; best case 2.8 s | M |
| MLB scoreboard fixture (historical, 3-day window ESPN no longer serves) | 1,457,268 B / 15 events | M |
| Retained after filtering (fixture) | 5,676 B raw / 10.5 KB PSRAM | W |
| Retained after filtering (live, `situation` present) | 24.5 KB PSRAM | M |
| Filtered parse cost, internal | 1.6 KB (from memory, W) / **13.1 KB** (off TLS, M) | M |
| Fetch + parse, T-0.5 single sample | 6,081 ms — **misattributed to parse; it was wire time** | M |
| ESPN JSON nesting depth | **15** — `NestingLimit(20)` mandatory, default 10 fails | M |
| All fixtures | 6 files, 1.77 MB | M |
| **Assets** | | |
| Logo at 32 px | 2176 B worst case, **~1.8 KB average** | M |
| Logo atlas, 144 pro teams | **258,992 B** | M |
| Logo atlas, ~1,000 teams incl. college | ~1.8 MB | P |
| `logos.bin` index stride | **22 B exactly**, no padding | M |
| **Panel** | | |
| Refresh rate | **110 Hz** at `lsbMsbTransitionBit` 1 | M |
| Card strip, 35 cards | 2520 × 32 × 2 B ≈ 158 KB, PSRAM | P |
| Per-frame blit | 4 KB, ~120 KB/s at 30 fps | P |
| **Source** | | |
| Python being replaced | 4,253 LOC, 10 widgets | M |
| `draw.textlength()` call sites | 27 (vs 31 `draw.text()`) — the fixed-width font argument | M |
| `draw.polygon()` call sites | 4 — needs a scanline fill helper | M |

> Superseded projections, kept so they are not re-derived: ~320 KB usable after WiFi (WiFi was double-counted — it is
> ~262 KB); 26–36 KB tuned mbedTLS (**unreachable on the Arduino core**, T-0.6); ~14 KB asymmetric-content-len saving
> (deferred to T-9.3); ~306 KB atlas (actual 258,992 B); ~20 B index stride (actual 22 B — cost 20 desynced lookups).
