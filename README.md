# Nosebleed

**Worst seat in the house. Best view of the score.**

A live sports ticker and ambient info display for HUB75 LED matrix panels, driven by an ESP32-S3. Scores update in near real time; a rotating carousel of widgets fills the gaps with a clock, weather, news, stocks and crypto. Everything is configured from a web page served by the device itself — no app, no cloud account, no subscription.

> **Status: Phase 3 of 11 complete — in active development, not yet usable.**
> The panel driver, render core and logo pipeline work on real hardware. There is no config UI, no data layer and no scoreboard yet. See [`PLAN.md`](PLAN.md) for the roadmap.

Two things were validated on hardware before any of it was built, and both are recorded with real numbers in [`SPIKE_RESULTS.md`](SPIKE_RESULTS.md):

- **The ESPN payload is tractable.** A single MLB scoreboard response is **1.46 MB**. Streaming it through an ArduinoJson filter retains ~300 bytes per game and costs 13 KB of internal heap — against a measured 262 KB ceiling.
- **The rendering is pixel-exact.** Card layouts are ported from the Python original and verified by a **zero-pixel diff** against Pillow's output, so the designs survive the rewrite rather than being re-tuned by eye.

| Phase | | |
|---|---|---|
| 0 | Spike — go/no-go gate | ✅ closed, GO |
| 1 | Skeleton, partitions, task topology, CI | ✅ |
| 2 | Render core — canvas, fonts, parity gate | ✅ |
| 3 | Logo pipeline — 144 teams, 259 KB atlas | ✅ |
| 4–7 | Config, data layer, widgets, scroll engine | ⬜ |
| 8–11 | Web UI, provisioning, OTA, hardening | ⬜ |

## Hardware

| Part | Spec |
|---|---|
| MCU | ESP32-S3-WROOM-1 **N16R8** — 16 MB flash, 8 MB octal PSRAM, dual LX7 @ 240 MHz |
| Panel | HUB75, 64×32 default (configurable), 1/16 scan |
| Adapter | DevKitC-1 carrier with a **74HCT245** level shifter |
| Power | 5 V @ 4 A minimum for one 64×32 panel, fed directly from the PSU |

The level shifter is not optional — ESP32 GPIO is 3.3 V and HUB75 expects 5 V logic. See [`AGENTS.md`](AGENTS.md) for the full hardware notes.

## Building

```bash
pio test -e native            # render tests, PNG output to test/out/
pio run -e native -t exec     # live scrolling preview, no hardware needed
pio run -e esp32s3 -t upload  # flash the device
```

Most of the code — every widget, all layout, the scroll engine — compiles and runs on your laptop with pixel-exact PNG output. You only need hardware for the panel itself and for anything timing-related.

## Documentation

- [`PLAN.md`](PLAN.md) — the phased build plan, ~100 tasks with acceptance criteria
- [`AGENTS.md`](AGENTS.md) — architecture, hard rules, memory budget, conventions
- [`docs/arduinojson-v7.md`](docs/arduinojson-v7.md) — v7 API reference for this project
- [`SPIKE_RESULTS.md`](SPIKE_RESULTS.md) — Phase 0 measurements on hardware, and the gate verdict
- [`WORKLOG.md`](WORKLOG.md) — running notes, one entry per task

## Why not a Raspberry Pi?

This started life as a working Raspberry Pi implementation in Python — Pillow for rendering, FastAPI for the web UI, the hzeller driver for the panel. It worked well, and the card designs here are ported from it.

The microcontroller version exists because a Pi is the wrong shape for an appliance that lives on a shelf:

- **Boot time.** Under a second, against roughly thirty. That's the difference between something you power-cycle at the wall and something you have to think about.
- **No SD card.** Config lives in NVS with wear levelling. The Pi version carries a whole apparatus of WAL mode, JSON snapshots and corruption recovery, written entirely to survive a failure mode that doesn't exist here.
- **Power.** ~2 W against ~7 W, and no filesystem to corrupt on an unclean shutdown.
- **Cost.** ~$10 of silicon rather than ~$50 of single-board computer, SD card and PSU.
- **No OS to fight.** No `systemd`, no `apt`, no disabling onboard audio so the LED driver can have its DMA channel back.

## A note on data sources

This project reads publicly accessible ESPN endpoints. **It is not affiliated with, endorsed by, or connected to ESPN or The Walt Disney Company in any way.** The endpoints are undocumented and unofficial; they may change or disappear without notice. Team names and logos are the property of their respective owners. Please be a good API citizen — the polling intervals and caching in this firmware are deliberately conservative, and you should not lower them.

## Licence

**GPL-3.0-only** — see [`LICENSE`](LICENSE).

Copyleft is deliberate. You may use, modify, sell and distribute this freely, but derivatives must stay open under the same terms, and if you ship it on hardware you must let owners install their own modified builds (GPLv3 §6). Closed-source forks are not permitted.

Dependency note: the ESP32Async fork of ESPAsyncWebServer is LGPL-3.0, which combines cleanly into GPLv3. See [`docs/DEPENDENCY-LICENCES.md`](docs/DEPENDENCY-LICENCES.md) for the full audit.
