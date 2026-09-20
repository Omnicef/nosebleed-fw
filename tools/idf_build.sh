#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# T-9.3: one entry point for the seven build targets that used to be
# PlatformIO envs. Each env gets its own build dir (and therefore its own
# sdkconfig) so the debug logostest layer can't bleed into the others.
#
#   tools/idf_build.sh <env> [--trace] <idf.py args...>   e.g. ... firmware build
#                                                         e.g. ... cardtest build flash
#                                                         e.g. ... firmware --trace build flash  (D-2 diag)
# --trace arms NB_D2_TRACE (page/commit markers on Serial). The cache var is
# re-forced on EVERY run, so a trace build can never leak into a later plain
# build of the same dir.
set -euo pipefail
cd "$(dirname "$0")/.."

env_name="${1:-firmware}"; shift || true

trace=OFF
rest=()
for a in "$@"; do
    if [[ "$a" == "--trace" ]]; then trace=ON; else rest+=("$a"); fi
done

case "$env_name" in
    firmware)
        dir=build
        defs="sdkconfig.defaults"
        ;;
    logostest)
        dir=build-logostest
        defs="sdkconfig.defaults;sdkconfig.logostest"
        ;;
    configtest|paneltest|httptest|cachetest|cardtest)
        dir="build-$env_name"
        defs="sdkconfig.defaults"
        ;;
    *)
        echo "usage: $0 firmware|logostest|configtest|paneltest|httptest|cachetest|cardtest [--trace] [idf.py args...]" >&2
        exit 2
        ;;
esac

exec idf.py -B "$dir" \
    -DSDKCONFIG="$PWD/$dir/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$defs" \
    -DNB_TARGET="$env_name" \
    -DNB_D2_TRACE="$trace" \
    "${rest[@]}"
