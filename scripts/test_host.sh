#!/usr/bin/env bash
#
# WiFi Dap -- one-shot host test runner (Wave 3 / WP-302).
#
# Runs every host-testable piece of the firmware/tooling contract without
# ESP-IDF, Docker, network access or hardware:
#
#   1. C unit tests:  tests/host/test_wifi_cred_logic.c
#                     (pure logic of project_components/wifi_manager/wifi_cred_logic.c)
#   2. DAP ASan harness: tests/host/dap/ (N1 0x7E recursion, N6 SWJ_Pins wait,
#                     N9 TransferBlock bounds; compiled from the repo DAP.c)
#   3. C unit tests:  tests/host/test_probe_identity_format.c
#                     (unique serial formatting from the factory WiFi MAC)
#   4. CMSIS-DAP over TCP frame codec:
#                     components/dap_tcp/test/host/run.sh (M3 skeleton)
#   5. SWD ultra-fast engine plan:
#                     components/DAP/test/host/run.sh (M4 level/timing model)
#   6. Python tests:  tests/host/test_protocol_frame.py   JSON/WS wire contract
#                     tests/host/test_ota_sha.py         OTA upload contract
#                     tests/host/test_partitions.py      frozen partition tables
#                     tests/host/test_security_regressions.py  N2/N8/N9 guards
#                     tests/host/test_hil_gate.py        HIL gate (CVF decode,
#                     thresholds, fake-device --dry-run scenarios)
#
# Output is TAP-ish (one "ok/not ok" line per suite + "#" diagnostics) and the
# exit code is the aggregate (0 = all suites passed).
#
# Usage:
#   scripts/test_host.sh               # auto-detect ASan/UBSan support
#   SANITIZE=1 scripts/test_host.sh    # force sanitizers on
#   SANITIZE=0 scripts/test_host.sh    # force sanitizers off
#   CC=clang PYTHON=python3.12 scripts/test_host.sh
#
# Requirements: a C11 compiler (cc/gcc/clang), make, python3 >= 3.9.
# Never builds firmware, never flashes, never touches git.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST_DIR="$ROOT/tests/host"
BUILD_DIR="$HOST_DIR/build"
CC_BIN="${CC:-cc}"
PYTHON_BIN="${PYTHON:-python3}"
SANITIZE="${SANITIZE:-auto}"

LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/birdiebot-host-tests.XXXXXX")"
cleanup() { rm -rf "$LOG_DIR"; }
trap cleanup EXIT

suite_no=0
suite_fail=0

tap_ok()   { suite_no=$((suite_no + 1)); printf 'ok %d - %s\n' "$suite_no" "$1"; }
tap_fail() { suite_no=$((suite_no + 1)); suite_fail=$((suite_fail + 1)); printf 'not ok %d - %s\n' "$suite_no" "$1"; }
diag()     { printf '# %s\n' "$*"; }

printf 'TAP version 13\n'
diag "birdiebot host tests (WP-302) -- root: $ROOT"
diag "compiler: $("$CC_BIN" --version 2>/dev/null | head -1 || echo "$CC_BIN")"
diag "python:   $("$PYTHON_BIN" --version 2>&1 | head -1)"

# ---------------------------------------------------------------------------
# 0. Toolchain checks
# ---------------------------------------------------------------------------
missing=0
for tool in "$CC_BIN" "$PYTHON_BIN"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        diag "MISSING required tool: $tool"
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    tap_fail "toolchain available"
    printf '1..%d\n' "$suite_no"
    diag "summary: suites=$suite_no failed=$suite_fail status=FAIL"
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. C unit tests -- wifi_cred_logic.c (pure, ESP-free)
# ---------------------------------------------------------------------------
case "$SANITIZE" in
    1) use_sanitize=1 ;;
    0) use_sanitize=0 ;;
    *)
        printf 'int main(void){return 0;}\n' > "$LOG_DIR/probe.c"
        if "$CC_BIN" -x c "$LOG_DIR/probe.c" -fsanitize=address,undefined \
                -o "$LOG_DIR/probe" >/dev/null 2>&1 && "$LOG_DIR/probe" >/dev/null 2>&1; then
            use_sanitize=1
        else
            use_sanitize=0
        fi
        ;;
esac
diag "sanitizers: $([ "$use_sanitize" -eq 1 ] && echo 'ASan+UBSan enabled' || echo 'disabled')"

cflags=(-std=c11 -O2 -g -Wall -Wextra -Werror -fno-omit-frame-pointer)
if [ "$use_sanitize" -eq 1 ]; then
    cflags+=(-fsanitize=address,undefined)
fi

mkdir -p "$BUILD_DIR"
c_bin="$BUILD_DIR/test_wifi_cred_logic"
rm -f "$c_bin"

if "$CC_BIN" "${cflags[@]}" -I"$ROOT/project_components/wifi_manager" \
        "$HOST_DIR/test_wifi_cred_logic.c" \
        "$ROOT/project_components/wifi_manager/wifi_cred_logic.c" \
        -o "$c_bin" >"$LOG_DIR/c-build.log" 2>&1; then
    if ASAN_OPTIONS="detect_leaks=0" "$c_bin" >"$LOG_DIR/c-run.log" 2>&1; then
        c_status=0
    else
        c_status=$?
    fi
    c_checks="$(grep -c '^ok\|^not ok' "$LOG_DIR/c-run.log" 2>/dev/null || true)"
    c_pass="$(grep -c '^ok ' "$LOG_DIR/c-run.log" 2>/dev/null || true)"
    c_xfail="$(grep -c '^not ok .*# TODO' "$LOG_DIR/c-run.log" 2>/dev/null || true)"
    c_checks="${c_checks:-0}"; c_pass="${c_pass:-0}"; c_xfail="${c_xfail:-0}"
    c_unexpected="$(grep '^not ok ' "$LOG_DIR/c-run.log" 2>/dev/null | grep -vc '# TODO' || true)"
    c_unexpected="${c_unexpected:-0}"
    diag "C wifi_cred_logic: checks=$c_checks pass=$c_pass expected-fail=$c_xfail unexpected-fail=$c_unexpected"
    if [ "$c_status" -eq 0 ]; then
        tap_ok "C wifi_cred_logic (blob/validation/eviction/rotation/AP SSID)"
    else
        tap_fail "C wifi_cred_logic (blob/validation/eviction/rotation/AP SSID)"
        sed 's/^/#   /' "$LOG_DIR/c-run.log" | tail -60
    fi
else
    tap_fail "C wifi_cred_logic build"
    sed 's/^/#   /' "$LOG_DIR/c-build.log" | tail -40
fi

# ---------------------------------------------------------------------------
# 2. DAP command-layer ASan harness (N1 / N6 / N9)
#    Source: dap-dev ASan harness, integrated under tests/host/dap/ (see its
#    README for provenance). It compiles the repository DAP.c against a stub
#    DAP_config.h, so it is a live regression for the security-review fixes.
# ---------------------------------------------------------------------------
dap_runner="$HOST_DIR/dap/run.sh"
dap_log="$LOG_DIR/dap.log"
if [ -x "$dap_runner" ]; then
    if SANITIZE="$use_sanitize" BUILD_DIR="$BUILD_DIR/dap" \
            ASAN_OPTIONS="detect_leaks=0" "$dap_runner" >"$dap_log" 2>&1; then
        dap_status=0
    else
        dap_status=$?
    fi
    dap_summary="$(grep -m1 -oE '[0-9]+ checks passed, [0-9]+ failed' "$dap_log" || true)"
    diag "DAP harness (N1/N6/N9): ${dap_summary:-no summary line} (ASan/UBSan=$use_sanitize)"
    if [ "$dap_status" -eq 0 ]; then
        tap_ok "DAP command layer N1/N6/N9 (ASan: 0x7E recursion + TransferBlock bounds)"
    else
        tap_fail "DAP command layer N1/N6/N9 (ASan: 0x7E recursion + TransferBlock bounds)"
        sed 's/^/#   /' "$dap_log" | tail -80
    fi
else
    tap_fail "DAP host harness runner missing (tests/host/dap/run.sh)"
fi

# ---------------------------------------------------------------------------
# 3. Probe identity serial formatting (pure C, ESP-IDF-free)
#    components/probe_identity/birdiebot_probe_identity_format.c is the single
#    source for the runtime serial "WiFi-Dap-<12 uppercase hex MAC>".
# ---------------------------------------------------------------------------
identity_bin="$BUILD_DIR/test_probe_identity_format"
rm -f "$identity_bin"
if "$CC_BIN" "${cflags[@]}" -I"$ROOT/components/probe_identity/include" \
        "$HOST_DIR/test_probe_identity_format.c" \
        "$ROOT/components/probe_identity/birdiebot_probe_identity_format.c" \
        -o "$identity_bin" >"$LOG_DIR/identity-build.log" 2>&1; then
    if ASAN_OPTIONS="detect_leaks=0" "$identity_bin" >"$LOG_DIR/identity-run.log" 2>&1; then
        identity_status=0
    else
        identity_status=$?
    fi
    identity_summary="$(grep -m1 -oE 'checks=[0-9]+ failed=[0-9]+' "$LOG_DIR/identity-run.log" || true)"
    diag "probe identity: ${identity_summary:-no summary line}"
    if [ "$identity_status" -eq 0 ]; then
        tap_ok "probe identity serial formatting (WiFi-Dap-<12HEX>)"
    else
        tap_fail "probe identity serial formatting (WiFi-Dap-<12HEX>)"
        sed 's/^/#   /' "$LOG_DIR/identity-run.log" | tail -40
    fi
else
    tap_fail "probe identity serial formatting build"
    sed 's/^/#   /' "$LOG_DIR/identity-build.log" | tail -40
fi

# ---------------------------------------------------------------------------
# 4. CMSIS-DAP over TCP frame codec (M3 skeleton, pure C, host-testable)
#    components/dap_tcp: 8-byte "DAP\0" frame header used by OpenOCD's
#    `cmsis-dap backend tcp` and the XRDAP probes (see docs/dap-tcp-backend.md).
# ---------------------------------------------------------------------------
dap_tcp_runner="$ROOT/components/dap_tcp/test/host/run.sh"
dap_tcp_log="$LOG_DIR/dap_tcp.log"
if [ -x "$dap_tcp_runner" ]; then
    if SANITIZE="$use_sanitize" BUILD_DIR="$BUILD_DIR/dap_tcp" \
            ASAN_OPTIONS="detect_leaks=0" "$dap_tcp_runner" >"$dap_tcp_log" 2>&1; then
        dap_tcp_status=0
    else
        dap_tcp_status=$?
    fi
    dap_tcp_summary="$(grep -m1 -oE '[0-9]+ checks?, [0-9]+ failure' "$dap_tcp_log" || true)"
    diag "DAP TCP frame codec: ${dap_tcp_summary:-no summary line}"
    if [ "$dap_tcp_status" -eq 0 ]; then
        tap_ok "CMSIS-DAP over TCP frame codec (M3 skeleton, host)"
    else
        tap_fail "CMSIS-DAP over TCP frame codec (M3 skeleton, host)"
        sed 's/^/#   /' "$dap_tcp_log" | tail -60
    fi
else
    tap_fail "DAP TCP frame codec runner missing (components/dap_tcp/test/host/run.sh)"
fi

# ---------------------------------------------------------------------------
# 5. SWD ultra-fast engine plan (M4, pure C, host-testable)
#    components/DAP/test/host: level table (CPU 160/240 MHz), never-exceed-host
#    level selection, request/ACK/parity models and the per-half-cycle wire
#    budget for the ESP32-S3 dedicated-GPIO engine (docs/swd-23mhz-engine.md).
# ---------------------------------------------------------------------------
swd_fast_runner="$ROOT/components/DAP/test/host/run.sh"
swd_fast_log="$LOG_DIR/swd_fast.log"
if [ -x "$swd_fast_runner" ]; then
    if SANITIZE="$use_sanitize" BUILD_DIR="$BUILD_DIR/swd_fast" \
            ASAN_OPTIONS="detect_leaks=0" "$swd_fast_runner" >"$swd_fast_log" 2>&1; then
        swd_fast_status=0
    else
        swd_fast_status=$?
    fi
    swd_fast_summary="$(grep -m1 -oE '[0-9]+ checks passed, [0-9]+ failed' "$swd_fast_log" || true)"
    diag "SWD ultra-fast plan: ${swd_fast_summary:-no summary line}"
    if [ "$swd_fast_status" -eq 0 ]; then
        tap_ok "SWD ultra-fast engine plan (M4: 20/24 MHz levels, host)"
    else
        tap_fail "SWD ultra-fast engine plan (M4: 20/24 MHz levels, host)"
        sed 's/^/#   /' "$swd_fast_log" | tail -40
    fi
else
    tap_fail "SWD ultra-fast plan runner missing (components/DAP/test/host/run.sh)"
fi

# ---------------------------------------------------------------------------
# 6. Python unittest suites (contract tests, stdlib only)
# ---------------------------------------------------------------------------
PYTHONDONTWRITEBYTECODE=1 "$PYTHON_BIN" -m unittest discover \
    -s "$HOST_DIR" -p 'test_*.py' -v >"$LOG_DIR/py.log" 2>&1
py_status=$?
py_ran="$(grep -m1 -oE 'Ran [0-9]+ tests?' "$LOG_DIR/py.log" | grep -oE '[0-9]+' || echo 0)"
py_xfail="$(grep -m1 -oE 'expected failures=[0-9]+' "$LOG_DIR/py.log" | grep -oE '[0-9]+' || echo 0)"
diag "python contract tests: ran=${py_ran} expected-fail=${py_xfail} (protocol_frame/ota_sha/partitions/security/hil_gate)"
if [ "$py_status" -eq 0 ]; then
    tap_ok "python contract tests (JSON/WS frame, OTA upload, partitions, HIL gate)"
else
    tap_fail "python contract tests (JSON/WS frame, OTA upload, partitions, HIL gate)"
    sed 's/^/#   /' "$LOG_DIR/py.log" | tail -80
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '1..%d\n' "$suite_no"
if [ "$suite_fail" -eq 0 ]; then
    diag "summary: suites=$suite_no failed=0 status=PASS"
    exit 0
fi
diag "summary: suites=$suite_no failed=$suite_fail status=FAIL"
exit 1
