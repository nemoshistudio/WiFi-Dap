# WiFi Dap

WiFi Dap is a wireless **CMSIS-DAP v2** debug probe for the **ESP32-S3**, with a browser-based management and debugging UI. It exposes SWD/JTAG over Wi-Fi or USB-NCM and can also flash a connected target.

**Hardware support:** **ESP32-S3 is the supported and tested target.** The source tree also contains build targets for ESP32-C3, ESP32 and ESP32-C5 for reference, but they are **untested and unsupported**; do not use them for production.

## Features

- CMSIS-DAP v2 (SWD/JTAG) over Wi-Fi and USB-NCM
- ESP32-S3 dedicated-GPIO SWD engine: 100 kHz–20 MHz at 160 MHz CPU, up to 24 MHz with the 240 MHz CPU variant
- Web UI: device status, Wi-Fi setup, target detection/flashing, variable curves, UART terminal
- Target flashing, SWD clock selection, curve sampling and UART bridge
- Optional OTA firmware update (`POST /api/ota/upload`)
- mDNS discovery, unique per-device serial and USB identity

## Hardware (ESP32-S3)

Default board: **WiFi Dap S3**, ESP32-S3 module (N16R8 class, 8 MB partition layout).

| Function | GPIO |
|---|---|
| SWCLK / TCK | GPIO12 |
| SWDIO / TMS | GPIO11 |
| nRESET | GPIO13 |
| TDO (JTAG, optional) | GPIO15 |
| TDI (JTAG, optional) | GPIO16 |
| nTRST (optional) | GPIO14 |
| UART bridge TX / RX | GPIO10 / GPIO9 |
| Status LEDs | GPIO5 / GPIO6 / GPIO4 |

Full pin map: [docs/board-pins.md](docs/board-pins.md).

## Build (ESP32-S3)

Requirements: Docker (recommended) or ESP-IDF 5.5.6, plus Node.js 20+ only if you rebuild the Web UI.

```bash
# Default 160 MHz build (includes the Web UI and all S3 features)
scripts/build_all.sh -t esp32s3 -j 8

# Optional 240 MHz variant (enables the 24 MHz SWD tier)
scripts/idf-docker.sh -B build/esp32s3-240 \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3.240mhz" \
  -D "SDKCONFIG=build/esp32s3-240/sdkconfig" set-target esp32s3
scripts/idf-docker.sh -B build/esp32s3-240 \
  -D "SDKCONFIG=build/esp32s3-240/sdkconfig" build

# Optional OTA-enabled variant (module 5 + POST /api/ota/upload)
# add sdkconfig.defaults.esp32s3.ota to SDKCONFIG_DEFAULTS above
```

The image is written to `build/esp32s3-240/birdiebot_wifi_dap.bin` for the 240 MHz variant.

## First flash

Enter the ESP32-S3 ROM download mode (hold **BOOT**, press **RESET**, release BOOT), then:

```bash
docker run --rm --device=/dev/ttyACM0 -v "$PWD:/project" -w /project \
  espressif/idf:v5.5.6 bash -lc \
  'idf.py -B build/esp32s3 -p /dev/ttyACM0 flash'
```

After the first USB flash, later updates can use the built-in OTA (if the OTA overlay was enabled):

```bash
python3 scripts/ota_upload.py --host 192.168.7.1 \
  --file build/esp32s3/birdiebot_wifi_dap.bin --info
```

## Web UI

1. Power the probe and connect to its AP `WiFi-Dap-XXXX` (default password `12345678`).
2. Open `http://192.168.1.1/`.
3. Change the AP password in **Wi-Fi** and join your own network if needed.

## Debugging

- **OpenOCD/pyOCD/Keil**: standard CMSIS-DAP probe; default SWD clock 2 MHz.
- **OpenOCD over Wi-Fi** (when the TCP variant is built): `cmsis-dap backend tcp` on port **3241**.
- **USBIP/WinUSB** and elaphureLink: use standard CMSIS-DAP tooling (select the CMSIS-DAP probe in your debugger).
- Target firmware and debugging use the same SWD pins; 16 MHz is the recommended adapter speed for long wires.

## Usage

See [docs/usage.md](docs/usage.md) for the Web UI, USBIP, OpenOCD and OTA workflows.

## Documentation

Start at [docs/README.md](docs/README.md):

- [Protocol](docs/protocol.md)
- [Board pins](docs/board-pins.md)
- [Local curve sampling](docs/curve-local-sampler.md)
- [Versioning](docs/versioning.md)

## License and attribution

Project-original code is **Apache-2.0** (see [LICENSE](LICENSE)); some scripts are MIT. Upstream projects remain under their own licenses and are credited in [NOTICE](NOTICE):

- [windowsair/wireless-esp8266-dap](https://github.com/windowsair/wireless-esp8266-dap) — MIT
- [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) — Apache-2.0
- [kerms/wireless-esp32-tools-web-host](https://github.com/kerms/wireless-esp32-tools-web-host) — MIT
- ARM CMSIS-DAP — Apache-2.0

WiFi Dap is maintained by its contributors. See [CONTRIBUTORS.md](CONTRIBUTORS.md); development assistance by [DeepSeek](https://github.com/deepseek-ai). The upstream authors above are dependencies and are credited, not the authors of this repository.
