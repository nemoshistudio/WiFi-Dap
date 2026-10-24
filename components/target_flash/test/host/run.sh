#!/usr/bin/env bash
# Host-side regression tests for the pure STM32 DEV_ID table, the flash-size
# narrowing helpers and the page/sector geometry used by the new family flash
# drivers (components/target_flash/src/tf_stm32_id.c + tf_stm32_geom.c), plus
# the P1-19 /api/discover JSON formatter (tf_discover_core.c).
#
# The sources intentionally have no ESP-IDF dependency (pure data/geometry/
# formatting helpers), so this runs with the host gcc only.  No target
# hardware and no firmware build/flash are involved.
#
# Usage: components/target_flash/test/host/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../../../.." && pwd -P)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

gcc -std=c99 -Wall -Wextra -Werror -O2 \
    -I "$ROOT/components/target_flash/src" \
    "$HERE/test_stm32_id.c" \
    "$ROOT/components/target_flash/src/tf_stm32_id.c" \
    -o "$BUILD_DIR/test_stm32_id"

gcc -std=c99 -Wall -Wextra -Werror -O2 \
    -I "$ROOT/components/target_flash/src" \
    "$HERE/test_stm32_geom.c" \
    "$ROOT/components/target_flash/src/tf_stm32_geom.c" \
    "$ROOT/components/target_flash/src/tf_stm32_id.c" \
    -o "$BUILD_DIR/test_stm32_geom"

# P1-19 discovery JSON formatter (no mDNS/ESP-IDF; esp_err stub under mock/).
gcc -std=gnu11 -Wall -Wextra -Werror -O2 \
    -I "$HERE/mock" \
    -I "$ROOT/components/target_flash/include" \
    "$HERE/test_discover.c" \
    "$ROOT/components/target_flash/src/tf_discover_core.c" \
    -o "$BUILD_DIR/test_discover"

"$BUILD_DIR/test_stm32_id"
echo
"$BUILD_DIR/test_stm32_geom"
echo
"$BUILD_DIR/test_discover"
