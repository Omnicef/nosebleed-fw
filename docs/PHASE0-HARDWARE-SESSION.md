# Session log — Phase 0 hardware bring-up (2026-09-07)

Self-contained record of the session that closed the Phase 0 gate on real
hardware. The authoritative measurement table lives in `SPIKE_RESULTS.md`;
this file captures the debugging history, the tooling, the environment and
the workflow gotchas that don't belong there. Read both before touching the
spike again.

**Outcome: GATE CLOSED, GO. Commit `1f7de13`. Phase 0 complete except T-0.2
(panel hello-world — needs HUB75 + 74HCT245 physically wired). No Phase 1
work started.**

---

## What happened, in order

1. **Serial output was invisible on hardware.** The firmware appeared to
   hang at boot. Root cause: the Arduino-ESP32 core defaults
   `ARDUINO_USB_CDC_ON_BOOT=0`, which maps `Serial` to UART0 (pins 43/44,
   unwired on the bench). Only IDF-console output (panics, `ESP_LOGE`)
   ever reached USB-CDC. This masqueraded as several other bugs and cost
   most of a session.
   - **Fix** (in both envs of `spike/platformio.ini`):
     `build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`
2. **T-0.1 / T-0.3 passed immediately after.** 16 MB flash, 8 MB octal
   PSRAM found and usable; WiFi + SNTP ceiling ~262 KB internal — within
   ~6 KB of the Wokwi prediction. The earlier "PSRAM is dead / fake N16R8"
   suspicion is dead; the hang was the USB init path all along.
3. **T-0.4 returned `status=0, errno=0, bytes=0`** on every ESPN request.
   Root cause: `esp_http_client_open()` does **not** read response headers —
   `esp_http_client_fetch_headers()` (esp_http_client.h:643) must be called
   first. Verified against the installed header before fixing. This bug was
   also latent in T-0.5's code path.
4. **ESPN returned 403 (438–442 B "Access Denied")** for the device while
   the laptop got 200 with the same URL. Isolation matrix on the laptop:
   `curl/*` and `python-requests/*` UAs pass; `Mozilla/5.0`, full Chrome
   UA, `ESP32_HTTP_Client`, custom and empty UAs all 403 — over plain HTTP
   too, so **not** JA3/TLS fingerprinting. Akamai runs a User-Agent prefix
   allowlist. Firmware now sends `python-requests/2.31` (the agent the
   Python original uses).
   - **Production watch-item:** if ESPN tightens this, gzip/proxying
     becomes a dependency, not an optimization (see SPIKE_RESULTS.md).
5. **The 403s then looked path-specific** (NFL passed, MLB failed) for two
   more boots. Actual cause: a leftover duplicate
   `cfg.user_agent = "Mozilla/5.0";` line in `espn_get()` silently
   overwrote the working UA — NFL went through `probe()` (correct UA),
   MLB through `espn_get()` (wrong UA). **Lesson: each config-struct field
   assigned exactly once.**
6. **T-0.5 first completion used a LAN mirror fallback** (ESPN 403 era):
   the Marquee fixture copied to the laptop, served by
   `python3 -m http.server`, device fetches it. That run exposed an
   unexplained multi-minute stall in the mirror path (suspect
   `esp_http_client_read` blocking on short final chunks against a
   `Connection: close` server). Moot — the ESPN-direct path works — but
   documented in case the mirror is ever needed again.
7. **Final run (UA fixed): T-0.5 against the real ESPN MLB endpoint,
   6.1 s end to end.** Filtered parse cost 13.4 KB internal, peak
   simultaneous TLS+parse 73.6 KB, zero leak, live games parsed correctly
   off the wire. Full decomposition in `SPIKE_RESULTS.md`.

---

## Hardware numbers (quick reference — full table in SPIKE_RESULTS.md)

| Quantity | Value |
|---|---|
| Flash / PSRAM | 16,777,216 B / 8,388,608 B, `psramFound()` yes |
| Working ceiling (WiFi+SNTP) | ~262,384 B internal (Wokwi predicted 268,652) |
| Untuned mbedTLS session | ~56 KB (Wokwi: 54.5 KB); untunable until T-9.3 |
| T-0.5 parse delta (THE GATE) | 13,428 B internal |
| T-0.5 PSRAM retained doc | 25,112 B (live payload, fatter than fixture) |
| T-0.5 leak | 0 B |
| Peak simultaneous TLS+parse | 73,616 B |
| T-0.5 elapsed (unmitigated) | 6,081 ms vs <6 s budget; T-5.6 narrowing = ~3× |
| Boot reliability | **~1 in 3 resets boots silently** — press RESET again; carry into Phase 1 logging |

---

## Environment / workflow (for reproducing a capture session)

- **Board:** ESP32-S3-DevKitC-1 N16R8 standalone, USB-CDC only.
  Device IP 192.168.123.157, laptop 192.168.123.105.
- **Build/upload:** `pio run -d spike -e esp32s3 -t upload`
  (`esp32s3` = stock OPI env; `esp32s3-nopsram` = qio_qspi fallback env).
- **Serial capture:** `/tmp/opencode/tapwatch.py [seconds]` — watches
  `/dev/ttyACM*` for node changes, opens with DTR asserted, spams `r` to
  release the firmware's pause gate, follows re-enumerations (crash-loops
  can't hide from it). Run via `/tmp/opencode/espvenv/bin/python`.
- **The RESET dance:** arm the watcher first, then press RESET; retry the
  RESET if a boot comes up silent (see boot reliability above).
- **LAN mirror (if ESPN 403s again):** fixture at
  `/tmp/opencode/www/mlb_scoreboard.json` (copied from the READ-ONLY
  Marquee repo), served with
  `setsid python3 -m http.server 8123 --bind 192.168.123.105 --directory /tmp/opencode/www`.
  Port 8080 on the laptop is occupied by an old http.server; use 8123.
- **WiFi creds** live in `spike/src/secrets.h` (gitignored).
- **LSP diagnostics on `spike/src/main.cpp` are stale clangd false
  positives** (`Arduino.h` not found etc.). `pio run` is the truth.
- **API verification rule** held all session: every `esp_http_client` call
  was checked against
  `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/esp_http_client/include/esp_http_client.h`
  before use (`fetch_headers` :643, `get_errno` :551, `set_header` :416,
  `cfg.user_agent` :194).

---

## Current state

- Committed `1f7de13` — "Phase 0: hardware gate closed — T-0.5 pass on live
  ESPN data, SPIKE_RESULTS verdict, CDC + Akamai UA fixes". Working tree
  clean except this file.
- Board holds the final spike firmware (esp32s3 OPI build, all fixes).
- `SPIKE_RESULTS.md` updated with the hardware section + Wokwi section
  relabeled as historical.
- Spike code (`spike/src/main.cpp`) is throwaway by design; the useful bits
  (filter doc, probe pattern, UA workaround, sampler) are referenced by
  PLAN.md phases 5+.

## Next steps (when resuming)

1. **T-0.2** — panel hello-world. Requires HUB75 panel + 74HCT245 adapter
   wired; 5 V @ 4 A min PSU for one 64×32 panel, panel powered from the
   PSU directly, separate cable to the board. Acceptance: no ghosting, no
   tearing, refresh measured at `lsbMsbTransitionBit` 0 and 1.
2. **Phase 1** — skeleton, partitions, task topology, purity guard, CI
   (T-1.1 … T-1.11). Do not start until the user says so.
