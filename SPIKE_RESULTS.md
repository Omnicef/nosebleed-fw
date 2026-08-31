# SPIKE_RESULTS.md — Phase 0 (T-0.1 … T-0.7)

Measured in **Wokwi** on the exact part (`board-esp32-s3-devkitc-1`, `flashSize: 16`, `psramSize: 8`, `psramType: octal`), 2026-08-31. All throwaway firmware lives in `spike/`. These are **real boot-log numbers, not estimates.**

Wokwi's two hard limits apply to everything below (PLAN.md §4): **no HUB75 output**, and **wall-clock timing is meaningless** (simulated CPU capped near 8 MHz). Heap figures are reasonably faithful; timings are not.

---

## THE GATE — three-way verdict (T-0.5)

| Half | Status | Evidence |
|---|---|---|
| **Memory** (filtered parse cost) | **PASS** (conditional) | filtered parse = **1.6 KB** internal; retained doc **5,676 B raw / 10.5 KB PSRAM** from a 1.46 MB input |
| **Streaming path** | **UNTESTED** | `ReadBufferingStream` over a real TLS stream never exercised; **peak simultaneous mbedTLS + active parse** unknown (measured separately at T-0.4 and T-0.5) |
| **Timing** (< 6 s per MLB poll) | **UNTESTED** | Wokwi CPU cap makes wall-clock meaningless |

**Go/no-go: GATE OPEN.** The memory half — the half that decides whether the architecture is viable — passes decisively. Do **not** read that as a full pass: the streaming and timing halves close on hardware. Proceed to Phase 1; Phase 0 is not "done" until the deferred set (below) runs on the board.

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

## Deferred to hardware (the day the board arrives)

- **T-0.2** — panel hello-world (no HUB75 in Wokwi; hardware only).
- **T-0.5 timing** — the < 6 s budget.
- **Streaming path** — `ReadBufferingStream` over a real TLS socket, and the **peak simultaneous** internal usage of an mbedTLS session *and* an active parse (T-0.4 and T-0.5 measured these separately).
- **T-0.6 tuning** — re-run after the T-9.3 IDF conversion, where the mbedTLS Kconfig actually takes effect.

---

*Verdict recorded per PLAN.md §4: split verdict, gate open until hardware closes the streaming and timing halves. Do not let the green memory number read as a full pass.*
