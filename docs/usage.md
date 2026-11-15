# WiFi Dap usage

All examples use the USB-NCM address `192.168.7.1`; replace it with the
device's Wi-Fi IP when connecting over the LAN.

## 1. Connect

- **USB**: the probe exposes a USB-NCM network interface. The host gets
  `192.168.7.2/24`; the probe is `192.168.7.1`.
- **Wi-Fi AP**: join `WiFi-Dap-XXXX` (default password `12345678`) and open
  `http://192.168.1.1/`.
- **Wi-Fi STA**: on the Web **Wi-Fi** page, join the local network and note the
  assigned IP shown there.

Check the device:

```bash
curl -s -X POST http://192.168.7.1/api \
  -H 'Content-Type: application/json' -d '{"module":0,"cmd":1}'
```

## 2. Web UI

Open `http://<device-ip>/`:

| Page | Use |
|---|---|
| **Target** | Detect the target, choose the SWD clock, flash BIN/ELF/HEX |
| **Debug curve** | Local ESP32-side sampling: select ELF variables, start/stop, inspect curves |
| **UART** | Interactive UART terminal and byte stream |
| **Wi-Fi** | AP/STA setup, saved networks, static IP |
| **Status** | Link health, SWD clock, heap, DAP session |
| **About** | Firmware/Web/protocol versions |

### Target flashing

1. Wire SWCLK/SWDIO/nRESET/GND to the target (pins in
   [board-pins.md](board-pins.md)).
2. Open **Target**, detect the chip, select BIN/ELF/HEX and flash.
3. Keep the target powered and the SWD wires short at high clocks.

### Debug curve

1. Load the target ELF in the Web UI (function/variable names come from it).
2. Select variables, choose `periodic` or `breakpoint` sampling.
3. Start; data is sampled by the ESP32 locally and streamed to the browser.

## 3. USBIP + OpenOCD (recommended for OpenOCD/pyOCD/Keil)

The probe runs a USBIP server on TCP **3240**. Attach the virtual CMSIS-DAP
device on the host:

```bash
sudo usbip attach -r 192.168.7.1 -b 1-1
lsusb   # shows the CMSIS-DAP device after attach
```

Then use the standard CMSIS-DAP interface:

```bash
openocd -f interface/cmsis-dap.cfg \
        -f target/stm32h7x.cfg \
        -c "adapter speed 16000"
```

- pyOCD: `pyocd list`, select the CMSIS-DAP probe.
- Keil: select CMSIS-DAP; the target DLL is the standard ARM CMSIS-DAP driver.
- On Windows use a USBIP client for the same remote device, or use
  elaphureLink for a driver-free Keil connection.
- The default SWD clock is 2 MHz. For H723/long wires use `adapter speed 16000`;
  `20000` also works, `24000` needs the 240 MHz CPU variant and short wires.

Detach when done:

```bash
sudo usbip detach -p 0
```

## 4. OpenOCD over Wi-Fi (TCP backend)

Build the TCP variant (add `sdkconfig.defaults.esp32s3.dap_tcp` to
`SDKCONFIG_DEFAULTS`),
then connect directly without USBIP:

```bash
openocd \
  -c "adapter driver cmsis-dap" \
  -c "cmsis-dap backend tcp" \
  -c "cmsis-dap tcp host 192.168.7.1" \
  -c "cmsis-dap tcp port 3241" \
  -c "transport select swd" \
  -f target/stm32h7x.cfg \
  -c "adapter speed 16000"
```

One OpenOCD client is supported at a time. This path is verified with an H723
target and `stm32h7x.cfg`.

## 5. UART

- Web **UART** page for text/hex, timestamps and line triggers.
- Raw TCP bridge on port **1346**:

```bash
nc 192.168.7.1 1346
```

UART pins on the default S3 board: TX GPIO10, RX GPIO9, 115200-8N1.

## 6. Firmware update (OTA)

OTA is optional and needs the OTA build overlay
(`sdkconfig.defaults.esp32s3.ota`). With that firmware:

```bash
python3 scripts/ota_upload.py --host 192.168.7.1 \
  --file build/esp32s3-240-dap-tcp-ota/birdiebot_wifi_dap.bin --info
```

The device writes the idle slot, reboots and keeps rollback enabled. Without
the OTA overlay, flash over USB (BOOT + RESET for ROM download mode).

## 7. Troubleshooting

- **No target detected**: check 3V3/GND and SWD wiring; try `adapter speed 1000`.
- **USBIP attach fails**: check port 3240 and the device's `1-1` bus id;
  detach stale devices with `sudo usbip detach -p 0`.
- **OpenOCD TCP cannot connect**: rebuild with `sdkconfig.defaults.esp32s3.dap_tcp`;
  port 3241 is closed by default.
- **OTA 404**: the firmware was built without the OTA overlay.
- **Web UI empty/cached**: hard-refresh; firmware and Web UI versions are
  checked on connect.
