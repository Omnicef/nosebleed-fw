# Dependency licences

**Nosebleed is licensed GPL-3.0-only.** Every dependency must be GPLv3-compatible. Permissive (MIT, BSD, Apache-2.0) and LGPL are fine; proprietary, non-commercial, and GPLv2-only are not.

This table was walked against the actual `.pio/libdeps/` tree at **T-1.9**
(versions from each `library.properties`, licences from the shipped licence
text). Deps not yet added to `platformio.ini` are marked with the phase that
pulls them in.

## Runtime dependencies

| Dependency | Version | Licence | GPLv3-compatible | Confirmed |
|---|---|---|---|---|
| `ESP32-HUB75-MatrixPanel-DMA` (mrcodetastic) | 3.0.15 | MIT | ✅ | `library.properties`; added Phase 2 (T-2.8) |
| `ArduinoJson` | 7.4.3 | MIT | ✅ | `library.properties` (spike libdeps); added Phase 5 |
| `StreamUtils` (bblanchon) | 1.9.2 | MIT | ✅ | `library.properties` (spike libdeps); added Phase 5 |
| `ESPAsyncWebServer` (ESP32Async fork) | not yet added | **LGPL-3.0** | ✅ | **Verified** — repo `LICENSE`; added Phase 8 (T-8.1) |
| `Adafruit_GFX` | **not linked** | BSD | ✅ | We build the panel lib with `-DNO_GFX` (T-0.2); GFX never linked. Row kept only because the driver is GFX-*compatible*, not because we depend on it. |
| Arduino-ESP32 core | 3.3.11 | **LGPL-2.1-or-later** | ✅ | **Resolved** — `package.json` `license` field **and** `Arduino.h` header both say *"version 2.1 … or (at your option) any later version"* → upgrades cleanly to GPL-3.0. Was the one open question. |
| ESP-IDF (via Arduino core) | 5.x | Apache-2.0 | ✅ (v3 only) | Core `esp32-hal` headers carry `SPDX-License-Identifier: Apache-2.0`; reconfirmed at the T-9.3 IDF conversion |

## Bundled assets

| Asset | Licence | Note |
|---|---|---|
| `assets/fonts/spleen-5x8.bdf`, `spleen-6x12.bdf` | BSD-2-Clause | **Confirmed** from `LICENSE.spleen.txt` (Frederic Cambus). BSD-2-Clause is GPLv3-compatible. Ship the licence alongside. |
| `assets/fonts/tom-thumb.bdf` | MIT | **Confirmed** from `LICENSE.tom-thumb.txt` (Robey Pointer). Ship the licence alongside. |

`tools/build_fonts.py` converts these BDFs into C glyph tables compiled into the firmware. **The attribution obligation follows the glyphs into the binary** — carrying the licence files over from the Python project is not optional.

Team logos are fetched from ESPN at build time by `tools/build_logos.py` and packed into `logos.bin`. They are trademarks of their respective clubs and leagues, are not covered by this project's licence, and are not redistributed in this repository.

## Notes worth keeping

**ESP-IDF is Apache-2.0, which is compatible with GPLv3 but *not* GPLv2.** This is the concrete reason Nosebleed is GPL-3.0 rather than GPL-2.0. Do not "simplify" to v2.

**ESPAsyncWebServer is LGPL-3.0 and this project links it statically.** LGPL §4 would normally require shipping object files or relinking instructions so a user can swap in a modified library. Publishing the full source under GPLv3 satisfies that. Note this obligation would exist *even under a permissive licence* — it is a consequence of the dependency, not of our choice.

**Arduino-ESP32 core is LGPL-2.1-or-later — resolved at T-1.9.** Both the
core `package.json` (`license: LGPL-2.1-or-later`) and the `Arduino.h` header
(*"either version 2.1 of the License, or (at your option) any later version"*)
carry the later-version clause, so it links cleanly into this GPL-3.0-only
work. The open question from the design doc is closed; nothing to ship.

**Non-commercial licences were never an option.** LGPL code cannot be relicensed under PolyForm Noncommercial or Commons Clause. Choosing ESPAsyncWebServer foreclosed that path regardless of preference.

## Adding a dependency

1. Check the licence *before* adding it to `platformio.ini`.
2. Reject anything proprietary, non-commercial, GPLv2-only, or unlicensed.
3. Add a row here with version and how you confirmed it.
4. If it ships its own licence text, vendor that file alongside.

An unlicensed dependency is not "probably fine" — with no licence grant, the default is no permission at all.
