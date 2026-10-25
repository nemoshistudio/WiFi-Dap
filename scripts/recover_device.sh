#!/usr/bin/env bash
# Recovery for the OTA-503 / no-USB DAP unit (task-66 ring-resident firmware).
#
# Waits for the ESP32-S3 ROM USB-Serial/JTAG download mode, then flashes the
# current 8 MB OTA-layout image with Docker (esptool inside espressif/idf).
# Destructive: erase_flash clears NVS/Wi-Fi credentials.
#
# Usage: scripts/recover_device.sh --yes
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.2.1}"
BIN="$ROOT/build/esp32s3/birdiebot_wifi_dap.bin"
YES=0
[[ "${1:-}" == "--yes" ]] && YES=1
if [[ "$YES" != 1 ]]; then
  echo "This will erase NVS/Wi-Fi and flash $BIN."
  echo "Re-run with --yes after entering BOOT+RST mode."
  exit 2
fi
[[ -f "$BIN" ]] || { echo "missing $BIN (run scripts/build_all.sh -t esp32s3)"; exit 2; }
echo "[recover] waiting for ESP32-S3 ROM USB-Serial/JTAG (303a:1001) and /dev/ttyACM* ..."
for _ in $(seq 1 240); do
  if lsusb -d 303a:1001 >/dev/null 2>&1; then
    for tty in /dev/ttyACM*; do
      [[ -e "$tty" ]] || continue
      echo "[recover] found $tty"
      docker run --rm --user "$(id -u):$(id -g)" \
        --group-add "$(getent group plugdev | cut -d: -f3)" -e HOME=/tmp \
        --device="$tty" -v "$ROOT:/project" -w /project "$IMAGE" \
        bash -c "python -m esptool --chip esp32s3 -p '$tty' erase_flash && \
          python -m esptool --chip esp32s3 -p '$tty' -b 460800 \
            write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
            0x0 build/esp32s3/bootloader/bootloader.bin \
            0x8000 build/esp32s3/partition_table/partition-table.bin \
            0x20000 build/esp32s3/birdiebot_wifi_dap.bin"
      echo "[recover] flashed; waiting for HTTP..."
      for _ in $(seq 1 60); do
        if curl -fsS -m 2 http://<device-ip>/api/target/status >/tmp/recover-status.json 2>/dev/null; then
          echo "[recover] device up:"; cat /tmp/recover-status.json; echo
          exit 0
        fi
        sleep 2
      done
      echo "[recover] flashed but HTTP not reachable yet"; exit 0
    done
  fi
  sleep 2
done
echo "[recover] timed out waiting for BOOT+RST"; exit 1
