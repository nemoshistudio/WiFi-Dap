#!/usr/bin/env bash
# Build a recent OpenOCD (master) with CMSIS-DAP support for WiFi Dap.
#
# Why: OpenOCD 0.12.x uses the legacy queued DAP_Transfer path and is ~2.4x
# slower for memory reads over the wireless USBIP link. Current OpenOCD master
# uses DAP_TransferBlock, which our firmware supports; measured on STM32F407:
#   1 MiB read: 0.12 = 25.2 s -> master = 10.3 s (~2.4x)
#
# Usage: scripts/build-openocd-master.sh [install-dir]
# Default install dir: build/openocd-master-install
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
INSTALL="${1:-$ROOT/build/openocd-master-install}"
SRC="$ROOT/build/openocd-master"

log() { printf '[openocd-master] %s\n' "$*"; }
die() { printf '[openocd-master] ERROR: %s\n' "$*" >&2; exit 1; }

for t in git autoconf automake libtoolize make gcc pkg-config; do
  command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
done
pkg-config --exists libusb-1.0 || die "missing libusb-1.0 dev headers (apt: libusb-1.0-0-dev)"
pkg-config --exists hidapi-hidraw || log "warning: hidapi dev headers not found (CMSIS-DAP v2 USB backend may be unavailable; USBIP transport unaffected)"

if [ ! -d "$SRC/.git" ]; then
  log "cloning openocd master -> $SRC"
  git clone --depth 1 https://github.com/openocd-org/openocd "$SRC"
else
  log "reusing $SRC"
fi

cd "$SRC"
[ -x ./configure ] || ./bootstrap
if [ ! -f Makefile ]; then
  ./configure --prefix="$INSTALL" \
    --enable-internal-jimtcl --enable-cmsis-dap --disable-werror \
    --disable-jaylink --disable-ftdi --disable-stlink --disable-ti-icdi \
    --disable-ulink --disable-usb-blaster-2 --disable-presto --disable-ft232r \
    --disable-openjtag --disable-linuxgpiod --disable-remote-bitbang
fi
make -j"$(nproc)"
make install
log "binary : $INSTALL/bin/openocd"
log "scripts: $INSTALL/share/openocd/scripts"
log "version: $("$INSTALL/bin/openocd" --version 2>&1 | head -n1)"
log "use: OPENOCD_SCRIPTS=$INSTALL/share/openocd/scripts $INSTALL/bin/openocd ..."
