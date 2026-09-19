# SPDX-License-Identifier: GPL-3.0-only
#
# T-9.3 — Arduino-as-ESP-IDF-component. The build of record: ESP-IDF 5.5.5
# with the Arduino core (3.3.11) as a managed component. PlatformIO keeps
# only env:native (host render tests). See WORKLOG T-9.3.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Serial on USB-JTAG CDC. Without these the core routes Serial to UART0
# (pins 43/44, unwired on this devkit) and every println is lost — the same
# -D flags platform.txt build.extra_flags passed to every component,
# including the Arduino core's own sources.
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_MODE=1;-DARDUINO_USB_CDC_ON_BOOT=1" APPEND)

# The Arduino core puts -Werror=all on every component. Keep that — it is the
# cheap guard against signature slips — but do not let GCC 14's inliner
# guesswork inside vendored StreamUtils (CharArray.hpp under
# StreamProxy/ReadBufferingStream) fail the build: the warning still prints,
# it just stays a warning. Our own code is fixed at the source instead.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)

project(nosebleed-fw)

# T-8.2 web SPA partition. tools/pack_web.py gzips assets/web/*.html into
# data/ (it reads PROJECT_DIR); spiffsgen then packs data/ into the 'web'
# partition image. pack_web must run before the image is built, and the
# image rebuilds on every build (spiffs_web_bin is an ALL target), so a
# stale gz can never ship.
add_custom_target(pack_web ALL
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/tools/pack_web.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
)
spiffs_create_partition_image(web ${CMAKE_CURRENT_LIST_DIR}/data)

# spiffs_web_bin only exists after the call above; pack_web must run first.
add_dependencies(spiffs_web_bin pack_web)
