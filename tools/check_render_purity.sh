#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# T-1.6 — render-layer purity guard.
#
# lib/render/ must be framework-agnostic C++ that compiles for both the device
# and the host. That is what keeps the render tests runnable on a laptop and
# what makes the Phase 9 Arduino -> ESP-IDF conversion a config change rather
# than a rewrite.
#
# Run from the repo root. Also wired as a PlatformIO pre-action and a CI job.

set -uo pipefail

TARGET="lib/render"

if [ ! -d "$TARGET" ]; then
  echo "purity: $TARGET does not exist yet — nothing to check (pre-Phase-2)"
  exit 0
fi

# Forbidden: Arduino/ESP framework headers and Arduino's String type.
# Kept as separate patterns so the failure message names the actual offender.
declare -a PATTERNS=(
  '#[[:space:]]*include[[:space:]]*[<"]Arduino\.h[>"]'
  '#[[:space:]]*include[[:space:]]*[<"]esp_[A-Za-z0-9_]*\.h[>"]'
  '#[[:space:]]*include[[:space:]]*[<"]freertos/'
  '#[[:space:]]*include[[:space:]]*[<"]WiFi\.h[>"]'
  '#[[:space:]]*include[[:space:]]*[<"]nvs[A-Za-z0-9_]*\.h[>"]'
  '\bString[[:space:]]+[A-Za-z_]'
)

declare -a LABELS=(
  'Arduino.h'
  'esp_*.h'
  'freertos/*'
  'WiFi.h'
  'nvs*.h'
  'Arduino String type'
)

fail=0
for i in "${!PATTERNS[@]}"; do
  if hits=$(grep -rnE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.cc' \
              "${PATTERNS[$i]}" "$TARGET" 2>/dev/null); then
    echo "purity: FORBIDDEN — ${LABELS[$i]} in $TARGET"
    echo "$hits" | sed 's/^/    /'
    echo
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  cat <<'EOF'
purity: FAILED

lib/render/ must not depend on Arduino, ESP-IDF or FreeRTOS. See AGENTS.md,
"Hard rules". If you need platform behaviour in a widget, inject it through an
interface defined in lib/render/ and implement it outside.
EOF
  exit 1
fi

echo "purity: OK — $TARGET is framework-agnostic"
