#!/usr/bin/env bash
# C1 USB-NCM one-shot acceptance runner for the WiFi Dap.
#
# Validates the C1 (USB-NCM wired) path end-to-end:
#   1. wait for the host cdc_ncm interface (enx*),
#   2. configure the host side (192.168.7.2/24, NetworkManager-unmanaged)
#      through ROOT_HELPER,
#   3. USBIP attach the DAP over USB-NCM (192.168.7.1),
#   4. session 1: OpenOCD flash write + verify of the H7 image,
#   5. detach + re-attach (the C1 second-session regression test),
#   6. session 2: read back 1 MiB and compare the image-prefix sha256,
#   7. print PASS/FAIL and the per-step timings.
#
# Root is needed for `ip`/`nmcli`/`usbip`/OpenOCD.  ROOT_HELPER is a command
# prefix used for all root operations (default: `sudo`).  When sudo has no
# password use the docker-nsenter helper that exists on this host:
#
#   ROOT_HELPER=/tmp/hostroot.sh scripts/c1_accept.sh --image <firmware.bin>
#
# Usage
# -----
#   scripts/c1_accept.sh --image <firmware.bin> [options]
#   scripts/c1_accept.sh --dry-run [--image <firmware.bin>]
#
# Options:
#   --image PATH            H7 firmware image (default: reference firmware reference)
#   --expected-sha256 HEX   expected image sha256 (default: sha256 of --image)
#   --openocd PATH          OpenOCD binary
#                           (default: build/openocd-master-install/bin/openocd)
#   --scripts PATH          OPENOCD_SCRIPTS dir
#                           (default: build/openocd-master-install/share/openocd/scripts)
#   --candidate             use the pipelining speed build
#                           (build/openocd-master-speed/install)
#   --adapter-speed KHZ     SWD clock (default: 1900)
#   --base-addr ADDR        flash base (default: 0x08000000)
#   --read-size BYTES       session-2 read size (default: 0x100000 = 1 MiB)
#   --read-method METHOD    flash|dump|auto (default: auto = flash read_bank,
#                           fall back to dump_image)
#   --device-ip IP          NCM device IP (default: 192.168.7.1)
#   --host-ip IP            host NCM IP (default: 192.168.7.2)
#   --busid BUSID           USBIP bus id (default: 1-1)
#   --iface IFACE           host NCM interface (default: auto-detect enx*)
#   --vid-pid VID PID       expected CMSIS-DAP USB IDs (default: 0xc251 0xf00a)
#   --root-helper CMD       root prefix (default: $ROOT_HELPER or sudo)
#   --usbip-attach-cmd CMD  override the attach command (run via ROOT_HELPER)
#   --usbip-detach-cmd CMD  override the detach command (run via ROOT_HELPER)
#   --no-detach-first       do not detach an already-imported DAP before attach
#   --detach-at-end         detach the DAP when the acceptance finishes
#   --iface-timeout SEC     seconds to wait for enx* (default: 30)
#   --usbip-timeout SEC     seconds to wait for the USBIP port (default: 30)
#   --openocd-timeout SEC   per-OpenOCD hard timeout (default: 180)
#   --log-dir DIR           artifact dir (default: /tmp/c1-accept-<timestamp>)
#   --label LABEL           label printed in the summary
#   --dry-run               print the plan + run offline-only checks, never
#                           touches root/hardware
#   --help                  show this help
#
# The script NEVER commits or modifies the repo; all artifacts go to --log-dir
# (default /tmp).  Exit codes: 0 PASS, 1 FAIL, 2 usage/config error.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"

IMAGE="$HOME/reference firmware/firmware/reference-board/reference/firmware.bin"
EXPECTED_SHA256=""
OPENOCD="$REPO_ROOT/build/openocd-master-install/bin/openocd"
SCRIPTS="$REPO_ROOT/build/openocd-master-install/share/openocd/scripts"
CANDIDATE_OPENOCD="$REPO_ROOT/build/openocd-master-speed/install/bin/openocd"
CANDIDATE_SCRIPTS="$REPO_ROOT/build/openocd-master-speed/install/share/openocd/scripts"
ADAPTER_SPEED=1900
BASE_ADDR=0x08000000
READ_SIZE=0x100000
READ_METHOD=auto
DEVICE_IP=192.168.7.1
HOST_IP=192.168.7.2
BUSID=1-1
IFACE=""
VID_PID="0xc251 0xf00a"
ROOT_HELPER="${ROOT_HELPER:-sudo}"
USBPIP_ATTACH_CMD=""
USBPIP_DETACH_CMD=""
DETACH_FIRST=1
DETACH_AT_END=0
IFACE_TIMEOUT=30
USBPIP_TIMEOUT=30
OPENOCD_TIMEOUT=180
LOG_DIR=""
LABEL="c1-accept"
DRY_RUN=0
INTERFACE_CFG="interface/cmsis-dap.cfg"
TARGET_CFG="target/stm32h7x.cfg"

STEP_NAMES=()
STEP_RESULTS=()
STEP_TIMES=()
ERRORS=0
TOTAL_START_NS=0
IFACE_FOUND=""

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
# Collapse duplicated spaces created by line-continued message arguments.
_clean_msg() {
  local s="$1"
  while [[ "$s" == *"  "* ]]; do s="${s//  / }"; done
  printf '%s' "$s"
}
log() { printf '[c1-accept] %s\n' "$(_clean_msg "$*")"; }
warn() { printf '[c1-accept] WARNING: %s\n' "$(_clean_msg "$*")" >&2; }
die() { printf '[c1-accept] ERROR: %s\n' "$(_clean_msg "$*")" >&2; exit 2; }

now_ns() { date +%s%N; }

fmt_ms() { # ns difference -> "X.xxx s"
  local delta_ns="$1"
  printf '%d.%03d s' "$((delta_ns / 1000000000))" \
    "$(((delta_ns / 1000000) % 1000))"
}

record_step() { # name result ns
  STEP_NAMES+=("$1")
  STEP_RESULTS+=("$2")
  STEP_TIMES+=("$(fmt_ms "$3")")
  if [ "$2" = "FAIL" ]; then
    ERRORS=$((ERRORS + 1))
  fi
}

run_root() { "$ROOT_HELPER" "$@"; }

usage() {
  sed -n '2,80p' "${BASH_SOURCE[0]}" | sed -e 's/^# \{0,1\}//'
  exit "${1:-2}"
}

# Parse `usbip port` output on stdin; print port numbers of imported CMSIS-DAP /
# WiFi Dap devices (one per line).
parse_dap_ports() {
  local line port="" matched=0
  local found=()
  while IFS= read -r line; do
    if [[ "$line" =~ ^Port\ ([0-9]+): ]]; then
      if [ -n "$port" ] && [ "$matched" -eq 1 ]; then
        found+=("$port")
      fi
      port="${BASH_REMATCH[1]}"
      matched=0
    elif [ -n "$port" ] && [[ "${line,,}" =~ (cmsis|birdiebot|c251:f00a|dap) ]]; then
      matched=1
    fi
  done
  if [ -n "$port" ] && [ "$matched" -eq 1 ]; then
    found+=("$port")
  fi
  if [ "${#found[@]}" -gt 0 ]; then
    printf '%s\n' "${found[@]}"
  fi
  return 0
}

attached_dap_ports() {
  run_root usbip port 2>/dev/null | parse_dap_ports || true
}

# Quote one path as a single TCL word; reject paths containing braces.
tcl_quote() {
  local value="$1"
  if [[ "$value" == *"{"* || "$value" == *"}"* ]]; then
    die "path is not TCL-safe (contains braces): $value"
  fi
  printf '{%s}' "$value"
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

# Check a numeric argument as hex or decimal (also accepts 0x... strings).
is_number() {
  [[ "$1" =~ ^(0x[0-9a-fA-F]+|[0-9]+)$ ]]
}

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --image) IMAGE="${2:?--image needs a value}"; shift 2 ;;
      --expected-sha256) EXPECTED_SHA256="${2:?--expected-sha256 needs a value}"; shift 2 ;;
      --openocd) OPENOCD="${2:?--openocd needs a value}"; shift 2 ;;
      --scripts) SCRIPTS="${2:?--scripts needs a value}"; shift 2 ;;
      --candidate) OPENOCD="$CANDIDATE_OPENOCD"; SCRIPTS="$CANDIDATE_SCRIPTS"; shift ;;
      --adapter-speed) ADAPTER_SPEED="${2:?--adapter-speed needs a value}"; shift 2 ;;
      --base-addr) BASE_ADDR="${2:?--base-addr needs a value}"; shift 2 ;;
      --read-size) READ_SIZE="${2:?--read-size needs a value}"; shift 2 ;;
      --read-method) READ_METHOD="${2:?--read-method needs a value}"; shift 2 ;;
      --device-ip) DEVICE_IP="${2:?--device-ip needs a value}"; shift 2 ;;
      --host-ip) HOST_IP="${2:?--host-ip needs a value}"; shift 2 ;;
      --busid) BUSID="${2:?--busid needs a value}"; shift 2 ;;
      --iface) IFACE="${2:?--iface needs a value}"; shift 2 ;;
      --vid-pid) VID_PID="${2:?--vid-pid needs a value}"; shift 2 ;;
      --root-helper) ROOT_HELPER="${2:?--root-helper needs a value}"; shift 2 ;;
      --usbip-attach-cmd) USBPIP_ATTACH_CMD="${2:?--usbip-attach-cmd needs a value}"; shift 2 ;;
      --usbip-detach-cmd) USBPIP_DETACH_CMD="${2:?--usbip-detach-cmd needs a value}"; shift 2 ;;
      --no-detach-first) DETACH_FIRST=0; shift ;;
      --detach-at-end) DETACH_AT_END=1; shift ;;
      --iface-timeout) IFACE_TIMEOUT="${2:?--iface-timeout needs a value}"; shift 2 ;;
      --usbip-timeout) USBPIP_TIMEOUT="${2:?--usbip-timeout needs a value}"; shift 2 ;;
      --openocd-timeout) OPENOCD_TIMEOUT="${2:?--openocd-timeout needs a value}"; shift 2 ;;
      --log-dir) LOG_DIR="${2:?--log-dir needs a value}"; shift 2 ;;
      --label) LABEL="${2:?--label needs a value}"; shift 2 ;;
      --dry-run) DRY_RUN=1; shift ;;
      --help|-h) usage 0 ;;
      *) die "unknown argument: $1 (try --help)" ;;
    esac
  done

  for value in "$ADAPTER_SPEED" "$IFACE_TIMEOUT" "$USBPIP_TIMEOUT" \
               "$OPENOCD_TIMEOUT"; do
    is_number "$value" || die "not a number: $value"
  done
  is_number "$READ_SIZE" || die "--read-size is not a number: $READ_SIZE"
  is_number "$BASE_ADDR" || die "--base-addr is not a number: $BASE_ADDR"
  case "$READ_METHOD" in
    auto|flash|dump) ;;
    *) die "--read-method must be auto, flash or dump" ;;
  esac
  command -v timeout >/dev/null 2>&1 || die "missing 'timeout' (coreutils)"
  # --expected-sha256 default: sha256 of the image (computed in preflight).
}

# ---------------------------------------------------------------------------
# dry-run checks (offline only; no root, no hardware, no usbip)
# ---------------------------------------------------------------------------
dry_run() {
  local checks=0 failures=0
  local start_ns end_ns

  log "DRY RUN: no root command is executed, no hardware is touched"
  [ -n "$LOG_DIR" ] || LOG_DIR="/tmp/c1-accept-dryrun-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$LOG_DIR"
  log "label=$LABEL log_dir=$LOG_DIR"
  log "image=$IMAGE"
  log "openocd=$OPENOCD"
  log "scripts=$SCRIPTS"
  log "read_method=$READ_METHOD read_size=$READ_SIZE base=$BASE_ADDR"
  log "device=${DEVICE_IP} host_ip=${HOST_IP} busid=${BUSID} root=$ROOT_HELPER"

  # --- check 1: image exists + expected sha256
  if [ -f "$IMAGE" ]; then
    local image_sha image_size
    image_sha="$(sha256_file "$IMAGE")"
    image_size="$(stat -c %s "$IMAGE")"
    checks=$((checks + 1))
    if [ -z "$EXPECTED_SHA256" ]; then
      EXPECTED_SHA256="$image_sha"
    fi
    if [ "${image_sha,,}" = "${EXPECTED_SHA256,,}" ]; then
      log "check sha256: OK ($image_sha, $image_size B)"
    else
      failures=$((failures + 1))
      warn "check sha256: image sha256 $image_sha != expected $EXPECTED_SHA256"
    fi
  else
    failures=$((failures + 1))
    warn "check image: not found: $IMAGE"
  fi

  # --- check 2: OpenOCD + scripts present
  checks=$((checks + 1))
  if [ -x "$OPENOCD" ]; then
    log "check openocd: OK ($("$OPENOCD" --version 2>&1 | head -n1))"
  else
    failures=$((failures + 1))
    warn "check openocd: not executable: $OPENOCD"
  fi
  checks=$((checks + 1))
  if [ -d "$SCRIPTS" ]; then
    log "check scripts: OK ($SCRIPTS)"
  else
    failures=$((failures + 1))
    warn "check scripts: not found: $SCRIPTS"
  fi

  # --- check 3: root helper
  checks=$((checks + 1))
  if [ "$ROOT_HELPER" = "sudo" ]; then
    if command -v sudo >/dev/null 2>&1; then
      log "check root helper: sudo present (no-password hosts may need " \
          "ROOT_HELPER=/tmp/hostroot.sh)"
    else
      failures=$((failures + 1))
      warn "check root helper: sudo not found"
    fi
  elif [ -x "$ROOT_HELPER" ] || command -v "$ROOT_HELPER" >/dev/null 2>&1; then
    log "check root helper: OK ($ROOT_HELPER)"
  else
    failures=$((failures + 1))
    warn "check root helper: not found/executable: $ROOT_HELPER"
  fi

  # --- check 4: host tools
  for tool in usbip nmcli ip ping; do
    checks=$((checks + 1))
    if command -v "$tool" >/dev/null 2>&1; then
      log "check tool $tool: OK"
    else
      warn "check tool $tool: MISSING (required on the host)"
    fi
  done

  # --- check 5: NCM interface detection (read-only sysfs/ip)
  checks=$((checks + 1))
  IFACE_FOUND="$(detect_iface_once || true)"
  if [ -n "$IFACE_FOUND" ]; then
    log "check ncm iface: found $IFACE_FOUND"
  else
    warn "check ncm iface: no enx* present now (plug the C1 USB-NCM device; " \
         "the real run waits up to ${IFACE_TIMEOUT}s)"
  fi

  # --- self-test: port parser + tcl quoting + read-method strings
  local fixture ports
  fixture=$'Imported USB devices\n====================\nPort 00: <Port in Use> at High Speed(480Mbps)\n       WiFi Dap (CMSIS-DAP)\n       1-1 -> usbip://192.168.7.1:3240/1-1\nPort 01: <Port in Use> at High Speed(480Mbps)\n       Generic USB Hub\n       2-1 -> usbip://example:3240/1-2\n'
  ports="$(printf '%s' "$fixture" | parse_dap_ports)"
  checks=$((checks + 1))
  if [ "$ports" = "00" ]; then
    log "self-test parse_dap_ports: OK (selected port 00, skipped 01)"
  else
    failures=$((failures + 1))
    warn "self-test parse_dap_ports: FAIL (got '$ports', want '00')"
  fi
  checks=$((checks + 1))
  if [ -z "$(printf 'Imported USB devices\n' | parse_dap_ports)" ]; then
    log "self-test parse_dap_ports empty: OK"
  else
    failures=$((failures + 1))
    warn "self-test parse_dap_ports empty: FAIL"
  fi
  checks=$((checks + 1))
  if [ "$(tcl_quote "/tmp/a b/image.bin")" = "{/tmp/a b/image.bin}" ]; then
    log "self-test tcl_quote: OK"
  else
    failures=$((failures + 1))
    warn "self-test tcl_quote: FAIL"
  fi
  local empty_sha
  empty_sha="$(printf '' | sha256sum | awk '{print $1}')"
  checks=$((checks + 1))
  if [ "${empty_sha,,}" = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ]; then
    log "self-test sha256 tool path: OK"
  else
    failures=$((failures + 1))
    warn "self-test sha256 tool path: FAIL"
  fi

  # --- planned command plan (TCL files are generated for inspection)
  write_tcl_files
  echo
  log "planned steps (in order):"
  cat <<EOF
  1. wait up to ${IFACE_TIMEOUT}s for an enx* cdc_ncm interface
  2. \$ROOT_HELPER nmcli device set <iface> managed no
     \$ROOT_HELPER ip addr replace ${HOST_IP}/24 dev <iface>
     \$ROOT_HELPER ip link set <iface> up
     ping -c1 -W1 ${DEVICE_IP}
  3. detach an already-imported DAP (if any), then
     \$ROOT_HELPER usbip attach -r ${DEVICE_IP} -b ${BUSID}
     (override with --usbip-attach-cmd)
  4. session 1 (one OpenOCD process): write + verify
     \$ROOT_HELPER timeout -k 10 ${OPENOCD_TIMEOUT} env OPENOCD_SCRIPTS=$SCRIPTS $OPENOCD \\
       -f ${INTERFACE_CFG} -c "cmsis_dap_vid_pid ${VID_PID}" -c "transport select swd" \\
       -f ${TARGET_CFG} -c "adapter speed ${ADAPTER_SPEED}" -c "reset_config none" \\
       -c "cortex_m reset_config sysresetreq" -c init -c "reset halt" -f $LOG_DIR/session1.tcl
  5. detach + re-attach: \$ROOT_HELPER usbip attach -r ${DEVICE_IP} -b ${BUSID}
  6. session 2 (fresh OpenOCD process): read ${READ_SIZE} bytes (${READ_METHOD})
     prefix sha256 of the readback is compared to ${EXPECTED_SHA256:-<image sha>}
EOF
  echo
  cat "$LOG_DIR/session1.tcl"
  echo "---"
  cat "$LOG_DIR/session2-flash.tcl"
  [ -f "$LOG_DIR/session2-dump.tcl" ] && { echo "---"; cat "$LOG_DIR/session2-dump.tcl"; }
  echo
  if [ "$failures" -eq 0 ]; then
    log "DRY-RUN PASS ($checks checks, no hardware touched)"
    return 0
  fi
  warn "DRY-RUN FAIL ($failures of $checks checks failed)"
  return 1
}

# ---------------------------------------------------------------------------
# TCL session files
# ---------------------------------------------------------------------------
write_tcl_files() {
  mkdir -p "$LOG_DIR" || die "cannot create log dir $LOG_DIR"
  local image_q read_q
  image_q="$(tcl_quote "$IMAGE")"
  read_q="$(tcl_quote "$LOG_DIR/readback.bin")"

  cat > "$LOG_DIR/session1.tcl" <<EOF
flash write_image erase ${image_q} ${BASE_ADDR}
verify_image ${image_q} ${BASE_ADDR}
reset run
shutdown
EOF

  cat > "$LOG_DIR/session2-flash.tcl" <<EOF
flash read_bank 0 ${read_q} 0 ${READ_SIZE}
reset run
shutdown
EOF

  cat > "$LOG_DIR/session2-dump.tcl" <<EOF
dump_image ${read_q} ${BASE_ADDR} ${READ_SIZE}
reset run
shutdown
EOF
}

# ---------------------------------------------------------------------------
# host / USBIP plumbing
# ---------------------------------------------------------------------------
detect_iface_once() {
  ip -br link show 2>/dev/null | awk '$1 ~ /^enx/ {print $1; exit}'
}

wait_for_iface() {
  local tries=$((IFACE_TIMEOUT * 2)) i
  if [ -n "$IFACE" ]; then
    if ip link show "$IFACE" >/dev/null 2>&1; then
      IFACE_FOUND="$IFACE"
      return 0
    fi
    warn "--iface $IFACE not present, waiting for it up to ${IFACE_TIMEOUT}s"
  fi
  for i in $(seq 1 "$tries"); do
    if [ -n "$IFACE" ]; then
      if ip link show "$IFACE" >/dev/null 2>&1; then
        IFACE_FOUND="$IFACE"
        return 0
      fi
    else
      IFACE_FOUND="$(detect_iface_once || true)"
      if [ -n "$IFACE_FOUND" ]; then
        return 0
      fi
    fi
    sleep 0.5
  done
  return 1
}

host_configure() {
  local iface="$1"
  # NetworkManager must not manage the probe interface: it flushes the manual
  # IPv4 address after its DHCP attempt fails, which kills in-flight USBIP /
  # OpenOCD sessions (observed: hung flash + vhci_hcd xmit -110).
  run_root nmcli device set "$iface" managed no >/dev/null 2>&1 || true
  run_root ip addr replace "${HOST_IP}/24" dev "$iface" >/dev/null 2>&1 || true
  run_root ip link set "$iface" up >/dev/null 2>&1 || true
  local i
  for i in 1 2 3 4 5 6 7 8 9 10; do
    if ping -c1 -W1 "$DEVICE_IP" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

usbip_attach_once() {
  local out rc
  if [ -n "$USBPIP_ATTACH_CMD" ]; then
    out="$(run_root sh -c "$USBPIP_ATTACH_CMD" 2>&1)"
    rc=$?
  else
    out="$(run_root usbip attach -r "$DEVICE_IP" -b "$BUSID" 2>&1)"
    rc=$?
  fi
  [ -n "$out" ] && log "usbip attach: $out"
  if [ "$rc" -ne 0 ] || [[ "${out,,}" == *error* ]]; then
    return 1
  fi
  return 0
}

wait_for_dap_port() {
  local tries=$((USBPIP_TIMEOUT * 2)) i ports
  for i in $(seq 1 "$tries"); do
    ports="$(attached_dap_ports || true)"
    if [ -n "$ports" ]; then
      echo "$ports" | head -n1
      return 0
    fi
    sleep 0.5
  done
  return 1
}

detach_dap() {
  local ports port
  if [ -n "$USBPIP_DETACH_CMD" ]; then
    run_root sh -c "$USBPIP_DETACH_CMD" >/dev/null 2>&1 || true
    return 0
  fi
  ports="$(attached_dap_ports || true)"
  if [ -z "$ports" ]; then
    return 0
  fi
  while IFS= read -r port; do
    [ -n "$port" ] || continue
    log "usbip detach -p $port"
    run_root usbip detach -p "$port" >/dev/null 2>&1 || true
  done <<< "$ports"
  return 0
}

attach_and_wait() {
  local attempt port
  for attempt in 1 2 3 4 5; do
    if usbip_attach_once; then
      port="$(wait_for_dap_port || true)"
      if [ -n "$port" ]; then
        log "USBIP DAP attached on port $port"
        return 0
      fi
      warn "attach returned success but no CMSIS-DAP USBIP port appeared"
    else
      warn "usbip attach attempt $attempt failed"
    fi
    sleep 2
  done
  return 1
}

# ---------------------------------------------------------------------------
# OpenOCD sessions
# ---------------------------------------------------------------------------
run_openocd_session() { # label tcl logfile
  local label="$1" tcl="$2" log_file="$3" rc
  local -a cmd=(
    "$OPENOCD"
    -f "$INTERFACE_CFG"
    -c "cmsis_dap_vid_pid $VID_PID"
    -c "transport select swd"
    -f "$TARGET_CFG"
    -c "adapter speed $ADAPTER_SPEED"
    -c "reset_config none"
    -c "cortex_m reset_config sysresetreq"
    -c init
    -c "reset halt"
    -f "$tcl"
  )
  log "session '$label' -> $log_file"
  run_root timeout -k 10 "$OPENOCD_TIMEOUT" env "OPENOCD_SCRIPTS=$SCRIPTS" \
    "${cmd[@]}" 2>&1 | tee "$log_file"
  rc=${PIPESTATUS[0]}
  return "$rc"
}

parse_verified_s() { # log file -> target-side verify seconds or ""
  grep -oE 'verified [0-9]+ bytes in [0-9.]+s' "$1" 2>/dev/null \
    | tail -n1 | sed -E 's/.* in ([0-9.]+)s/\1/' || true
}

# ---------------------------------------------------------------------------
# acceptance flow
# ---------------------------------------------------------------------------
preflight_local() {
  [ -f "$IMAGE" ] || die "image not found: $IMAGE"
  local image_sha
  image_sha="$(sha256_file "$IMAGE")"
  if [ -z "$EXPECTED_SHA256" ]; then
    EXPECTED_SHA256="$image_sha"
  fi
  if [ "${image_sha,,}" != "${EXPECTED_SHA256,,}" ]; then
    die "image sha256 $image_sha != expected $EXPECTED_SHA256"
  fi
  IMAGE_SIZE="$(stat -c %s "$IMAGE")"
  [ "$IMAGE_SIZE" -gt 0 ] || die "image is empty: $IMAGE"
  [ -x "$OPENOCD" ] || die "OpenOCD binary not executable: $OPENOCD"
  [ -d "$SCRIPTS" ] || die "OpenOCD scripts dir not found: $SCRIPTS"
  [ -n "$LOG_DIR" ] || LOG_DIR="/tmp/c1-accept-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$LOG_DIR" || die "cannot create log dir $LOG_DIR"
  write_tcl_files
  log "image: $IMAGE ($IMAGE_SIZE B, sha256 ${image_sha:0:12}...)"
  log "openocd: $("$OPENOCD" --version 2>&1 | head -n1)"
  log "logs: $LOG_DIR"
}

check_root() {
  if ! run_root true >/dev/null 2>&1; then
    die "ROOT_HELPER ($ROOT_HELPER) cannot run commands as root; on this " \
        "host try ROOT_HELPER=/tmp/hostroot.sh"
  fi
}

acceptance() {
  local start_ns end_ns step_ns ports port
  local s1_rc s1_verify
  local s2_rc read_method_used="" read_ok=0
  local readback="$LOG_DIR/readback.bin"
  local expected_prefix actual_prefix read_bytes s2_throughput

  preflight_local
  check_root
  rm -f "$readback"

  # 1) interface + host configuration
  log "waiting for the USB-NCM interface (enx*) up to ${IFACE_TIMEOUT}s ..."
  start_ns=$(now_ns)
  if ! wait_for_iface; then
    record_step "ncm interface" FAIL "$(( $(now_ns) - start_ns ))"
    finish FAIL "no enx* cdc_ncm interface appeared within ${IFACE_TIMEOUT}s"
  fi
  log "NCM interface: $IFACE_FOUND"
  if ! host_configure "$IFACE_FOUND"; then
    record_step "host config+ping" FAIL "$(( $(now_ns) - start_ns ))"
    finish FAIL "host configuration failed (ping $DEVICE_IP unreachable)"
  fi
  end_ns=$(now_ns)
  record_step "ncm iface+host config" OK "$((end_ns - start_ns))"

  # 2) detach a stale DAP and attach over USB-NCM (session boundary 1 starts)
  start_ns=$(now_ns)
  if [ "$DETACH_FIRST" -eq 1 ]; then
    detach_dap
  fi
  if ! attach_and_wait; then
    record_step "usbip attach (NCM)" FAIL "$(( $(now_ns) - start_ns ))"
    finish FAIL "USBIP attach over USB-NCM failed (device $DEVICE_IP, busid $BUSID)"
  fi
  record_step "usbip attach (NCM)" OK "$(( $(now_ns) - start_ns ))"

  # 3) session 1: flash + verify
  log "=== session 1: flash + verify ==="
  start_ns=$(now_ns)
  run_openocd_session "session1 flash+verify" "$LOG_DIR/session1.tcl" \
    "$LOG_DIR/session1.log"
  s1_rc=$?
  step_ns=$(( $(now_ns) - start_ns ))
  s1_verify="$(parse_verified_s "$LOG_DIR/session1.log")"
  if [ "$s1_rc" -ne 0 ] || [ ! -s "$LOG_DIR/session1.log" ]; then
    record_step "session1 flash+verify" FAIL "$step_ns"
    finish FAIL "session 1 OpenOCD failed (rc=$s1_rc); see $LOG_DIR/session1.log"
  fi
  record_step "session1 flash+verify" OK "$step_ns"
  log "session1 flash+verify: $(fmt_ms "$step_ns")" \
      "(target verify ${s1_verify:-n/a} s)"

  # 4) detach + re-attach -> session 2
  log "=== detach + re-attach (C1 second-session test) ==="
  start_ns=$(now_ns)
  detach_dap
  sleep 0.5
  if ! attach_and_wait; then
    record_step "detach + re-attach" FAIL "$(( $(now_ns) - start_ns ))"
    finish FAIL "detach/re-attach failed; session 2 cannot be tested"
  fi
  record_step "detach + re-attach" OK "$(( $(now_ns) - start_ns ))"

  # 5) session 2: read back 1 MiB and compare the image prefix sha256
  log "=== session 2: ${READ_SIZE}-byte readback ==="
  local methods=()
  case "$READ_METHOD" in
    flash) methods=(flash) ;;
    dump) methods=(dump) ;;
    auto) methods=(flash dump) ;;
  esac
  start_ns=$(now_ns)
  local method
  for method in "${methods[@]}"; do
    [ "$method" = "flash" ] && tcl="$LOG_DIR/session2-flash.tcl"
    [ "$method" = "dump" ] && tcl="$LOG_DIR/session2-dump.tcl"
    log "read method: $method"
    [ -f "$readback" ] && rm -f "$readback"
    run_openocd_session "session2 read ($method)" "$tcl" \
      "$LOG_DIR/session2-$method.log"
    s2_rc=$?
    if [ "$s2_rc" -eq 0 ] && [ -s "$readback" ]; then
      read_method_used="$method"
      read_ok=1
      break
    fi
    warn "read method '$method' failed (rc=$s2_rc); trying the next method"
    if [ "$method" = "flash" ] && [ "${#methods[@]}" -gt 1 ]; then
      # the failed read consumed the USBIP session: restart it for the retry
      detach_dap
      sleep 0.5
      attach_and_wait || true
    fi
  done
  step_ns=$(( $(now_ns) - start_ns ))
  if [ "$read_ok" -ne 1 ]; then
    record_step "session2 read" FAIL "$step_ns"
    finish FAIL "session 2 readback failed for method(s): ${methods[*]}; " \
        "see $LOG_DIR/session2-*.log"
  fi
  record_step "session2 read ($read_method_used)" OK "$step_ns"

  read_bytes=$(stat -c %s "$readback")
  log "readback: $read_bytes bytes in $(fmt_ms "$step_ns") " \
      "($(awk -v b="$read_bytes" -v n="$step_ns" \
        'BEGIN {printf "%.1f", b/1024.0/(n/1e9)}') KiB/s)"
  if [ "$read_bytes" -lt "$IMAGE_SIZE" ]; then
    record_step "readback size" FAIL 0
    finish FAIL "readback is smaller than the image ($read_bytes < $IMAGE_SIZE)"
  fi
  record_step "readback size" OK 0

  # 6) prefix sha256 comparison
  log "=== sha256 comparison (prefix of ${IMAGE_SIZE} B) ==="
  # root-created readback must be readable by this user
  if [ ! -r "$readback" ]; then
    run_root chmod 0644 "$readback" >/dev/null 2>&1 || true
  fi
  if [ ! -r "$readback" ]; then
    record_step "prefix sha256" FAIL 0
    finish FAIL "readback file is not readable: $readback"
  fi
  start_ns=$(now_ns)
  actual_prefix="$(head -c "$IMAGE_SIZE" "$readback" | sha256sum | awk '{print $1}')"
  expected_prefix="$EXPECTED_SHA256"
  end_ns=$(now_ns)
  if [ "${actual_prefix,,}" = "${expected_prefix,,}" ]; then
    record_step "prefix sha256" OK "$((end_ns - start_ns))"
    log "prefix sha256 MATCH: $actual_prefix"
  else
    record_step "prefix sha256" FAIL "$((end_ns - start_ns))"
    finish FAIL "prefix sha256 MISMATCH: readback $actual_prefix != image " \
        "$expected_prefix"
  fi

  # 7) cleanup / final state
  if [ "$DETACH_AT_END" -eq 1 ]; then
    detach_dap
    log "DAP detached (--detach-at-end)"
  else
    log "DAP left attached on USBIP (pass --detach-at-end to detach)"
  fi
  finish PASS ""
}

finish() { # result message
  local result="$1" message="${2:-}"
  local total_ns=$(( $(now_ns) - TOTAL_START_NS ))
  echo
  printf '================ %s: %s ================\n' "$LABEL" "$result"
  printf '%-26s %-6s %s\n' "step" "result" "time"
  printf '%-26s %-6s %s\n' "----" "------" "----"
  local i
  for i in "${!STEP_NAMES[@]}"; do
    printf '%-26s %-6s %s\n' "${STEP_NAMES[$i]}" "${STEP_RESULTS[$i]}" \
      "${STEP_TIMES[$i]}"
  done
  printf '%-26s %-6s %s\n' "total" "" "$(fmt_ms "$total_ns")"
  [ -n "$message" ] && log "reason: $message"
  log "artifacts: $LOG_DIR"
  if [ "$result" = "PASS" ]; then
    log "C1 USB-NCM acceptance PASSED"
    exit 0
  fi
  log "C1 USB-NCM acceptance FAILED"
  exit 1
}

main() {
  parse_args "$@"
  TOTAL_START_NS=$(now_ns)
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_run
    exit $?
  fi
  acceptance
}

main "$@"
