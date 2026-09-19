# SPDX-License-Identifier: GPL-3.0-only
#
# Header-only IDF wrapper for StreamUtils v1.9.2 (MIT, bblanchon). The repo's
# own CMakeLists is its host-test harness, not an IDF component file, so this
# thin component is the only way in — see README.md for provenance. The only
# API used is ReadBufferingStream (lib/data/espn.cpp, src/envs/http_test.cpp).
idf_component_register(
    INCLUDE_DIRS "src"
    REQUIRES espressif__arduino-esp32
)
