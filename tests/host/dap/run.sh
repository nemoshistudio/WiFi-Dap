#!/usr/bin/env bash
#
# DAP command-layer host regression (N1 0x7E recursion / N6 SWJ_Pins wait /
# N9 TransferBlock bounds). Compiles the repository's DAP.c against the stub
# DAP_config.h + FreeRTOS shims in this directory, with ASan+UBSan by default.
#
# Source: adapted from dap-dev's ASan harness in /tmp/dap_host (36/36 checks at
# the time, 51/51 after the 0x7F/0x7E batch cases were added). See
# tests/host/dap/README.md and docs/host-test-report.md for provenance.
#
# Usage:
#   tests/host/dap/run.sh                 # ASan+UBSan build + run
#   SANITIZE=0 tests/host/dap/run.sh      # plain build (no sanitizer runtime)
#   CC=clang tests/host/dap/run.sh
#   BUILD_DIR=/tmp/dap-build tests/host/dap/run.sh
#
# Exit code: 0 = all harness checks passed, 1 = check failure, 2 = build error.
# Also invoked by scripts/test_host.sh as the "DAP harness" suite.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_DIR="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$HOST_DIR/build/dap}"
CC_BIN="${CC:-cc}"
SANITIZE="${SANITIZE:-1}"

# The harness is upstream (dap-dev) code kept unchanged; compile without -Wall
# so its provenance/verification state stays reproducible (no warning noise).
cflags=(-std=gnu11 -O1 -g -fno-omit-frame-pointer)
if [ "$SANITIZE" = "1" ]; then
    cflags+=(-fsanitize=address,undefined)
fi

mkdir -p "$BUILD_DIR"
BIN="$BUILD_DIR/test_dap"

if ! "$CC_BIN" "${cflags[@]}" -I"$HERE" -I"$ROOT/components/DAP" \
        "$HERE/harness.c" \
        "$ROOT/components/DAP/cmsis-dap/source/DAP.c" \
        -o "$BIN"; then
    echo "DAP host harness: build failed" >&2
    exit 2
fi

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$BIN"
