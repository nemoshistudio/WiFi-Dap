#!/usr/bin/env bash
#
# Host-side test for the pure logic of the ESP32-S3 ultra-fast SWD engine
# (components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h): request-header bytes,
# parity, ACK decoding and the timing level table / level selection safety
# rules documented in docs/swd-23mhz-engine.md.
#
# Plain host compiler only: no ESP-IDF, no Docker, no target hardware.  The
# Xtensa assembly itself is compile-checked with the xtensa-esp32s3-elf
# toolchain and validated on the bench (HIL checklist in the doc).
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
