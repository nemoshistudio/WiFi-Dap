#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# device_smoke.sh -- one-shot READ-ONLY smoke test for a running WiFi Dap
#                   WiFi DAP device (Wave3 hardware bring-up).
#
# Runs, in order, the existing read-only device test tools:
#   1. scripts/dap_smoke.py      --host H --http-port P [--token T]
#        USBIP(3240) devlist + HTTP GET / + JSON /ws GET_FM_INFO handshake
#   2. scripts/e2e_ws_client.py  --host H --port P [--token T] ping
#        WebSocket transport ping + SYSTEM/GET_FM_INFO
#   3. scripts/e2e_ws_client.py  --host H --port P [--token T] ota-info
#        OTA(5)/GET_INFO: running slot / version / free / status
#
# It never flashes, resets or reboots the device and writes only to a temporary
# directory. Use scripts/ota_upload.py for the (write) OTA upload path; use
# scripts/e2e_ws_client.py directly for deeper per-scenario tests (wifi-scan,
# uart-echo, dataflow-list, generic json).
#
# Usage:
#   scripts/device_smoke.sh --host 192.168.1.1
#   scripts/device_smoke.sh --host 192.168.1.1 --token 123456
#   scripts/device_smoke.sh --host 192.168.1.1 --skip-dap --json
#   scripts/device_smoke.sh --host wifi-dap.local --port 80
#
# Exit codes:
#   0 = all enabled steps PASS
#   1 = at least one step failed, or the device is unreachable
#   2 = usage/argument error
#
# Prerequisites:
#   * device is up (WiFi + webserver started; serial logs show the startup
#     banner / WT_OTA init) and <host>:<port> is reachable from this machine;
#   * no active debugger session on the DAP proxy port 3240 (single-connection
#     and serial; the dap_smoke USBIP probe can disturb an active session);
#   * --token is required when the firmware was built with BIRDIEBOT_WEB_AUTH=y.
#
# Environment (test hook, not needed on a real device):
#   DEVICE_SMOKE_SCRIPTS_DIR  directory containing dap_smoke.py /
#                             e2e_ws_client.py (default: this script's dir).
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"
readonly HELPER_DIR="${DEVICE_SMOKE_SCRIPTS_DIR:-$SCRIPT_DIR}"
readonly OUTPUT_TAIL_LINES=8
readonly CONNECT_TIMEOUT_S=3

HOST=""
PORT=80
TOKEN=""
SKIP_DAP=0
SKIP_WS=0
JSON=0

usage() {
    cat <<'EOF'
Usage: device_smoke.sh --host <ip|hostname> [options]

Read-only device smoke test (USBIP/HTTP/WS handshake + WS ping + OTA GET_INFO).
Never flashes, resets or reboots.

  --host <ip>     device address (required; IP or mDNS hostname, no scheme)
  --port N        HTTP/WebSocket port (default 80; USBIP probe stays on 3240)
  --token T       pair code for BIRDIEBOT_WEB_AUTH=y builds
  --skip-dap      skip scripts/dap_smoke.py (USBIP/HTTP/WS handshake)
  --skip-ws       skip the two scripts/e2e_ws_client.py steps
  --json          machine-readable JSON summary on stdout (diagnostics on stderr)
  -h, --help      show this help

Exit codes: 0 all enabled steps passed; 1 step failed / device unreachable;
            2 usage error.

Examples:
  scripts/device_smoke.sh --host 192.168.1.1
  scripts/device_smoke.sh --host 192.168.1.1 --token 123456
  scripts/device_smoke.sh --host 192.168.1.1 --skip-dap --json
EOF
}

die_usage() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    printf "try '%s --help'\n" "$SCRIPT_NAME" >&2
    exit 2
}

log() {
    printf '[device_smoke] %s\n' "$*" >&2
}

now_ms() {
    date +%s%3N
}

masked() {
    printf '***'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --host)
            [ "$#" -ge 2 ] || die_usage "--host requires a value"
            HOST="$2"
            shift 2
            ;;
        --host=*)
            HOST="${1#*=}"
            shift
            ;;
        --port)
            [ "$#" -ge 2 ] || die_usage "--port requires a value"
            PORT="$2"
            shift 2
            ;;
        --port=*)
            PORT="${1#*=}"
            shift
            ;;
        --token)
            [ "$#" -ge 2 ] || die_usage "--token requires a value"
            TOKEN="$2"
            shift 2
            ;;
        --token=*)
            TOKEN="${1#*=}"
            shift
            ;;
        --skip-dap)
            SKIP_DAP=1
            shift
            ;;
        --skip-ws)
            SKIP_WS=1
            shift
            ;;
        --json)
            JSON=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die_usage "unknown argument '$1'"
            ;;
    esac
done

[ -n "$HOST" ] || die_usage "--host is required"
case "$HOST" in
    *://*|*/*)
        die_usage "--host must be a bare IP/hostname (got '$HOST'); use --port for the port"
        ;;
esac
[[ "$PORT" =~ ^[0-9]+$ ]] || die_usage "--port must be an integer (got '$PORT')"
[ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die_usage "--port out of range: $PORT"
if [ "$SKIP_DAP" -eq 1 ] && [ "$SKIP_WS" -eq 1 ]; then
    die_usage "--skip-dap and --skip-ws leave nothing to test"
fi
command -v python3 >/dev/null 2>&1 || {
    printf '%s: error: python3 is required for the read-only device tools\n' "$SCRIPT_NAME" >&2
    exit 1
}

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/device-smoke.XXXXXX")"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

: >"$TMP_DIR/steps.tsv"
STEP_INDEX=0
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
STARTED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
SMOKE_START_MS="$(now_ms)"

record_step() {
    # record_step <name> <status> <exit_code> <duration_ms> <display> <output_file>
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$1" "$2" "$3" "$4" "$5" "$6" >>"$TMP_DIR/steps.tsv"
    case "$2" in
        PASS) PASS_COUNT=$((PASS_COUNT + 1)) ;;
        FAIL) FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
        SKIP) SKIP_COUNT=$((SKIP_COUNT + 1)) ;;
    esac
}

run_step() {
    # run_step <name> <display> <cmd...>
    local name="$1"; shift
    local display="$1"; shift
    local out_file started ended rc duration
    STEP_INDEX=$((STEP_INDEX + 1))
    out_file="$TMP_DIR/step-${STEP_INDEX}.log"
    started="$(now_ms)"
    if "$@" >"$out_file" 2>&1; then
        rc=0
    else
        rc=$?
    fi
    ended="$(now_ms)"
    duration=$((ended - started))

    if [ "$rc" -eq 0 ]; then
        log "[$STEP_INDEX] $name ... PASS (${duration} ms)"
        record_step "$name" PASS "$rc" "$duration" "$display" "$out_file"
    else
        log "[$STEP_INDEX] $name ... FAIL (exit $rc, ${duration} ms)"
        log "  command: $display"
        log "  output (last $OUTPUT_TAIL_LINES lines):"
        tail -n "$OUTPUT_TAIL_LINES" "$out_file" 2>/dev/null | sed 's/^/    /' >&2 || true
        record_step "$name" FAIL "$rc" "$duration" "$display" "$out_file"
    fi
}

run_missing_helper_step() {
    # run_missing_helper_step <name> <display> <helper path>
    local name="$1" display="$2" helper="$3"
    local out_file
    STEP_INDEX=$((STEP_INDEX + 1))
    out_file="$TMP_DIR/step-${STEP_INDEX}.log"
    printf 'helper script not found: %s\n(a merged main branch provides it; see scripts/README.md)\n' \
        "$helper" >"$out_file"
    log "[$STEP_INDEX] $name ... FAIL (missing helper $helper)"
    log "  $helper is missing; merge the main branch or set DEVICE_SMOKE_SCRIPTS_DIR"
    record_step "$name" FAIL 127 0 "$display" "$out_file"
}

skip_step() {
    # skip_step <name> <reason>
    local name="$1" reason="$2"
    local out_file
    STEP_INDEX=$((STEP_INDEX + 1))
    out_file="$TMP_DIR/step-${STEP_INDEX}.log"
    printf 'SKIPPED: %s\n' "$reason" >"$out_file"
    log "[$STEP_INDEX] $name ... SKIP ($reason)"
    record_step "$name" SKIP 0 0 "(not run: $reason)" "$out_file"
}

# --- which steps are enabled? ---------------------------------------------
STEP_NAMES=()
if [ "$SKIP_DAP" -eq 0 ]; then
    STEP_NAMES+=("dap-smoke")
fi
if [ "$SKIP_WS" -eq 0 ]; then
    STEP_NAMES+=("ws-ping" "ws-ota-info")
fi

# --- pre-flight: is the device TCP-reachable at all? ----------------------
DEVICE_REACHABLE=0
UNREACHABLE_REASON=""
if python3 - "$HOST" "$PORT" "$CONNECT_TIMEOUT_S" <<'PY'
import socket, sys
host, port, timeout = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
try:
    with socket.create_connection((host, port), timeout=timeout):
        pass
except OSError as exc:
    print(f"{exc}", file=sys.stderr)
    sys.exit(1)
PY
then
    DEVICE_REACHABLE=1
else
    UNREACHABLE_REASON="TCP ${HOST}:${PORT} not reachable"
    log "device not reachable: $UNREACHABLE_REASON"
    log "设备未连接/未就绪，请检查："
    log "  - 设备已上电且启动完成（串口可见 BIRDIEBOT/WT_OTA 日志、webserver 已启动）"
    log "  - PC/手机已连上设备 AP（默认 WiFi-Dap-XXXX）或与设备在同一局域网"
    log "  - --host/--port 正确（AP 模式默认 IP 192.168.1.1、端口 80）"
    log "  - 本机防火墙/路由允许访问，先试: ping $HOST 与 curl -sS -o /dev/null -w '%{http_code}' http://$HOST:$PORT/"
    log "  - 开启 BIRDIEBOT_WEB_AUTH 时请提供 --token"
fi

DAP_HELPER="$HELPER_DIR/dap_smoke.py"
E2E_HELPER="$HELPER_DIR/e2e_ws_client.py"

# --- run steps ------------------------------------------------------------
if [ "$DEVICE_REACHABLE" -eq 0 ]; then
    for name in "${STEP_NAMES[@]}"; do
        skip_step "$name" "$UNREACHABLE_REASON"
    done
else
    if [ "$SKIP_DAP" -eq 0 ]; then
        display="python3 scripts/dap_smoke.py --host $HOST --http-port $PORT"
        [ -n "$TOKEN" ] && display+=" --token $(masked)"
        if [ -f "$DAP_HELPER" ]; then
            dap_cmd=(python3 "$DAP_HELPER" --host "$HOST" --http-port "$PORT")
            [ -n "$TOKEN" ] && dap_cmd+=(--token "$TOKEN")
            run_step "dap-smoke" "$display" "${dap_cmd[@]}"
        else
            run_missing_helper_step "dap-smoke" "$display" "$DAP_HELPER"
        fi
    fi
    if [ "$SKIP_WS" -eq 0 ]; then
        for scenario in ping ota-info; do
            display="python3 scripts/e2e_ws_client.py --host $HOST --port $PORT"
            [ -n "$TOKEN" ] && display+=" --token $(masked)"
            display+=" $scenario"
            if [ -f "$E2E_HELPER" ]; then
                e2e_cmd=(python3 "$E2E_HELPER" --host "$HOST" --port "$PORT")
                [ -n "$TOKEN" ] && e2e_cmd+=(--token "$TOKEN")
                e2e_cmd+=("$scenario")
                run_step "ws-$scenario" "$display" "${e2e_cmd[@]}"
            else
                run_missing_helper_step "ws-$scenario" "$display" "$E2E_HELPER"
            fi
        done
    fi
fi

SMOKE_END_MS="$(now_ms)"
TOTAL_MS=$((SMOKE_END_MS - SMOKE_START_MS))

if [ "$FAIL_COUNT" -gt 0 ] || [ "$SKIP_COUNT" -gt 0 ]; then
    RESULT="FAIL"
    EXIT_CODE=1
else
    RESULT="PASS"
    EXIT_CODE=0
fi

# --- summary --------------------------------------------------------------
if [ "$JSON" -eq 1 ]; then
    python3 - "$HOST" "$PORT" "$TOKEN" "$DEVICE_REACHABLE" "$UNREACHABLE_REASON" \
        "$STARTED_AT" "$TOTAL_MS" "$RESULT" "$TMP_DIR/steps.tsv" "$OUTPUT_TAIL_LINES" <<'PY'
import json
import sys

(host, port, token, reachable, unreachable,
 started_at, total_ms, result, steps_file, tail_n) = sys.argv[1:]
steps = []
with open(steps_file, encoding="utf-8") as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        name, status, rc, duration, display, out_file = line.split("\t")
        tail = ""
        if out_file:
            try:
                with open(out_file, encoding="utf-8", errors="replace") as oh:
                    lines = [ln.rstrip("\n") for ln in oh][-int(tail_n):]
                tail = "\n".join(lines)[-4000:]
            except OSError:
                tail = ""
        steps.append({
            "name": name,
            "command": display,
            "status": status,
            "exit_code": int(rc),
            "duration_ms": int(duration),
            "output_tail": tail,
        })
summary = {
    "host": host,
    "port": int(port),
    "token_used": bool(token),
    "device_reachable": reachable == "1",
    "started_at": started_at,
    "duration_ms": int(total_ms),
    "steps": steps,
    "summary": {
        "total": len(steps),
        "passed": sum(1 for s in steps if s["status"] == "PASS"),
        "failed": sum(1 for s in steps if s["status"] == "FAIL"),
        "skipped": sum(1 for s in steps if s["status"] == "SKIP"),
    },
    "result": result,
}
if not summary["device_reachable"]:
    summary["hint"] = (
        "device unreachable: power on the device, join its AP (WiFi-Dap-XXXX) "
        "or the same LAN, verify --host/--port and firewall; check serial logs that "
        "the webserver started"
    )
json.dump(summary, sys.stdout, indent=2, ensure_ascii=False)
sys.stdout.write("\n")
PY
    log "JSON summary written to stdout (result: $RESULT)"
else
    echo
    echo "== device_smoke: $HOST:$PORT (read-only) =="
    printf '%-14s %-6s %-8s %s\n' "STEP" "STATUS" "TIME(ms)" "COMMAND"
    while IFS=$'\t' read -r name status rc duration display out_file; do
        [ -n "$name" ] || continue
        printf '%-14s %-6s %-8s %s\n' "$name" "$status" "$duration" "$display"
    done <"$TMP_DIR/steps.tsv"
    echo "------------------------------------------------------------"
    printf 'summary: %d passed, %d failed, %d skipped, total %d ms\n' \
        "$PASS_COUNT" "$FAIL_COUNT" "$SKIP_COUNT" "$TOTAL_MS"
    if [ "$DEVICE_REACHABLE" -eq 0 ]; then
        printf 'device : UNREACHABLE (%s) - see the hints above\n' "$UNREACHABLE_REASON"
    else
        printf 'device : reachable at %s:%s\n' "$HOST" "$PORT"
    fi
    printf 'RESULT : %s\n' "$RESULT"
fi

exit "$EXIT_CODE"
