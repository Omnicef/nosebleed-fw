# Dependency licences

**Nosebleed is licensed GPL-3.0-only.** Every dependency must be GPLv3-compatible. Permissive (MIT, BSD, Apache-2.0) and LGPL are fine; proprietary, non-commercial, and GPLv2-only are not.

This table is the starting state. **T-1.9 confirms every row against the actual `.pio/libdeps/` tree** and fills in the versions — the "Confirmed" column says how each entry was established, not how confident we feel about it.

## Runtime dependencies

| Dependency | Licence | GPLv3-compatible | Confirmed |
|---|---|---|---|
| `ESP32-HUB75-MatrixPanel-DMA` (mrcodetastic) | MIT | ✅ | Reported by maintainer of this project — recheck at T-1.9 |
| `ESPAsyncWebServer` (ESP32Async fork) | **LGPL-3.0** | ✅ | **Verified** — read from the repo's `LICENSE` |
| `ArduinoJson` v7 | MIT | ✅ | Recheck at T-1.9 |
| `StreamUtils` (bblanchon) | MIT | ✅ | Recheck at T-1.9 |
| `Adafruit_GFX` | BSD | ✅ | Recheck at T-1.9 |
| Arduino-ESP32 core | LGPL-2.1 | ⚠️ verify | Recheck at T-1.9 — see note below |
| ESP-IDF | Apache-2.0 | ✅ (v3 only) | Recheck at T-1.9 — see note below |

## Bundled assets

| Asset | Licence | Note |
|---|---|---|
| `assets/fonts/spleen-5x8.bdf`, `spleen-6x12.bdf` | BSD-2-Clause (verify) | Ship `LICENSE.spleen.txt` alongside |
| `assets/fonts/tom-thumb.bdf` | verify | Ship `LICENSE.tom-thumb.txt` alongside |

`tools/build_fonts.py` converts these BDFs into C glyph tables compiled into the firmware. **The attribution obligation follows the glyphs into the binary** — carrying the licence files over from the Python project is not optional.

Team logos are fetched from ESPN at build time by `tools/build_logos.py` and packed into `logos.bin`. They are trademarks of their respective clubs and leagues, are not covered by this project's licence, and are not redistributed in this repository.

## Notes worth keeping

**ESP-IDF is Apache-2.0, which is compatible with GPLv3 but *not* GPLv2.** This is the concrete reason Nosebleed is GPL-3.0 rather than GPL-2.0. Do not "simplify" to v2.

**ESPAsyncWebServer is LGPL-3.0 and this project links it statically.** LGPL §4 would normally require shipping object files or relinking instructions so a user can swap in a modified library. Publishing the full source under GPLv3 satisfies that. Note this obligation would exist *even under a permissive licence* — it is a consequence of the dependency, not of our choice.

**Arduino-ESP32 core is LGPL-2.1.** Confirm at T-1.9 whether the notice carries an "or any later version" clause; that determines whether it upgrades cleanly to LGPL-3.0/GPL-3.0. If it is 2.1-only, check the FSF compatibility matrix before shipping binaries.

**Non-commercial licences were never an option.** LGPL code cannot be relicensed under PolyForm Noncommercial or Commons Clause. Choosing ESPAsyncWebServer foreclosed that path regardless of preference.

## Adding a dependency

1. Check the licence *before* adding it to `platformio.ini`.
2. Reject anything proprietary, non-commercial, GPLv2-only, or unlicensed.
3. Add a row here with version and how you confirmed it.
4. If it ships its own licence text, vendor that file alongside.

An unlicensed dependency is not "probably fine" — with no licence grant, the default is no permission at all.
