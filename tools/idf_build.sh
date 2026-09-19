#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# T-9.3: one entry point for the seven build targets that used to be
# PlatformIO envs. Each env gets its own build dir (and therefore its own
# sdkconfig) so the debug logostest layer can't bleed into the others.
#
#   tools/idf_build.sh <env> <idf.py args...>     e.g. ... firmware build
#                                                 e.g. ... cardtest build flash
set -euo pipefail
cd "$(dirname "$0")/.."

env_name="${1:-firmware}"; shift || true

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
        echo "usage: $0 firmware|logostest|configtest|paneltest|httptest|cachetest|cardtest [idf.py args...]" >&2
        exit 2
        ;;
esac

exec idf.py -B "$dir" \
    -DSDKCONFIG="$PWD/$dir/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$defs" \
    -DNB_TARGET="$env_name" \
    "$@"
