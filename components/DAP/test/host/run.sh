#!/usr/bin/env bash
#
# Host-side tests for the ESP32-S3 dedicated-GPIO SWD engine
# (components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h/.c, M5: all SWD rates):
#
#   1. test_swd_ultra_plan.c  - request-header bytes, parity, ACK decoding,
#      fast/paced level plan for 100 kHz .. 24 MHz (never faster than the
#      request, within -25 %, monotone) and the timing-budget arithmetic.
#   2. test_swd_ultra_routing.py - source-level routing contract: S3 SWD_Transfer
#      / SWJ_Sequence / SWD_Sequence use the engine, the legacy SWD transfer
#      backends are compiled out for S3, Kconfig defaults (engine on, MIN_CLOCK
#      deprecated to 0) and the DAP.c wiring status.
#
# Plain host compiler / Python only: no ESP-IDF, no Docker, no target hardware.
# The Xtensa assembly itself is compile-checked with the xtensa-esp32s3-elf
# toolchain and validated on the bench (HIL checklist in
# docs/swd-23mhz-engine.md).
#
# Usage:
#   components/DAP/test/host/run.sh                # build + run
#   CC=clang components/DAP/test/host/run.sh
#   SANITIZE=0 components/DAP/test/host/run.sh     # no ASan/UBSan
#
# Exit code: 0 = all checks passed, 1 = check failure, 2 = build error.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../../../.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/dap-swd-ultra-test.XXXXXX")}"
CC_BIN="${CC:-cc}"
SANITIZE="${SANITIZE:-auto}"

case "$SANITIZE" in
    1) use_sanitize=1 ;;
    0) use_sanitize=0 ;;
    *)
        if printf 'int main(void){return 0;}\n' >"$BUILD_DIR/probe.c" 2>/dev/null &&
           "$CC_BIN" -fsanitize=address,undefined "$BUILD_DIR/probe.c" \
               -o "$BUILD_DIR/probe" >/dev/null 2>&1 &&
           "$BUILD_DIR/probe" >/dev/null 2>&1; then
            use_sanitize=1
        else
            use_sanitize=0
        fi
        ;;
esac

cflags=(-std=c11 -O2 -g -Wall -Wextra -Werror -fno-omit-frame-pointer)
if [ "$use_sanitize" -eq 1 ]; then
    cflags+=(-fsanitize=address,undefined)
fi

mkdir -p "$BUILD_DIR"
BIN="$BUILD_DIR/test_swd_ultra_plan"

if ! "$CC_BIN" "${cflags[@]}" \
        -I "$ROOT/components/DAP/cmsis-dap/source" \
        "$HERE/test_swd_ultra_plan.c" \
        -o "$BIN"; then
    echo "SWD ultra-fast plan test: build failed" >&2
    exit 2
fi

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$BIN"
c_rc=$?

PYTHON_BIN="${PYTHON:-python3}"
if command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    "$PYTHON_BIN" "$HERE/test_swd_ultra_routing.py"
    py_rc=$?
else
    echo "python3 not found: routing contract test skipped" >&2
    py_rc=0
fi

if [ "$c_rc" -ne 0 ] || [ "$py_rc" -ne 0 ]; then
    exit 1
fi
exit 0
