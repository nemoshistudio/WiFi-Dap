#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build-wired.sh -- build the INDEPENDENT wired-DAP (USB-NCM) variant (C1 MVP).
#
# This variant is NOT part of the default release matrix:
#   - default firmware:  scripts/build_all.sh  (Kconfig BIRDIEBOT_WIRED_TRANSPORT=NONE)
#   - wired firmware:    scripts/build-wired.sh (NCM, separate build dir)
#
# What it does:
#   1) configures build/esp32s3-wired with
#      SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp32s3;sdkconfig.wired.defaults"
#      (the wired defaults enable TinyUSB NCM, disable the USB-Serial/JTAG
#       secondary console and select BIRDIEBOT_WIRED_TRANSPORT_NCM);
#   2) builds, then verifies the key wires (transport/NCM/console) and prints
#      the app version/size;
#   3) touches nothing outside build/esp32s3-wired and managed_components/.
#
# Usage:
#   scripts/build-wired.sh              # configure + build
#   scripts/build-wired.sh --clean      # remove the wired build dir first
#   scripts/build-wired.sh -j 4         # parallel jobs (CMAKE_BUILD_PARALLEL_LEVEL)
#
# Flash / recovery / host test steps: docs/wired-dap-mvp.md.
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"

TARGET="esp32s3"
BUILD_REL="build/esp32s3-wired"
DEFAULTS="sdkconfig.defaults.esp32s3;sdkconfig.wired.defaults"
CLEAN=0
JOBS=""

log() { printf '[build-wired] %s\n' "$*"; }
die() { printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2; exit 2; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        -j|--jobs)
            [ "$#" -ge 2 ] || die "$1 requires an argument"
            JOBS="$2"
            shift 2
            ;;
        -j?*)
            JOBS="${1#-j}"
            shift
            ;;
        -t|--target)
            [ "$#" -ge 2 ] || die "$1 requires an argument"
            TARGET="$2"
            shift 2
            ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            die "unknown argument '$1' (try --help)"
            ;;
    esac
done

[ "$TARGET" = "esp32s3" ] || die "the wired NCM variant is ESP32-S3 only (got '$TARGET')"
[ -x "$SCRIPT_DIR/idf-docker.sh" ] || die "missing executable $SCRIPT_DIR/idf-docker.sh"
[ -f "$ROOT/CMakeLists.txt" ] || die "$ROOT is not an ESP-IDF project root"
[ -f "$ROOT/sdkconfig.wired.defaults" ] || die "missing $ROOT/sdkconfig.wired.defaults"

SDKCONFIG_REL="$BUILD_REL/sdkconfig"
BUILD_ABS="$ROOT/$BUILD_REL"

if [ "$CLEAN" -eq 1 ]; then
    log "removing $BUILD_REL"
    rm -rf "$BUILD_ABS"
fi
mkdir -p "$ROOT/build/logs"

log_file="$ROOT/build/logs/${TARGET}-wired.log"
: >"$log_file"
log "log: $log_file"

# Configure only when the build directory is missing or configured for another
# target; repeated runs are incremental (same policy as scripts/build_all.sh).
need_set_target=1
if [ -f "$BUILD_ABS/CMakeCache.txt" ] && \
   grep -qE "^IDF_TARGET:(STRING|INTERNAL)=$TARGET$" "$BUILD_ABS/CMakeCache.txt"; then
    need_set_target=0
fi

run_idf() {
    if [ -n "$JOBS" ]; then
        "$SCRIPT_DIR/idf-docker.sh" -C "$ROOT" -j "$JOBS" "$@"
    else
        "$SCRIPT_DIR/idf-docker.sh" -C "$ROOT" "$@"
    fi
}

if [ "$need_set_target" -eq 1 ]; then
    log "idf.py set-target $TARGET (SDKCONFIG_DEFAULTS=$DEFAULTS, SDKCONFIG=$SDKCONFIG_REL)"
    run_idf -B "$BUILD_REL" \
        -D "SDKCONFIG_DEFAULTS=$DEFAULTS" \
        -D "SDKCONFIG=$SDKCONFIG_REL" \
        -D "BIRDIEBOT_FW_RELEASE=OFF" \
        set-target "$TARGET" >>"$log_file" 2>&1
else
    log "reusing configured build directory ($BUILD_REL)"
fi

log "idf.py build${JOBS:+ (-j $JOBS)}"
build_args=(-B "$BUILD_REL" -D "SDKCONFIG=$SDKCONFIG_REL" -D "BIRDIEBOT_FW_RELEASE=OFF" build)
run_idf "${build_args[@]}" >>"$log_file" 2>&1

# --- verify the wired build actually selected the expected configuration -----
sdkconfig="$BUILD_ABS/sdkconfig"
[ -f "$sdkconfig" ] || die "missing $sdkconfig after build"
check_kconfig() {
    local sym="$1" want="$2"
    local got
    got="$(grep -E "^${sym}=" "$sdkconfig" | head -n1 || true)"
    if [ "$got" = "$sym=$want" ]; then
        log "config ok: $got"
    else
        printf '%s: error: expected %s=%s, got "%s" in %s\n' \
            "$SCRIPT_NAME" "$sym" "$want" "${got:-<not set>}" "$sdkconfig" >&2
        exit 1
    fi
}
check_kconfig CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM y
check_kconfig CONFIG_TINYUSB_NET_MODE_NCM y
check_kconfig CONFIG_ESP_CONSOLE_SECONDARY_NONE y
if grep -qE '^CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y' "$sdkconfig"; then
    die "USB-Serial/JTAG secondary console is still enabled in the wired build"
fi

bin="$BUILD_ABS/birdiebot_wifi_dap.bin"
[ -f "$bin" ] || die "build reported success but $bin is missing"
info="$(python3 - "$bin" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
ver = ""
if len(data) >= 0x50 and data[0] == 0xE9 and struct.unpack_from("<I", data, 0x20)[0] == 0xABCD5432:
    ver = struct.unpack_from("<32s", data, 0x30)[0].split(b"\0", 1)[0].decode("ascii", "replace")
print(f"{ver or 'EMPTY'} {len(data)}")
PY
)"
read -r ver size <<<"$info"
[ -n "$ver" ] && [ "$ver" != "EMPTY" ] || die "empty esp_app_desc.version in $bin"

printf '\n[build-wired] ===== wired (USB-NCM) build =====\n'
printf 'target       : %s\n' "$TARGET"
printf 'build dir    : %s\n' "$BUILD_REL"
printf 'version      : %s\n' "$ver"
printf 'app bin      : %s bytes\n' "$size"
printf 'device IP    : 192.168.7.1/24 (host: DHCP off, configure 192.168.7.2/24)\n'
printf 'DAP proxy    : TCP 3240 (usbip attach_ude -r 192.168.7.1 -b 1-1)\n'
printf 'console      : UART0 (USB-Serial/JTAG secondary console disabled)\n'
printf 'flash/recover: BOOT+RESET ROM USB-Serial/JTAG download mode / UART0 / Web OTA\n'
printf 'host steps   : docs/wired-dap-mvp.md\n'
