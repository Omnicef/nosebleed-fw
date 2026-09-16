# AGENTS.md — nosebleed-fw

Project guidance for AI coding agents. Read this at the start of every session. The full build plan lives in `PLAN.md`; this file is the quick-reference for conventions, hardware facts, and current state.

> Claude Code reads `CLAUDE.md`, which is a one-line stub importing this file. Other tools (opencode, Codex, Cursor) read this file directly. **Edit `AGENTS.md`, never the stub.**

## What this is

**Nosebleed** is an LED-matrix sports ticker: live scores plus rotating info widgets (clock, weather, news/stocks/crypto), configured from a browser. *Worst seat in the house. Best view of the score.* `nosebleed-fw` is the **ESP32-S3 firmware rewrite** of `Marquee`, the original Raspberry Pi Python implementation.

> **On `Marquee`.** Nosebleed is a rewrite of a working Raspberry Pi implementation in Python, referred to throughout as *Marquee* or *"the Python"*. **That repo is private and not publicly available** — it is a design reference for the author and for agents working in this repo, not a dependency. Nothing here requires it: the ported card layouts, fonts and test fixtures are all committed. References to it are historical provenance, and can be read as "this decision came from a working prior implementation".
>
> Agents with local access: set `$MARQUEE_REPO` to its checkout path. It is **READ-ONLY**.

The Python project (`Marquee`, Phase 5 complete) is the **design reference**. Its `PLAN.md`, `CARD_DESIGN_SPEC.md`, `docs/API_NOTES.md`, `tests/fixtures/*.json` and BDF fonts are the source of truth for *what to build*. None of its code is being ported line-by-line — Pillow, FastAPI, SQLite and the hzeller driver have no equivalents here.

Build it by following the phased plan in `PLAN.md` — **do not jump ahead**; each phase de-risks the next. Phase 0 is a go/no-go gate.

## Hardware (fixed — do not re-litigate)

- **PlatformIO board ID:** `esp32-s3-devkitc1-n16r8` — **not** `esp32-s3-devkitc-1`, which is an N8 part with no PSRAM. `board_build.flash_size` is ignored by pioarduino; it reads `upload.flash_size`. Getting this wrong produces a silently degraded 8 MB no-PSRAM build. Always confirm against the boot log, never the config file.
- **MCU:** ESP32-S3-WROOM-1 **N16R8** — 16 MB quad flash, 8 MB **octal** PSRAM, dual Xtensa LX7 @ 240 MHz, 512 KB internal SRAM, native USB, 2.4 GHz WiFi + BLE 5.
- **Panel:** HUB75, default 64×32, 1/16 scan. **Never hardcode 64×32** — read from config.
- **Adapter:** DevKitC-1 carrier with a **74HCT245** level shifter. ESP32 GPIO is 3.3 V; HUB75 wants 5 V logic.
  On the SEENGREAT V2.x carrier the **silkscreen colour labels are wrong**: G1/B1 and G2/B2 are transposed
  vs. the actual wiring (silkscreen G1=GPIO8 is really B). `lib/panel/panel.cpp`'s pin map is the
  **hardware-proven** order and wins over the silkscreen and the manufacturer wiki. Prove any new panel or
  adapter with a colour-*name* test (the word GREEN drawn in green), never solid fills — coherent ≠ correct.
- **Power:** panel from the PSU directly (5 V @ 4 A min for one 64×32, 8 A sustained full-white). Separate cable to the board. Shared rails brown out.

## Tech stack (decided — see PLAN.md §1)

- **Framework:** Arduino-ESP32 via **pioarduino** (`pioarduino/platform-espressif32`), converting to **Arduino-as-ESP-IDF-component** when Phase 9 needs `esp_ota`. Upstream PlatformIO `espressif32` is stale — do not use it.
- **Panel driver:** `ESP32-HUB75-MatrixPanel-DMA` (mrcodetastic). Adafruit_GFX-compatible, DMA off the S3 LCD peripheral.
- **JSON:** **ArduinoJson v7** with `DeserializationOption::Filter`. This is non-negotiable — see the hard rules.
- **HTTP client:** `esp_http_client` / `HTTPClient` with `esp_crt_bundle`.
- **Web server:** `ESP32Async/ESPAsyncWebServer` v3.x (the maintained fork — **not** `me-no-dev`). Fall back to `PsychicHttp` if it proves unstable under load.
- **Config:** NVS (`Preferences` or `nvs_*` directly). Config only.
- **Concurrency:** four pinned FreeRTOS tasks. See below.

## Architecture in one paragraph

A `render` task on **core 1** owns the panel: it composes a wide RGB565 "strip" of cards in PSRAM whenever content changes, and every frame blits a 64 px window of it into the DMA framebuffer in internal SRAM. A `poll` task on **core 0** fetches ESPN/weather/ticker data over TLS, parses it with a filtered streaming JSON reader, and publishes complete `Game[]` arrays by atomic pointer swap. A `web` task on core 0 serves the settings SPA and `/api/*`. A `net` task supervises WiFi and SNTP. Team logos are pre-rendered on a laptop into a flash partition and read via `esp_partition_mmap` with zero RAM copy. The HUB75 DMA refresh runs autonomously off the LCD peripheral — a CPU stall is a scroll hitch, never a blank panel.

## Hard rules (don't violate)

### Never write an API call from memory

**Do not write a call to any `esp_*`, `nvs_*`, `MatrixPanel_I2S_DMA`, `HUB75_I2S_CFG`, ArduinoJson, ESPAsyncWebServer, or Adafruit_GFX API from memory.** Open the actual header in `.pio/libdeps/` (or the IDF component directory) and match the real signature, parameter order, and return type before writing the call.

This platform is a thin slice of any model's training data and plausible-looking API calls are the dominant failure mode here — invented parameters on `esp_partition_mmap`, misremembered `HUB75_I2S_CFG` field names, ArduinoJson v6 idioms in a v7 project. A wrong signature that compiles is worse than one that doesn't.

If the header is not present, **stop and say so** rather than guessing. Do not infer a signature from a blog post, a Stack Overflow answer, or an example sketch without confirming it against the installed version — these libraries have breaking changes between majors, and ArduinoJson v6 → v7 in particular rewrote the document API.

### Everything else

- **Never buffer a whole ESPN response.** The MLB scoreboard fixture is **1.46 MB for 15 events**. Always parse with an ArduinoJson `Filter` straight off the HTTP stream. Retained payload should be ~300 B per game.
- **`lib/render/` must not include `Arduino.h`, `esp_*.h`, or `String`.** It is framework-agnostic C++ compiled for both the device and the host. This is what keeps the render tests runnable on a laptop, and what makes the eventual IDF conversion cheap. CI greps for violations.
- **Never block the render task on I/O or a mutex.** If it cannot take the data lock instantly, it re-renders the last strip. Data comes from the in-memory cache only.
- **Only `render` runs on core 1.** WiFi, TLS and JSON all live on core 0, where the multi-second stalls belong.
- **`DataCache` needs synchronisation.** The Python original was single-threaded asyncio and used no locks. FreeRTOS preempts and the S3 is genuinely dual-core. Publish by building a complete new array and swapping a pointer atomically — never mutate a `Game` in place while `render` may be reading it.
- **DMA framebuffer stays in internal SRAM.** The library can place it in PSRAM on octal parts like ours, but that caps the output clock near 13 MHz. Only relevant for long chains; we have one panel.
- **SNTP must succeed before the first TLS handshake**, or certificate validation fails on the `notBefore` date. Boot order: WiFi → SNTP → TLS.
- **Use `esp_crt_bundle`, never a pinned certificate.** ESPN rotates certs; pinning schedules an outage.
- **One TLS session at a time.** Stagger league polls. Concurrent sessions blow the internal-RAM budget.
- **Degrade gracefully.** Missing ESPN fields (especially `situation`) hide that element rather than erroring. Network down = last-good data plus a small offline indicator, never a blank panel. Missing logo = team abbreviation in team colour.
- **Config only in NVS.** Scores, caches and runtime status stay in RAM, never persisted. (Same rule as the Pi original — flash wear replaces SD-card corruption as the reason.)
- **The Marquee Python repo (`$MARQUEE_REPO`) is READ-ONLY.** It is the design reference — `PLAN.md`, `CARD_DESIGN_SPEC.md`, `docs/API_NOTES.md`, the BDF fonts and the ESPN fixtures. Never write to it, not even a small fix or a backport. Changes there are a separate decision in a separate session.
- **Never read an ESPN fixture whole.** `tests/fixtures/mlb_scoreboard.json` is **1.4 MB — roughly 400k tokens**. One `cat` ends the session. Sample the specific fields you need with `jq` or a Python one-liner. The fixtures are a test corpus for compiled code, not reading material. Same rule at T-5.8.
- **Be a good API citizen.** Cache aggressively, jittered exponential backoff on errors, live games 15–30 s, everything else far slower. No API key for ESPN — don't add one.

## Memory budget (measured, not projected)

**Internal SRAM — measured in Wokwi at T-0.1/T-0.3. Re-verify on hardware.**

| Point | Free internal heap |
|---|---|
| No WiFi stack linked (T-0.1) | 346,072 B |
| WiFi + SNTP linked, not connected | 318,032 B |
| **WiFi connected + SNTP synced — the working ceiling** | **268,652 B (~262 KB)** |

Linking the WiFi/SNTP stack costs ~27 KB of static baseline; connecting costs a further ~48 KB, at the low edge of
the 50–80 KB expectation. SNTP is ~504 B in steady state — effectively free once the clock lands.

**Everything below is spent from that ~262 KB ceiling. WiFi is already deducted — do not count it again.**

| Consumer | Budget |
|---|---|
| HUB75 DMA framebuffer, 64×32, 8-bit depth, double-buffered | **61 KB** measured at `begin()` (32 KB fb + ~29 KB driver task stack/structs) |
| mbedTLS session, peak, one connection | **53 KB** (measured; see below) |
| HTTP stream buffer | 4–8 KB |
| Task stacks (4) | ~32 KB |
| Web server + connections | 20–40 KB |
| **Total** | **141–165 KB** |
| **Headroom** | **~97–121 KB** |

DMA framebuffer arithmetic: 16 row-pairs × 8 bit-planes × (64 px × 2 B) = **16 KB per buffer**.

> **TLS is 53 KB and cannot be tuned on this stack.** T-0.6 proved the mbedTLS options (`ASYMMETRIC_CONTENT_LEN`, `SSL_OUT_CONTENT_LEN`, drop-keep-peer-cert) are **inert under Arduino-ESP32**: the core links a *prebuilt* `libmbedtls` compiled untuned, pioarduino offers no from-source path, and its prebuild hook overwrites `sdkconfig`. The ~14 KB recovery is real but only lands after the **T-9.3 IDF conversion**. Budget 53 KB until then. It still fits comfortably — do not treat this as a reason to convert early.

> **Superseded projection.** This file previously claimed "~320 KB usable after WiFi" *and* listed WiFi as a 50–80 KB
> consumer — internally inconsistent, since a post-WiFi figure already has WiFi deducted. Measurement settled it:
> the ceiling is ~262 KB and WiFi is not a line item against it. If you see a headroom figure in the 20–80 KB range,
> WiFi has been counted twice.

**PSRAM (8 MB) — effectively unconstrained.** Card strip ×2 ≈ 320 KB, JSON document 8–32 KB. Under 400 KB of 8 MB.
Measured free at boot: 8,384,788 B.

**Flash (16 MB):** logo atlas is the only large consumer — ~306 KB for 144 pro teams, ~2.1 MB including all college.

## Task topology

| Task | Core | Prio | Stack | Role |
|---|---|---|---|---|
| `render` | 1 | 3 | 8 KB | 30 fps window blit, strip rebuild |
| `poll` | 0 | 2 | 12 KB | ESPN/weather/ticker fetch + filtered parse |
| `web` | 0 | 2 | 8 KB | HTTP server |
| `net` | 0 | 1 | 4 KB | WiFi supervision, SNTP, reconnect |

## Repo layout

```
nosebleed-fw/
├── .github/workflows/ci.yml  # purity + native tests + esp32s3 build
├── AGENTS.md                 # this file — the real guidance
├── CLAUDE.md                 # one-line stub: @AGENTS.md
├── PLAN.md
├── LICENSE                   # GPL-3.0-only
├── platformio.ini            # env:esp32s3, env:native
├── partitions.csv            # two app slots + logos + web
├── sdkconfig.defaults        # mbedTLS tuning
├── opencode.json             # lsp + instructions for opencode
├── wokwi.toml                # simulator config — logic only, see PLAN.md §4
├── diagram.json              # S3 board as N16R8: 16 MB flash, 8 MB octal PSRAM
├── tools/                    # HOST-side build tooling
│   ├── check_render_purity.sh  # T-1.6 guard, run by CI and pre-action
│   ├── build_logos.py        # Pillow pipeline → logos.bin
│   ├── build_fonts.py        # BDF → C glyph tables
│   ├── build_tzmap.py        # IANA → POSIX TZ table
│   └── pack_web.py           # gzip the SPA
├── assets/
│   ├── fonts/*.bdf           # copied from the Python, unchanged
│   └── web/index.html        # copied from the Python, ~unchanged
├── lib/
│   ├── render/               # NO Arduino.h — host-compilable
│   │   ├── canvas.*, font.*, primitives.*, strip.*
│   │   ├── widgets/          # 10 widgets
│   │   └── situation/        # diamond, gridiron, indicators
│   ├── data/                 # structs, cache, espn, weather, ticker
│   ├── config/               # NVS store, defaults, timezone
│   ├── logos/                # mmap reader
│   ├── net/                  # wifi, sntp, provisioning, ota
│   └── web/                  # server + api handlers
├── src/main.cpp              # app_main, task creation
└── test/
    ├── test_native/          # host render + parser tests
    └── test_embedded/        # on-device smoke tests
```

> Suite directories **must** be named `test_*` — PlatformIO's test discovery
> silently ignores any `test/` subdir without the prefix (it reports "Nothing to
> build", not an error naming the folder). `pio test -e native` selects the
> `test_native` suite via `test_filter`.

## Build phases (PLAN.md — do in order)

0. **Spike / go-no-go.** Toolchain, panel hello-world, TLS, filtered MLB fetch. Measure heap and time. **Stop here if it fails.**
1. Skeleton, build system, partitions, task topology.
2. Render core: RGB565 canvas, font blitter, primitives, **host test harness**.
3. Logo asset pipeline: build tool, `logos.bin`, mmap reader.
4. Config store (NVS) + timezone table.
5. Data layer: structs, cache, ESPN client with per-league filters.
6. Cards: `game_strip` states + situation indicators.
7. Scroll engine + favourite-team preemption.
8. Web server + `/api/*` + the existing SPA + `/preview`.
9. Provisioning (SoftAP) + OTA (firmware and logos).
10. Weather + ticker widgets.
11. Hardening + stretch (standings, play-by-play, win probability).

## Conventions

- C++17. `constexpr` over `#define`. POD structs for data; no dynamic allocation in the render path.
- Fixed-size buffers with explicit bounds — no `std::string` or `String` in `lib/render/` or `lib/data/`.
- Every widget implements the `CardProducer` interface: `cards()` and `cards_key()`. Content only rebuilds when `cards_key()` changes.
- **Layout constants are copied verbatim from the Python** (`_LOGO_H_LIVE = 13`, `_FIELD_STRIP_H = 8`, etc.). Do not re-derive them; `CARD_DESIGN_SPEC.md` was tuned against real hardware.
- Fonts are **fixed-width bitmap**, not Adafruit GFX fonts. `text_width(s) == len(s) * advance`. This is deliberate — see PLAN.md §T-2.4.
- Render tests output PNGs at target resolution and assert size and regions, running headless on the host via `env:native`. Every widget lands with a test.
- ESPN fixtures from the Python repo are the parser test corpus. Never hit the network in a test.
- Secrets and API keys in NVS or `secrets.h` (gitignored), never committed.
- **Licence: GPL-3.0-only.** Every new source file starts with `// SPDX-License-Identifier: GPL-3.0-only`. Do not add a dependency without checking its licence is GPLv3-compatible — permissive (MIT/BSD/Apache-2.0) and LGPL are fine; anything proprietary, non-commercial, or GPLv2-only is not. Record it in `docs/DEPENDENCY-LICENCES.md`.
- Commit at the end of each phase.

## Testing layers

| Layer | Tool | Covers |
|---|---|---|
| Render | `pio test -e native` | All 10 widgets, layout, fonts, strip, scroll — pixel-exact PNG |
| Logic | Wokwi | WiFi, TLS, filtered JSON, NVS, partitions, task structure |
| Reality | Hardware | Panel output, **all timing**, thermal, soak |

**Two things no simulator can do for us**, both consequences of Wokwi's limits (PLAN.md §4):

- **HUB75 output cannot be simulated.** I2S is unimplemented on the S3 in Wokwi and no HUB75 part exists. There is no
  RGBMatrixEmulator equivalent. Card design is iterated through the native preview (T-2.10) and confirmed on hardware.
- **Timing measured in a simulator is meaningless.** Wokwi caps the CPU near 8 MHz. Never evaluate the T-0.5 budget or
  the 30 fps target there. Heap figures are reasonably faithful; wall-clock figures are not.

## Running locally (no hardware)

```bash
pio test -e native            # render + parser tests, PNG output to test/out/
pio run -e native -t exec     # live scrolling preview (T-2.10)
wokwi-cli .                   # logic-only simulation; see the limits above
pio run -e esp32s3 -t upload  # flash the device
pio device monitor
pio run -t compiledb          # refresh compile_commands.json for clangd
```

## Reference: what changed from the Pi

**Config fields that no longer exist:** `hardware_mapping`, `gpio_slowdown`, `pwm_bits`, `pwm_dither_bits`, `pixel_mapper_config`. All Pi-driver concepts.

**Replaced by:** `lsbMsbTransitionBit` (depth/refresh tradeoff — 0 ≈ 57 Hz, 1 ≈ 110 Hz on 64×32), `clkphase`, `latch_blanking`, `i2sspeed`, `double_buff`.

**Gone entirely:** SQLite/WAL/corruption recovery, `install.sh`, systemd, the wheel build, Pi-model detection, `dtparam=audio=off`, `isolcpus`.

**Changed meaning:** `timezone` stores IANA names but newlib needs POSIX TZ strings — a lookup table ships in flash (T-4.5).
