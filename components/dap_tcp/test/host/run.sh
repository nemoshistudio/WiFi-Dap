#!/usr/bin/env bash
#
# Host-side regression test for the CMSIS-DAP over TCP frame codec and
# streaming parser (components/dap_tcp/src/dap_tcp_frame.c).
#
# Pure C: no ESP-IDF, no lwIP, no sockets, no target hardware.  Covers the
# 8-byte header ("DAP\0" + u16 little-endian length + type + reserved), the
# probe-side direction rules (request type 1 / control request type 3),
# half packets, sticky (pipelined) packets, invalid-header rejection, the
# 1024-byte payload limit and a deterministic random-chunking fuzz pass over
# 128 frames.  ASan+UBSan are enabled when the host compiler supports them.
#
# Usage:
#   components/dap_tcp/test/host/run.sh                 # auto-detect sanitizers
#   SANITIZE=1 components/dap_tcp/test/host/run.sh      # force sanitizers on
#   SANITIZE=0 components/dap_tcp/test/host/run.sh      # force sanitizers off
#   CC=clang components/dap_tcp/test/host/run.sh
#   BUILD_DIR=/tmp/dap-tcp-build components/dap_tcp/test/host/run.sh
#
# Exit code: 0 = all checks passed, 1 = check failure, 2 = build error.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../../../.." && pwd -P)"
CC_BIN="${CC:-cc}"
SANITIZE="${SANITIZE:-auto}"

if [ -n "${BUILD_DIR:-}" ]; then
    mkdir -p "$BUILD_DIR"
else
    BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dap_tcp_host.XXXXXX")"
    trap 'rm -rf "$BUILD_DIR"' EXIT
fi
BIN="$BUILD_DIR/test_dap_tcp_frame"

case "$SANITIZE" in
    1) use_sanitize=1 ;;
    0) use_sanitize=0 ;;
    *)
        printf 'int main(void){return 0;}\n' >"$BUILD_DIR/probe.c"
        if "$CC_BIN" -x c "$BUILD_DIR/probe.c" -fsanitize=address,undefined \
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

if ! "$CC_BIN" "${cflags[@]}" \
        -I "$ROOT/components/dap_tcp/include" \
        "$HERE/test_dap_tcp_frame.c" \
        "$ROOT/components/dap_tcp/src/dap_tcp_frame.c" \
        -o "$BIN"; then
    echo "dap_tcp frame host tests: build failed" >&2
    exit 2
fi

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$BIN"
