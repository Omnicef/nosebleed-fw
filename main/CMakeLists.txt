# SPDX-License-Identifier: GPL-3.0-only
#
# The nosebleed main component. Under PlatformIO, LDF compiled every lib/
# directory that src textually included; here the same set is registered
# explicitly. NB_TARGET selects one of the seven standalone mains (firmware +
# the six device-test envs), mirroring the old esp32s3 + six test envs.

set(NB_TARGET "firmware" CACHE STRING "build target: firmware|logostest|configtest|paneltest|httptest|cachetest|cardtest")

set_property(CACHE NB_TARGET PROPERTY STRINGS firmware logostest configtest paneltest httptest cachetest cardtest)

if(NOT NB_TARGET IN_LIST NB_TARGET__LIST AND NOT NB_TARGET MATCHES "^(firmware|logostest|configtest|paneltest|httptest|cachetest|cardtest)$")
    message(FATAL_ERROR "NB_TARGET='${NB_TARGET}' — must be firmware, logostest, configtest, paneltest, httptest, cachetest or cardtest")
endif()

# env name -> src/envs/<file>_test.cpp (empty for the shipping build)
if(NB_TARGET STREQUAL "firmware")
    set(nb_app_srcs "${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp")
else()
    # logostest -> logos_test, configtest -> config_test, paneltest -> panel_test,
    # httptest -> http_test, cachetest -> cache_test, cardtest -> card_test
    if(NB_TARGET STREQUAL "logostest")
        set(nb_test_file logos_test)
    elseif(NB_TARGET STREQUAL "configtest")
        set(nb_test_file config_test)
    elseif(NB_TARGET STREQUAL "paneltest")
        set(nb_test_file panel_test)
    elseif(NB_TARGET STREQUAL "httptest")
        set(nb_test_file http_test)
    elseif(NB_TARGET STREQUAL "cachetest")
        set(nb_test_file cache_test)
    else()
        set(nb_test_file card_test)
    endif()
    set(nb_app_srcs "${CMAKE_CURRENT_LIST_DIR}/../src/envs/${nb_test_file}.cpp")
endif()

file(GLOB nb_lib_srcs
    "${CMAKE_CURRENT_LIST_DIR}/../lib/render/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/data/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/config/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/net/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/panel/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../lib/web/*.cpp"
)

idf_component_register(
    SRCS ${nb_app_srcs} ${nb_lib_srcs}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../src"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/render"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/data"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/config"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/net"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/logos"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/panel"
        "${CMAKE_CURRENT_LIST_DIR}/../lib/web"
    REQUIRES
        espressif__arduino-esp32
        esp-tls
        nvs_flash
        spiffs
        app_update
        bblanchon__arduinojson
        esp32async__espasyncwebserver
        nb_streamutils
        esp32_hub75_matrixpanel_dma
)

# Compile flags that were platformio.ini build_flags. NO_GFX is NOT here: the
# HUB75 component propagates it PUBLIC in its own IDF-mode branch.
# ARDUINO_USB_* are global because they steer the Arduino CORE's own sources
# (Serial-over-USB-CDC), exactly like platform.txt build.extra_flags did.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    NB_FW_VERSION="0.8.0"
)

if(NB_TARGET STREQUAL "logostest")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        NB_LOGOS_TEST=1 NB_LOGOS_ASSERT=1 NB_LOGOS_TRAP_EXPECT=1)
elseif(NB_TARGET STREQUAL "configtest")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NB_CONFIG_TEST=1)
elseif(NB_TARGET STREQUAL "paneltest")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NB_PANEL_TEST=1 NB_VERSION="phase2-seam")
elseif(NB_TARGET STREQUAL "httptest")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NB_HTTP_TEST=1)
elseif(NB_TARGET STREQUAL "cachetest")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NB_CACHE_TEST=1)
elseif(NB_TARGET STREQUAL "cardtest")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NB_CARD_TEST=1)
endif()

# D-2 diagnostic: millis-stamped page transitions and strip commits on Serial.
# OFF by default and re-forced OFF by idf_build.sh on every run that doesn't
# pass --trace, so it can never ride into a shipping build via stale cache.
option(NB_D2_TRACE "print [d2] page-transition / strip-commit markers" OFF)
if(NB_D2_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NB_D2_TRACE=1)
endif()

# IDF builds C++ as gnu++23 by default; pin the project's C++17 contract.
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_EXTENSIONS ON)
