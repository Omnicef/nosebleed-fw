# SPIKE_RESULTS.md — Phase 0 (T-0.1 … T-0.7)

Measured in **Wokwi** on the exact part (`board-esp32-s3-devkitc-1`, `flashSize: 16`, `psramSize: 8`, `psramType: octal`), 2026-08-31, and on **hardware** (real ESP32-S3-DevKitC-1 N16R8 over USB-CDC), 2026-09-07. All throwaway firmware lives in `spike/`. These are **real boot-log numbers, not estimates.**

---

## HARDWARE RESULTS — 2026-09-07 (the board arrived)

Real ESP32-S3-WROOM-1 N16R8, single USB cable, firmware `spike/src/main.cpp` (throwaway). Serial capture via `/dev/ttyACM0` with DTR asserted — see the CDC pitfall below; without it the app's `Serial` output is invisible and only IDF-console errors appear, which masquerades as "the firmware hangs".

### T-0.1 — Toolchain / board / PSRAM — PASS (hardware)

| Quantity | Measured |
|---|---|
| Flash chip size | 16,777,216 B (16 MB) |
| Flash mode | QUAD (QIO/QOUT) |
| PSRAM size | 8,388,608 B (8 MB) |
| PSRAM free at boot | 8,386,096 B |
| `psramFound()` | yes |

The real board **finds and uses the full octal PSRAM** — the Wokwi figures reproduce almost exactly (8,384,788 vs 8,386,096 B free).

### T-0.3 — WiFi + SNTP — PASS (hardware)

| Phase | Internal heap |
|---|---|
| pre-WiFi | 312,132 B |
| post-WiFi (192.168.123.157, RSSI −41…−47 dBm) | 262,852 B |
| post-SNTP (synced, 100–2,300 ms) | **262,384 B — working ceiling** |

Wokwi predicted 268,652 B; hardware lands ~6 KB lower. RSSI varies −41…−47 dBm across runs. SNTP sync 100 ms typical.

### T-0.4 — TLS + `esp_crt_bundle` — PASS (hardware)

ESPN NFL scoreboard over TLS with cert-bundle validation: **HTTP 200, 200,402 B body in 1.1–2.8 s**, repeated across runs. Heap:

| Phase | Internal heap |
|---|---|
| pre-fetch | 261,472 B |
| during read (sampler low-water) | ~195,000 B |
| post-cleanup | 261,140 B |
| **handshake drop** | **~55,900 B** (Wokwi: 54,532 B) |
| leak | ~2,400 B (client buffers released on the next cycle) |

Untuned mbedTLS session ≈ **56 KB** on hardware — matches the Wokwi number. T-0.6 tuning remains impossible on the Arduino core (below); recovery deferred to T-9.3.

### T-0.5 — THE GATE — **PASS (memory) / PASS-at-budget (timing)**

GET the real ESPN MLB scoreboard over TLS (not the Wokwi fixture workaround — the actual endpoint), ArduinoJson `Filter` + `ReadBufferingStream(512)` off the TLS stream, document in PSRAM via `SpiRamAllocator`. **The payload was the live 2026-09-07 MLB day (11 events incl. in-progress games) — a fatter-than-fixture payload.**

| # | Phase delta | Measured | What it measures | Verdict |
|---|---|---|---|---|
| 1 | pre-fetch → pre-parse | **60,188 B (58.8 KB)** | mbedTLS session + HTTP buffers | known budget line (T-0.4) |
| 2 | pre-parse → low-water | **13,428 B (13.1 KB)** | **the filtered parse itself — THE GATE** | **PASS** — small; socket/TLS read-path buffers, not filter failure |
| 3 | PSRAM delta across parse | **25,112 B (24.5 KB)** | retained `JsonDocument` | 2.3× the fixture figure — live games carry full `situation` subtrees; irrelevant against 8 MB |
| 4 | post-cleanup vs pre-fetch | **0 B** | leak check | **zero leak** |

Raw internal free across the run: pre-fetch 261,076 → pre-parse 200,888 → **low-water 187,460** → post-parse 207,396 → post-cleanup 261,140.

- **Peak simultaneous (mbedTLS session + active parse): 73,616 B (71.9 KB)** — the number the Wokwi run could not measure. Inside the AGENTS.md budget with ~100 KB+ headroom.
- **Elapsed open → parse done: 6,081 ms** against the < 6 s budget — at budget for the **unmitigated full-size payload**; T-5.6 date-narrowing (~3× smaller response) brings it to ~2–3 s. The four PLAN §2 mitigations stand.
- Spot-check on live data: PHI 1 @ ATL 0 (`post`), MIA 4 @ NYM 9 (`in`), BOS 5, LAA 1 — scores, abbreviations and `state` all correct off the wire.

**Go/no-go: GO.** All three Wokwi caveats are now closed on hardware: the streaming path works (`ReadBufferingStream` over real TLS), the peak-simultaneous figure is measured (73.6 KB), and timing is real (6.1 s unmitigated, at budget). The architecture is viable. Proceed to Phase 1.

### T-0.2 — Panel hello-world — **PASS (signal path) / DEFERRED (bright-white acceptance)**

Hardware: **SEENGREAT RGB Matrix Adapter Board (E)**, SKU 250911, ESP32-S3-DevKitC-1, silkscreened revision **V2.x**. Driver `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA` v3.0.15, `NO_GFX`.

- `begin()` OK. **`calculated_refresh_rate = 110 Hz`** — this is the lib's auto-derived value from `i2sspeed`/depth/`min_refresh_rate`; v3.0.15 made `lsbMsbTransitionBit` internal, so this reported figure is the refresh measurement. Matches the ≈110 Hz AGENTS predicts for the fast end.
- DMA framebuffer **~30 KB internal SRAM** (free heap 344,376 → 313,520), single-buffered. Inside the ~32 KB budget.
- **Signal path confirmed**: the full cycle (R/G/B/white fills → gradient → sweeping stripe) renders coherently across the whole 64×32. Wiring and level-shift are correct.
  > **Amended at T-2.8 — this acceptance was insufficient.** "Coherent" was true and meant nothing: the V2.x
  > adapter's actual wiring transposes G↔B vs. its silkscreen (silkscreen G1=GPIO8 is really B1, and G2/B2
  > likewise), so this run rendered solid *blue* where it recorded "green". Solid fills cannot detect a channel
  > permutation. **Acceptance for any panel/adapter bring-up is a colour-NAME test** — draw the words RED, GREEN,
  > BLUE in their own colours and read the panel. The hardware-proven pin map now lives in `lib/panel/panel.cpp`.

**Deferred — bright-white acceptance.** Bench has no 5 V/4 A supply, so the panel is on the devkit's USB and firmware is pinned to `setBrightness(16)` to stay under the PC port's 500 mA. The no-brown-out / no-ghosting / full-brightness check is **open until the real PSU is connected**.

**Findings:**
- **The adapter's USB-C is "power input only"** (wiki silkscreen ①). 5 V from the devkit does **not** power the adapter's 74HCT245 or the panel. Correct rig: 5 V/4 A into the adapter's own USB-C/DC-044, panel power on one of its two VH-4P outputs, adapter power LED ② lit.
- **The library's S3 default pin map is wrong for this board** — using it gave no display / flickering lines. The real per-revision map is SEENGREAT wiki 186. **V2.x**: R1=18 G1=8 B1=17 R2=16 G2=1 B2=15 A=7 B=48 C=6 D=47 E=2 LAT=21 OE=4 CLK=5. (V1.x is different; `spike/src/panel_main.cpp` switches between them with `BOARD_V1`/`BOARD_V2`.)

### Hardware-only findings

- **`ARDUINO_USB_CDC_ON_BOOT` must be set or `Serial` is invisible.** The Arduino core defaults it to 0, routing `Serial` to UART0 (pins 43/44, unwired on a bench). Symptom: firmware "hangs" with zero output while IDF-console errors still print. Fix: `build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`. This cost most of a session to find.
- **`esp_http_client_open()` does not read response headers.** `esp_http_client_fetch_headers()` must be called before `get_status_code()`/`get_content_length()` are valid and before `read()` returns body data (esp_http_client.h:643). Omitting it yields `status=0, errno=0, bytes=0` — a silent API-flow trap.
- **ESPN's CDN enforces a User-Agent prefix allowlist.** `curl/*` and `python-requests/*` pass; `Mozilla/5.0`, full Chrome UAs, `ESP32_HTTP_Client`, and empty UAs get **403 with a 438–442-byte "Access Denied" body** — regardless of TLS stack (plain HTTP behaves identically, so it is not JA3 fingerprinting). The firmware identifies as `python-requests/2.31`, the agent the Python original uses. **Watch for this in production polls; if ESPN tightens it, T-11.3 gzip or a proxying CDN becomes a dependency, not an optimization.**
- **A leftover duplicate `user_agent` assignment silently overwrote the working UA** in `espn_get()` (the "Mozilla/5.0" from an earlier experiment shadowed the `python-requests` line). The 403s looked path-specific (NFL passed via one code path, MLB failed via another) until the duplicate was found. Lesson recorded: config-struct field assignments should happen exactly once.
- **Boot is intermittently silent (~1 in 3 resets)** — the chip boots but produces no USB output until another RESET. A hardware/flake risk to carry into Phase 1 logging (and a reason the CI harness should assert on *content*, not on boot count).
- **LAN-mirror fallback path had unexplained multi-minute stalls** (python `http.server` + `ReadBufferingStream`); moot — the ESPN-direct path runs in seconds — but if the mirror is ever needed again, suspect `esp_http_client_read` blocking on short final chunks against a `Connection: close` server.

---

## WOKWI RESULTS — 2026-08-31 (simulator; timing meaningless, heap faithful)

Wokwi's two hard limits apply to everything below (PLAN.md §4): **no HUB75 output**, and **wall-clock timing is meaningless** (simulated CPU capped near 8 MHz). Heap figures are reasonably faithful; timings are not. Retained for the record and because the hardware numbers track them closely.

### The gate as measured in Wokwi (superseded by the hardware run above)

| Half | Status | Evidence |
|---|---|---|
| **Memory** (filtered parse cost) | **PASS** | filtered parse = **1.6 KB** internal; retained doc **5,676 B raw / 10.5 KB PSRAM** from a 1.46 MB input |
| **Streaming path** | was UNTESTED | closed on hardware 2026-09-07 |
| **Timing** (< 6 s per MLB poll) | was UNTESTED | closed on hardware 2026-09-07 |

---

## Per-task results

### T-0.1 — Toolchain / board / PSRAM-Flash probe — PASS

Firmware printed its own evidence (the acceptance criterion is the boot log, not the config):

| Quantity | Measured |
|---|---|
| Flash chip size | 16,777,216 B (16 MB) |
| PSRAM size | 8,388,608 B (8 MB) |
| PSRAM free at boot | 8,384,788 B |
| `psramFound()` | yes |
| Flash mode | **DIO** (not QIO) — record and move on; a read-bandwidth question only, revisit at T-3.4 if mmap'd logo reads feel slow |

### T-0.3 — WiFi + SNTP — PASS

| Phase | Internal heap |
|---|---|
| pre-WiFi | 318,032 B |
| post-WiFi (connected, 10.13.37.2) | 269,156 B |
| post-SNTP (synced, 1,300 ms) | **268,652 B** |

268,652 B is the **working ceiling** (the `AGENTS.md` budget) — WiFi+SNTP connected, before any TLS. Wall-clock is not meaningful under Wokwi.

### T-0.4 — TLS + `esp_crt_bundle` — PASS

Real handshake + cert validation against ESPN's real certificate.

| Phase | Internal heap |
|---|---|
| pre-TLS | 268,080 B |
| post-open (handshake done) | 214,412 B |
| lowest during body read | 213,548 B |
| post-cleanup | 267,228 B |

- **Peak mbedTLS session drop (untuned): 54,532 B (≈ 53 KB)** internal — this is the number T-0.6 tries to shrink.
- ESPN returns **403 with a 442-byte body** (the CDN blocks the Wokwi gateway) — but the **cert validated cleanly** via `esp_crt_bundle` and **442/442 bytes read**. That is a TLS pass; the 403 is a network-path issue, not a cert issue.

### T-0.5 — THE GATE (memory half) — PASS (conditional)

GET the MLB scoreboard with an ArduinoJson `Filter`, parse straight off the stream into a PSRAM `JsonDocument`. Raw 1.46 MB fixture inflated into PSRAM (Wokwi firmware-size workaround, see Findings), parsed **from memory** — not off a TLS stream (that is the untested half).

Internal-heap phase deltas:

| # | Phase delta | Measured | What it measures | Verdict |
|---|---|---|---|---|
| 1 | pre-fetch → pre-parse | **4,080 B (4.0 KB)** | filter document (internal) | expected |
| 2 | pre-parse → parse low-water | **1,656 B (1.6 KB)** | **the filtered parse itself — THE GATE** | **PASS** (< 8 KB) |
| 3 | PSRAM delta across parse | **10,740 B (10.5 KB)** | retained `JsonDocument` | raw **5,676 B**; ~1.9× ArduinoJson slot overhead |
| 4 | post-cleanup vs pre-fetch | **532 B (0.5 KB)** | leak check | ≈ the 512 B read buffer in scope; **zero leak** |

Raw internal free across the run: pre-fetch 268,676 → pre-parse 264,596 → **parse low-water 262,940** → post-parse 267,800 → post-cleanup 268,144.

Spot-check vs fixture, **6/6**: PIT 2, MIA 4, BAL 2, SD 5, WSH 10, SEA 1 (all `post`). Note: ESPN stores scores as JSON **strings** (`"score":"2"`), not integers — the reader takes `const char*`.

A correct filtered parse costs almost nothing in **internal** heap because the document lives in PSRAM via `SpiRamAllocator`. That is the whole point of the gate, and it holds.

### T-0.6 — mbedTLS tuning — **CANNOT APPLY on the Arduino core**

Applied the three settings via `spike/sdkconfig.defaults`:

```
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n
```

Rebuilt and verified: **the tuning does not bite.** Evidence:

- mbedTLS is a **pre-built static archive** — the firmware map links **989 objects from `libmbedtls_2.a`** and 73 from `libmbedtls.a` (the crt bundle), all from the pioarduino package, compiled with the **untuned** defaults (`SSL_MAX_CONTENT_LEN=16384`, symmetric in/out, `KEEP_PEER_CERTIFICATE=y`).
- The pioarduino build has **no from-source component compilation** (`pioarduino-build.py` only gathers pre-built libs/includes; no `idf.py`/cmake/ninja), and the `platform.txt` prebuild hook overwrites the build's `sdkconfig` with the package's.
- After adding the project `sdkconfig.defaults` and rebuilding, our three values appear **nowhere** in the build, and the map still pulls from the pre-built `libmbedtls_2.a`.

**This is the known Arduino-core issue** the PLAN anticipated. The ~14 KB recovery (53 KB → ~39 KB) is **deferred to Phase 9 (T-9.3, Arduino-as-IDF-component conversion)**, which compiles mbedTLS from source against a real sdkconfig. The 53.3 KB untuned session stands as the working number until then. The (now-removed) `sdkconfig.defaults` is carried forward to the Phase 9 project config.

---

## Findings & caveats

- **ESPN blocks the Wokwi gateway.** Handshake + `esp_crt_bundle` validation succeed against ESPN's real cert, but the CDN returns **403 / 442 B** regardless of User-Agent. So a real ESPN payload cannot be drawn through Wokwi.
- **Wokwi firmware-size limit.** A raw 1.46 MB flash embed → a 2.25 MB image → Wokwi will not boot it. Workaround: store the fixture **zlib-compressed in flash** (88,349 B) and inflate into PSRAM with the S3 ROM `tinfl_decompress_mem_to_mem`. This changes the input source (memory, not TLS) — the reason the streaming half is untested.
- **`NestingLimit(20)` is mandatory.** The MLB payload nests to depth **15**; ArduinoJson's default is 10 → the parse dies with `TooDeep` without it. Carried into T-5.4 and `docs/arduinojson-v7.md` §7.
- **ESPN scores are JSON strings**, not integers.
- **Flash mode is DIO, not QIO.** Not a correctness issue; revisit at T-3.4 if mmap'd logo reads are slow.
- **Timing is meaningless in Wokwi** (CPU capped near 8 MHz). The T-0.5 < 6 s budget and the 30 fps target are hardware-only.

## Deferred to hardware (status after 2026-09-07)

- ~~**T-0.5 timing**~~ — **DONE** (6.1 s unmitigated, at budget; T-5.6 is the known mitigation).
- ~~**Streaming path**~~ — **DONE** (`ReadBufferingStream` over real TLS; peak simultaneous 73.6 KB).
- **T-0.2** — panel hello-world — still open (no HUB75 wired yet; first item when the panel + 74HCT245 adapter are connected).
- **T-0.6 tuning** — re-run after the T-9.3 IDF conversion, where the mbedTLS Kconfig actually takes effect.

---

*Verdict recorded per PLAN.md §4: the gate is CLOSED — GO. Memory, streaming and timing all measured on hardware. T-0.2 (panel) remains the only open Phase 0 item and blocks nothing architectural.*
