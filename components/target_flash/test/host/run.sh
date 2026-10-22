#!/usr/bin/env bash
# Host-side regression test for the pure STM32 DEV_ID table + flash-size
# narrowing helpers (components/target_flash/src/tf_stm32_id.c).
#
# `tf_stm32_id.c` intentionally has no ESP-IDF dependency (it is pure data +
# string helpers), so this runs with the host gcc only.  No target hardware and
# no firmware build/flash are involved.
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

"$BUILD_DIR/test_stm32_id"
