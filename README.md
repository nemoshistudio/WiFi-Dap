# WiFi Dap

**English** | [中文](README_CN.md)

A wireless CMSIS-DAP debug probe built on the **ESP32-S3**, with a full embedded Web management panel. Flash it once over USB, then debug over Wi-Fi (SWD/JTAG), bridge UART traffic, manage the device, and perform browser-local OTA updates.

The project is a merge/rework of [windowsair/wireless-esp8266-dap](https://github.com/windowsair/wireless-esp8266-dap) (MIT) and [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) (Apache-2.0 platform layer), upgraded to **ESP-IDF 5.2.1** and extended with a Web UI plus a JSON/WebSocket protocol. Full documentation index: **[docs/README.md](docs/README.md)**.

> ⚠️ **Secure-by-default warning**: the Web/JSON API, WebSocket-DAP, UART TCP bridge and OTA upload are **unauthenticated by default**, and all transports are plaintext. Deploy only on a **trusted LAN** and read [§9 Security defaults & hardening](#9-security-defaults--hardening).

---

## Table of contents

1. [Overview](#1-overview)
2. [Feature matrix](#2-feature-matrix)
3. [Hardware & pinout](#3-hardware--pinout)
4. [Build & flash](#4-build--flash)
5. [Quick start](#5-quick-start)
6. [Web UI](#6-web-ui)
7. [Debugger transports](#7-debugger-transports)
8. [UART & data paths](#8-uart--data-paths)
9. [Security defaults & hardening](#9-security-defaults--hardening)
10. [Versions & protocol](#10-versions--protocol)
11. [Origins & changes from upstream](#11-origins--changes-from-upstream)
12. [Repository layout](#12-repository-layout)
13. [Testing & validation](#13-testing--validation)
14. [Troubleshooting](#14-troubleshooting)
15. [Documentation, feedback & license](#15-documentation-feedback--license)

---

## 1. Overview

- **What it is**: an ESP32-S3 (8 MB flash) turned into a wireless CMSIS-DAP probe plus a self-hosted Web dashboard.
- **Debug transports**: USBIP/WinUSB (CMSIS-DAP v2), elaphureLink (driver-free Keil), and WebSocket-DAP (browser tools) — all on **TCP 3240**.
- **Management path**: an embedded single-page Web UI (Vue 3 + Vite, gzip-embedded into the firmware) served over **HTTP 80**, covering Wi-Fi setup, UART terminal, OTA upload, **Web-side STM32 target flashing**, version info and credits.
- **Release target**: **ESP32-S3 8 MB** (default board `BOARD_ZXR_DAP_S3`, dual OTA slots). ESP32-C3 / ESP32 (4 MB) are **compile-only**: no OTA, not hardware-validated.
- **Removed**: ESP8266 support, experimental KCP transport, corsacOTA (port 3241).

---

## 2. Feature matrix

| Category | Feature | Status | Notes |
|---|---|---|---|
| SoC | ESP32-S3 | ✅ hardware target | 8 MB flash, default board `BOARD_ZXR_DAP_S3`, dual OTA slots |
| SoC | ESP32-C3 / ESP32 | 🧪 compile-only | 4 MB single-app partition, no OTA, not hardware-validated |
| Debug | SWD / JTAG | ✅ | CMSIS-DAP implementation; pins in [docs/board-pins.md](docs/board-pins.md); default SWD clock **2 MHz** |
| Target flash | Web direct flashing / STM32 identify | ✅ F4/H7 flash verified | Browser uploads `.bin`; ESP32 streams it over SWD (sector-selective erase, x64/x32, SHA-256 verify + SWD reset); **all STM32 series are identified** (DEV_ID table, H7 real-hardware ID); F4 and H7 flash are real-hardware verified; see [docs/stm32-web-flash-plan.md](docs/stm32-web-flash-plan.md) |
| Transport perf | USBIP/DAP tuning | ✅ | A3: ~43 KiB/s 64 KB read over Wi-Fi (latency-bound); UART bridge is paused during target flashing while Wi-Fi/HTTP stay online |
| USB transport | USBIP / WinUSB | ✅ | CMSIS-DAP v2 on TCP **3240**; Windows: usbip-win 0.3.0 `attach_ude` |
| Toolchains | OpenOCD / pyOCD / Keil | ✅ | Via CMSIS-DAP v2; **use OpenOCD master for 2.4× faster reads** (see [docs/openocd-master.md](docs/openocd-master.md)) |
| Driver-free | elaphureLink / OpenOCD-elaphureLink | ✅ | Keil plugin or OpenOCD, TCP **3240**, no USBIP driver required |
| Browser transport | WebSocket-DAP (legacy) | ✅ (can disable) | TCP **3240**, binary frames = CMSIS-DAP command packets; `CONFIG_BIRDIEBOT_DAP_WS_ENABLE` |
| Web UI | Embedded dashboard (zh/en) | ✅ | HTTP **80**, single SharedWorker WS connection; Wi-Fi / UART / Update / About / Feedback |
| Wi-Fi | AP + STA (Smart APSTA) | ✅ | Default AP `WiFi-Dap-XXXX` / `12345678`; AP off 30 s after STA connects, on 10 s after disconnect |
| Wi-Fi | Multi-credential memory | ✅ | Up to 8 networks, auto-save + rotation on boot/disconnect ([contract](docs/wifi-credential-memory.md)) |
| Serial | Web UART terminal + WS binary | ✅ | 115200-8N1 default; data bits / parity / stop bits / flow control configurable |
| Serial | UART TCP bridge | ✅ (can disable) | TCP **1346**; first text packet may switch baud rate; `CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE` |
| Discovery | mDNS / SSDP | ✅ | mDNS `_http._tcp`; SSDP 1900/UDP + `GET /SSDP.xml` |
| OTA | Browser-local upload (dual slot + rollback) | ✅ S3 8 MB only | `POST /api/ota/upload`, see [contract](docs/ota-local-upload.md) |
| Diagnostics | coredump to flash | ✅ | 64 KB `coredump` partition (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`) |
| Security | No login/auth (lab mode); Host/Origin guard always on | ✅ | see [§9](#9-security-defaults--hardening) |
| Versioning | Firmware semver + protocol v1 | ✅ | About page / `SYSTEM/GET_FM_INFO`, see [docs/versioning.md](docs/versioning.md) |

---

## 3. Hardware & pinout

- Board pins and defaults are a **frozen contract**: [docs/board-pins.md](docs/board-pins.md) is the single source of truth.
- Board selection: `idf.py menuconfig → WiFi Dap board configuration → Target board`. Kconfig provides per-target defaults after `set-target`.

| IDF target | Default board | Notes |
|---|---|---|
| `esp32s3` | `BOARD_ZXR_DAP_S3` | Default board, WiFi Dap hardware, 8 MB |
| `esp32s3` | `BOARD_GENERIC_S3` | Generic S3 wiring, no status LEDs, compile/reference only |
| `esp32c3` | `BOARD_GENERIC_C3` | Compile-only |
| `esp32` | `BOARD_GENERIC_ESP32` | Compile-only |

Key pins of the default `BOARD_ZXR_DAP_S3`:

| Function | Pin | Notes |
|---|---|---|
| SWCLK / TCK | GPIO12 | do not change |
| SWDIO / TMS | GPIO11 | single-wire SWDIO |
| TDO (target → host) | GPIO15 | JTAG |
| TDI (host → target) | GPIO16 | JTAG |
| nTRST / nRESET | GPIO14 / GPIO13 | nTRST optional |
| Connected / Running LED | GPIO5 / GPIO6 | driven by CMSIS-DAP HostStatus |
| Wi-Fi status LED | GPIO4 | |
| UART bridge TX / RX | GPIO10 / GPIO9 | UART1; the console stays on UART0 (S3: GPIO43/44) |

> Generic C3/ESP32 pinouts and the "no LED" board convention (`_` macros) are documented in [docs/board-pins.md §4](docs/board-pins.md). Never hard-code `#ifdef CONFIG_IDF_TARGET_*` pin branches elsewhere.

**Power**: tie 3V3/GND to the target board; probe GPIOs are 3.3 V.

---

## 4. Build & flash

### 4.1 Prerequisites

- **Recommended**: Docker (`espressif/idf:v5.2.1` image) plus `bash`, `flock`, `python3`, `gcc`.
- A local **ESP-IDF v5.2.1** installation also works (`main/idf_component.yml` requires `idf >= 5.2.0`).
- Node.js (20+) and npm are only needed to rebuild the Web UI; firmware builds use the committed `project_components/html/*.gz` assets.

```bash
docker pull espressif/idf:v5.2.1
```

### 4.2 One-shot build (Docker)

```bash
# all three targets (S3 8MB + C3 4MB + ESP32 4MB, with partition gates)
scripts/build_all.sh

# release target only / clean rebuild
scripts/build_all.sh -t esp32s3
scripts/build_all.sh --clean -t esp32s3
```

- Each target has its own build dir and SDKCONFIG: `build/<target>/`, `build/<target>/sdkconfig`; logs in `build/logs/<target>.log`.
- The script validates partition layout and app size, and parses `esp_app_desc.version` (must be non-empty).
- More options and manual build steps: [scripts/README.md](scripts/README.md).

### 4.3 Manual build (equivalent)

```bash
scripts/idf-docker.sh -B build/esp32s3 \
    -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 \
    -D SDKCONFIG=build/esp32s3/sdkconfig set-target esp32s3
scripts/idf-docker.sh -B build/esp32s3 -D SDKCONFIG=build/esp32s3/sdkconfig build
```

> A shared root `sdkconfig` is overwritten by every target; always put SDKCONFIG into the per-target build directory (`build_all.sh` already does this).

### 4.4 Partitions & images

| Target | Partition table | Layout | OTA |
|---|---|---|---|
| ESP32-S3 (8 MB, release) | [`partitions/partitions_8mb_ota.csv`](partitions/partitions_8mb_ota.csv) | bootloader + ptable@0x8000 + nvs 24K + phy 4K + otadata 8K + `ota_0` 3M@0x20000 + `ota_1` 3M@0x320000 + wt_nvs 64K + coredump 64K | ✅ dual slot + rollback |
| ESP32-C3 / ESP32 (4 MB) | [`partitions/partitions_4mb_single.csv`](partitions/partitions_4mb_single.csv) | nvs + phy + `factory` 3.75M + wt_nvs + coredump | ❌ no OTA |

App-size limits, otadata rules and rollback semantics: [docs/ota-local-upload.md §1](docs/ota-local-upload.md).

### 4.5 First flash (ESP32-S3 8 MB)

Write **bootloader + partition table + ota_0** only; leave `otadata` blank (0xFF) so the bootloader falls back to `ota_0`:

```bash
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  --device=/dev/ttyUSB0 -v "$PWD:/project" -w /project espressif/idf:v5.2.1 \
  bash -c 'esptool.py --chip esp32s3 -p /dev/ttyUSB0 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 build/esp32s3/bootloader/bootloader.bin \
    0x8000 build/esp32s3/partition_table/partition-table.bin \
    0x20000 build/esp32s3/birdiebot_wifi_dap.bin'
```

- ❌ Do **not** write `build/esp32s3/ota_data_initial.bin`, and do **not** first-flash a `merge_bin` image (it initializes otadata and defeats the fallback path).
- Serial console: UART0, 115200 (S3: GPIO43/44).
- Later upgrades go through Web UI OTA or `scripts/ota_upload.py` — no full re-flash needed.

### 4.6 Release package (optional)

```bash
scripts/release_local.sh                 # strict: clean tree + exact vX.Y.Z tag
scripts/release_local.sh --allow-dirty   # local validation
```

Artifacts land in `build/release/`: `birdiebot_wifi_dap.bin` (the only image accepted by OTA), bootloader, partition table, `flash.sh`, Web gz assets, `SHA256SUMS`, etc. See [scripts/README.md](scripts/README.md).

### 4.7 Rebuild the Web UI (optional)

```bash
cd web && ./build.sh             # writes web/dist and web/dist-gz
cd .. && scripts/gen_web_assets.sh --install   # explicitly updates project_components/html/
```

> Without `--install`, `gen_web_assets.sh` only stages into `build/web-assets/`; use `--check` to verify the committed gz assets match the current Web UI build.

---

## 5. Quick start

1. First-flash as in [§4.5](#45-first-flash-esp32-s3-8-mb); confirm on the serial console (UART0 115200) that boot has no panic.
2. Join the device hotspot from your PC/phone:
   - SSID: `WiFi-Dap-XXXX` (`XXXX` = last two bytes of the STA MAC, uppercase hex, e.g. `WiFi-Dap-A1B2`)
   - Default password: `12345678` (**change it in the Wi-Fi page on first use**, see [§9](#9-security-defaults--hardening))
3. Open **<http://192.168.1.1/>** in a browser (default AP gateway/device address).
   - If the device has been joined to your LAN via the Wi-Fi page, use the IP assigned by your router instead;
   - mDNS: the device registers an `_http._tcp` service under hostname `wifi-dap` (`project_components/wt_mdns/wt_mdns_config.h`), so `http://wifi-dap.local/` should work depending on your resolver — otherwise use the device IP.
4. In the **Wi-Fi** page, join your LAN (multiple networks are remembered and rotated automatically). Afterwards the Web UI, debugger ports and UART bridge are reachable from that LAN.
5. Open the **UART** page (115200-8N1 by default) to watch the target's serial output, or start debugging over USBIP/elaphureLink ([§7](#7-debugger-transports)).

> The device runs Smart APSTA by default: the AP stays up while STA is disconnected, turns off 30 s after a successful STA connection, and comes back 10 s after a disconnect (delays configurable via Wi-Fi page/JSON API).

---

## 6. Web UI

Entry point: `http://<device-ip>/` (defaults to `/#/uart`). The frontend talks to the firmware over a single WebSocket (`ws://<ip>/ws`): text frames are JSON, binary frames are 4-byte header + payload; the firmware sends an empty TEXT heartbeat every **1500 ms**. See [docs/protocol.md](docs/protocol.md) and [docs/ws-binary-router.md](docs/ws-binary-router.md).

| Page | Route | Purpose |
|---|---|---|
| **UART** | `/#/uart` | Serial terminal (TX/RX) and baud/data-bits/parity/stop-bits/flow control; traffic uses WS binary `RAW`/`RAW_BROADCAST` frames |
| **Wi-Fi** | `/#/wifi` | STA scan/connect, saved networks (up to 8, add/delete, auto-save), AP name/password, static IP/DNS, AP/STA mode and Smart APSTA delays |
| **Update** | `/#/update` | Browser-local OTA: pick an app `.bin`, live progress, verification and rollback (S3 8 MB only; 4 MB targets show "unsupported") |
| **About** | `/#/about` | Firmware / protocol / Web UI / hardware versions, serial number, IDF version |
| **Feedback** | `/#/feedback` | Feedback entry points (author/project pages) |

- Languages: Chinese / English (312 i18n keys, see [docs/i18n-keys.md](docs/i18n-keys.md)).
- Security: no login/auth (removed by design); the Web UI is usable from the trusted LAN only (see [§9](#9-security-defaults--hardening)).
- Screenshots: [UART terminal](web/assets/uart.png)
- Note: the former Widget / `#/widget` dashboard was removed on 2026-10-21 (the UART page keeps the data viewer, macros and its full feature set).

### OTA (ESP32-S3 8 MB only)

1. On the `Update` page select `build/esp32s3/birdiebot_wifi_dap.bin` (**app image only** — not a merge/full image, not a zip).
2. The device writes into the **free slot** (`ota_0` ↔ `ota_1`); states: `IDLE → RECEIVING → VERIFYING → READY → APPLYING`, then reboots after ~1 s.
3. The new image boots as `PENDING_VERIFY`; `main` confirms it after 10 s. If the device crashes/resets within 10 s, the bootloader rolls back to the previous slot.
4. If the current Web build does not expose the Update page (build flag `VITE_ENABLE_OTA`), use the equivalent HTTP/script flow:
   - `python3 scripts/ota_upload.py --host <ip> --file <app.bin> --info`
   - or raw `curl`, see [docs/hardware-test-ota.md §2/§3](docs/hardware-test-ota.md).

---

## 7. Debugger transports

All DAP transports share **TCP 3240** (USBIP / elaphureLink / WebSocket-DAP are auto-detected from the first bytes).

### 7.1 USBIP / WinUSB (recommended on Windows)

1. Install [usbip-win 0.3.0 (kmdf/ude)](https://github.com/cezanne/usbip-win).
2. Make sure the PC and device are on the same network (AP mode, or the device joined your LAN):

```bash
# WinUSB mode (recommended)
usbip.exe attach_ude -r <device-ip> -b 1-1

# legacy usbip form (HID-mode builds; this firmware is WinUSB by default — HID requires rebuilding with USE_WINUSB=0)
usbip.exe -D -a <device-ip> 1-1
```

Windows binds WinUSB automatically through WCID / MS OS 2.0 descriptors; the probe appears as a CMSIS-DAP debugger (VID:PID = `C251:F00A`). Linux ships a usbip client in the kernel, but Linux is not part of this project's validation scope.

### 7.2 elaphureLink (driver-free Keil)

- Install the [elaphureLink](https://github.com/windowsair/elaphureLink) Keil plugin and connect to `TCP:3240`; no USBIP driver required.
- OpenOCD users can use [OpenOCD-elaphureLink](https://github.com/windowsair/openocd-elaphurelink).

### 7.3 WebSocket-DAP (legacy, browser tools)

- Also on port 3240: after an HTTP `GET ` upgrade, **binary frame payloads are CMSIS-DAP command packets** (see [components/dap_proxy/](components/dap_proxy/)).
- Disable with `CONFIG_BIRDIEBOT_DAP_WS_ENABLE=n` (USBIP/elaphureLink are unaffected; the port stays open for them).
- Intended for browser-based tools such as [elaphure-dap.js](https://github.com/windowsair/elaphure-dap.js).

### 7.4 OpenOCD / pyOCD / Keil

The device presents itself as a standard **CMSIS-DAP v2** probe (SWD/JTAG); select CMSIS-DAP in OpenOCD, pyOCD or Keil. The default SWD clock is **1 MHz** (`BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ`, see [docs/board-pins.md §5](docs/board-pins.md)); higher clocks are chosen by the host tool.

---

## 8. UART & data paths

- **Web UART**: send/receive directly on the UART page (WS binary frames), 115200-8N1 by default.
- **UART TCP bridge**: raw TCP server on **port 1346**: `data → TCP → target UART TX`, `target UART RX → TCP → data`.
  - The **first text packet** on a new connection is interpreted as a baud rate if valid (2–7 digits, e.g. `115200`); otherwise it is forwarded as normal data.
  - Default baud is 115200 (the old 1234/74880 defaults are deprecated); disable with `CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n`.
  - Example: `nc <device-ip> 1346`.
- **Console UART0** (S3: GPIO43/44, 115200) is for firmware logs only and is not used by the bridge or Web UART.
- Web UART and the TCP bridge share the same UART service and can be connected simultaneously; multi-client broadcast semantics are covered in [docs/hardware-test-uart-dataflow.md](docs/hardware-test-uart-dataflow.md).

---

## 9. Security defaults & hardening

### 9.1 Defaults (laboratory mode)

| Surface | Default | Risk |
|---|---|---|
| Web UI / JSON API / WS data plane | no auth | any host on the LAN can read config, drive UART, start OTA |
| OTA upload (`POST /api/ota/upload`) | no auth | arbitrary firmware can be written (no signature check) |
| USBIP / elaphureLink / WS-DAP (3240) | no auth | probe can be attached and the target chip read/written |
| UART TCP bridge (1346) | no auth | direct access to the target's serial port |
| Transport encryption | none (HTTP/WS/TCP plaintext) | sniffable/tamperable on the same network |
| Wi-Fi credential storage | plaintext NVS (no flash encryption) | readable with physical access; `GET_AP_INFO`/`GET_STA_INFO` return passwords |
| AP password | `12345678` | change it on first use |

> Recommendation: use only on a **trusted LAN / isolated VLAN**. Never expose the probe ports to an office network or the Internet.

### 9.2 Known security limitations (lab mode)

The default firmware is designed for "laboratory mode". The following limitations **remain** until the device is hardened. Full severity breakdown, evidence line numbers and remediation: **[docs/security-review-web.md](docs/security-review-web.md)** (P0 current 0 / conditional P0 1 / P1 4 / P2 10):

- **No authentication by default**: HTTP **80** (`/`, `/ws`, `/api`), TCP **3240** (USBIP / elaphureLink / WS-DAP) and TCP **1346** (UART TCP bridge) are all unauthenticated; anyone on the same network can fully control the device and the target MCU.
- **Default AP password `12345678`**: physically nearby users can join the device hotspot and gain all of the above; change it on first use.
- **No login/auth at all (by design)**: the pairing-code/token feature was removed; 80/3240/1346 are open on the trusted LAN. Host/Origin validation stays enabled for browser CSWSH/DNS-rebinding protection, but it is not authentication.
- **OTA is unsigned / no anti-downgrade**: `esp_ota_end()` only validates image format; anti-rollback and Secure Boot are not enabled, so any valid image can be written from the trusted LAN.

> Note: Origin/Host validation is now **always enabled** (decoupled from `auth`, default: IP-literal Host + same-origin only), closing the CSWSH/DNS-rebinding gap reported in review §1.2. Deployment hardening checklist: [docs/security.md](docs/security.md).

**Accepted risks (studio LAN)**: the device is **open by design** (no login/auth; 80/3240/1346 unauthenticated) and the default AP password stays `12345678` until changed. These are accepted for a **trusted studio LAN**; Host/Origin validation remains enabled for browser-origin attacks. Use constraints and N1–N9 status: [docs/security.md](docs/security.md). **Never expose the device to the public Internet or untrusted Wi-Fi.**

### 9.3 Host/Origin validation (no login)

Login/pairing-code/token authentication has been **removed**. What remains is an always-on browser-origin guard:

- `Host` must be an IP literal, `CONFIG_LWIP_LOCAL_HOSTNAME`, or listed in `BIRDIEBOT_WEB_ALLOWED_ORIGIN` (DNS-rebinding protection);
- when a browser sends `Origin`, it must be same-origin or whitelisted (CSWSH protection);
- non-browser clients (curl, OpenOCD, scripts) send no Origin and are allowed — this is **not authentication**.

If you access the UI through a hostname (mDNS/reverse proxy), add it to `BIRDIEBOT_WEB_ALLOWED_ORIGIN` (e.g. `http://wifi-dap.local`). Scripts no longer accept/need `--token`.

### 9.4 Disabling unused transports

```text
idf.py menuconfig
  → WiFi Dap proxy (USBIP / WebSocket-DAP) → BIRDIEBOT_DAP_WS_ENABLE = n
  → WiFi Dap UART TCP bridge                  → BIRDIEBOT_UART_BRIDGE_ENABLE = n
```

USBIP and elaphureLink have no separate switch (port 3240 remains open); if debugging is not needed, block ports 3240/1346 at the network layer.

### 9.5 Other recommendations

- Change the AP password after first use (Wi-Fi page → AP settings).
- Only join trusted STA networks; avoid public hotspots.
- Enable Secure Boot / Flash Encryption for released firmware (off by default); `esp_ota_end()` will then verify image signatures.
- Coredumps may contain sensitive memory (including Wi-Fi passwords); treat dump files as confidential.

---

## 10. Versions & protocol

| Version | Source | Visible at runtime |
|---|---|---|
| Firmware `vMAJOR.MINOR.PATCH` | local git tag (`git describe`, falling back to `version.txt`/`v1.0.0`) | boot log, About page, `SYSTEM/GET_FM_INFO.fm_ver`, `OTA/GET_INFO.running_ver` |
| Protocol `protocol_version = 1` | `project_components/wt_common/wt_protocol_version.h` (single source) | boot log, `GET_FM_INFO.protocol_version`, About page |
| Web UI `1.0.0` | `web/package.json`, injected at build time | About page, `build-info.json`, `web_version.txt` |

- After connecting, the frontend performs a `SYSTEM/GET_FM_INFO` version handshake. On protocol mismatch it warns "firmware and web UI versions do not match" and disables dangerous actions (e.g. OTA).
- Release consistency is enforced by gate scripts such as `scripts/check-release-versions.sh`; see [docs/versioning.md](docs/versioning.md) and [scripts/README.md](scripts/README.md).

---

## 11. Origins & changes from upstream

Three sources (details and per-file mapping: [docs/merge-map.md](docs/merge-map.md)):

| Ref | Repository | License | Contribution |
|---|---|---|---|
| **A** | [nemoshistudio/wireless-esp8266-dap](https://github.com/nemoshistudio/wireless-esp8266-dap) (fork of windowsair's project) | MIT | CMSIS-DAP core, USBIP/elaphureLink proxy, fork changes (LED, S3 UART, …) |
| **C** | [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) | Apache-2.0 | ESP-IDF 5.2.1 platform: wifi_manager / web_server / wt_nvs / memory_pool / SSDP / UART service / DataFlow |
| **D** | [kerms/wireless-esp32-tools-web-host](https://github.com/kerms/wireless-esp32-tools-web-host) | MIT | Vue 3 + Vite Web UI (extended here with OTA / Wi-Fi management / auth / i18n) |

What this repository (WiFi Dap) changes:

- Upgraded to **ESP-IDF 5.2.1** and componentized (`components/` + `project_components/`); board pins abstracted into `components/board` (`BOARD_*` + Kconfig, no per-target pin branches elsewhere).
- Added the **Web UI + JSON/WS protocol v1** ([docs/protocol.md](docs/protocol.md)), multi-credential Wi-Fi memory, browser-local OTA (8 MB dual slot + rollback), optional Web auth, SSDP/mDNS, coredump, and the UART service/DataFlow data plane.
- Kept and hardened the upstream DAP proxy layer (USBIP / elaphureLink / legacy WebSocket-DAP): ring-buffer lifecycle owned by the DAP task, socket fd leak fixes, error-path handling, etc.
- Changed defaults: SWD clock 1 MHz, UART bridge 1346/115200, AP SSID `WiFi-Dap-XXXX`; removed ESP8266/KCP/corsacOTA (3241).
- Default board `BOARD_ZXR_DAP_S3`; C3/ESP32 compile-only.

---

## 12. Repository layout

```text
.
├── main/                  # app_main and DAP configuration entry point
├── components/            # firmware components (DAP, USBIP, dap_proxy, board, SSDP, wt_ota, …)
├── project_components/    # IDF5 project components (wifi_manager, web_server, api_router, html, wt_mdns, …)
├── web/                   # Vue 3 Web UI sources + build scripts (gz output embedded into firmware)
├── partitions/            # 8MB dual-OTA / 4MB single-app partition tables
├── scripts/               # Docker builds, partition/board checks, web assets, release, OTA/E2E tools
├── docs/                  # contracts and test docs (index: docs/README.md)
├── sdkconfig.defaults.*   # per-target defaults
└── CMakeLists.txt         # version injection + project definition
```

`build/` and `build-*/` are gitignored; `project_components/html/*.gz` are committed Web assets.

---

## 13. Testing & validation

### 13.1 Local checks (no hardware)

```bash
scripts/build_all.sh                         # three targets + partition/app gates
python3 scripts/check_partitions.py          # partition contract + app size + embedded version
python3 scripts/check_board_pins.py          # board headers vs docs/board-pins.md contract
scripts/gen_web_assets.sh --check            # committed web gz vs current web/ build
```

### 13.2 WebSocket / UART / OTA automation (device required)

```bash
# connectivity + version handshake
python3 scripts/e2e_ws_client.py --host <ip> ping
# Wi-Fi scan / UART loopback / DataFlow / OTA info / arbitrary JSON
python3 scripts/e2e_ws_client.py --host <ip> wifi-scan
python3 scripts/e2e_ws_client.py --host <ip> uart-echo --payload 48656c6c6f --hex
python3 scripts/e2e_ws_client.py --host <ip> dataflow-list
python3 scripts/e2e_ws_client.py --host <ip> ota-info
# OTA upload (also polls for the new version)
python3 scripts/ota_upload.py --host <ip> --file build/esp32s3/birdiebot_wifi_dap.bin --info
```

### 13.3 Hardware checklists

- **OTA** (8 MB S3): [docs/hardware-test-ota.md](docs/hardware-test-ota.md) — first flash, Web/HTTP upload, error injection, rollback checks and a record template.
- **UART/DataFlow/WS binary**: [docs/hardware-test-uart-dataflow.md](docs/hardware-test-uart-dataflow.md) — scenarios T1–T8 (connectivity/config/loopback/multi-client broadcast, …).
- **DAP/USBIP hardware validation**: checklist being produced by dap-dev; it will be linked from [docs/README.md](docs/README.md) once landed.

---

## 14. Troubleshooting

| Symptom | Check first |
|---|---|
| `WiFi-Dap-XXXX` AP not found | first flash done? correct board (`sdkconfig.defaults.*`)? Wi-Fi init errors in the serial log? AP turns off 30 s after STA connects (normal) |
| `192.168.1.1` does not respond | is the client on the device AP? did Smart APSTA turn the AP off? use the device's STA-network IP |
| `wifi-dap.local` does not resolve | mDNS support on device/client; fall back to the IP (hostname is `wifi-dap` in `wt_mdns_config.h`) |
| Web UI opens but operations are rejected with HTTP 403 | Host/Origin guard: access via IP or add the hostname to `BIRDIEBOT_WEB_ALLOWED_ORIGIN` |
| `usbip.exe attach_ude` fails | usbip-win 0.3.0+? device IP/network reachable? port 3240 blocked by firewall? busid is always `1-1` |
| DAP disconnects/slow | Wi-Fi environment/distance, 2.4 GHz interference, too-high debug clock, broadcast storms on the LAN |
| UART page/bridge shows no data | baud rate matches the target? correct board TX/RX pins (see board-pins)? is the target actually transmitting? UART0 console is not bridged |
| OTA returns 404 | current firmware lacks the upload endpoint (old firmware / feature-trimmed build); rebuild and re-flash |
| OTA returns 409 | an upload session is already active (RECEIVING/VERIFYING/READY/APPLYING); wait for timeout or send ABORT |
| OTA returns 413 / 422 | file larger than the slot (3 MB), or a merge/zip image instead of the app bin; verify file and SHA-256 |
| Version rolled back after OTA | new image crashed/reset within 10 s → bootloader rollback; inspect reset reason/panic on serial, fix and re-upload |
| Device won't boot after upload | serial log + coredump: `idf.py coredump-info` (same IDF version as the build); if needed `esptool.py erase_flash` then re-flash per §4.5 |
| Version shows `v1.0.0`/empty | build environment has no `vX.Y.Z` tag and no `version.txt` (fallback is expected for dev builds), see [docs/versioning.md](docs/versioning.md) |

More OTA error codes and state mappings: [docs/hardware-test-ota.md §5/§6](docs/hardware-test-ota.md).

---

## 15. Documentation, feedback & license

- **Documentation index**: [docs/README.md](docs/README.md) (frozen contracts, test checklists, pending docs and maintenance rules).
- **Key contracts**: [protocol.md](docs/protocol.md) (JSON/WS protocol), [ota-local-upload.md](docs/ota-local-upload.md), [wifi-credential-memory.md](docs/wifi-credential-memory.md), [versioning.md](docs/versioning.md), [board-pins.md](docs/board-pins.md).
- **Security**: [docs/security.md](docs/security.md) (deployment hardening guide), [docs/security-review-web.md](docs/security-review-web.md) (Web/OTA/WiFi security review with P0/P1/P2 breakdown and evidence).
- **Feedback**: the Feedback page in the Web UI; author homepage <https://yunsi.studio/>; project homepage <https://yunsi.studio/wireless-debugger>.
- **License** (mixed licensing; full statement in [NOTICE](NOTICE), texts in [LICENSES/](LICENSES/)):
  - **Project-original parts**: **Apache License 2.0** (root [LICENSE](LICENSE) / [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); individual files may state another SPDX license in their header (e.g. some `scripts/` are SPDX MIT).
  - **Third-party components keep their upstream licenses**:
    - windowsair/wireless-esp8266-dap — **MIT**, covering `components/DAP`, `components/USBIP`, `components/elaphureLink`, `components/dap_proxy` (per-directory `LICENSE`);
    - kerms/wireless-esp32-tools — **Apache-2.0**, covering the ESP-IDF 5.2.1 platform components (SPDX headers);
    - kerms/wireless-esp32-tools-web-host — **MIT**, covering the `web/` frontend ([web/LICENCE.txt](web/LICENCE.txt));
    - ARM CMSIS-DAP — **Apache-2.0**, covering `components/DAP/cmsis-dap/**`.
  - MIT text: [LICENSES/MIT.txt](LICENSES/MIT.txt); copyright/attribution summary: [NOTICE](NOTICE); upstream differences: [§11](#11-origins--changes-from-upstream).
