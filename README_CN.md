# WiFi Dap

WiFi Dap 是一款基于 **ESP32-S3** 的无线 **CMSIS-DAP v2** 调试器，带浏览器管理/调试界面。它通过 Wi-Fi 或 USB-NCM 提供 SWD/JTAG，并可直接烧录目标芯片。

**芯片支持：仅 ESP32-S3 是支持并验证过的目标。** 源码里仍保留 ESP32-C3、ESP32、ESP32-C5 的构建目标，但它们**未测试、不受支持**，不要用于生产环境。

## 功能

- Wi-Fi / USB-NCM 上的 CMSIS-DAP v2（SWD/JTAG）
- ESP32-S3 专用 GPIO SWD 引擎：160 MHz CPU 下 100 kHz–20 MHz，240 MHz 变体最高 24 MHz
- Web UI：设备状态、Wi-Fi 设置、目标检测/烧录、变量曲线、UART 终端
- 目标烧录、SWD 时钟选择、曲线采样、UART 桥
- 可选 OTA 固件更新（`POST /api/ota/upload`）
- mDNS 发现、每台设备唯一序列号和 USB 身份

## 硬件（ESP32-S3）

默认板卡：**WiFi Dap S3**，ESP32-S3（N16R8 级别，8 MB 分区布局）。

| 功能 | GPIO |
|---|---|
| SWCLK / TCK | GPIO12 |
| SWDIO / TMS | GPIO11 |
| nRESET | GPIO13 |
| TDO（JTAG，可选） | GPIO15 |
| TDI（JTAG，可选） | GPIO16 |
| nTRST（可选） | GPIO14 |
| UART 桥 TX / RX | GPIO10 / GPIO9 |
| 状态 LED | GPIO5 / GPIO6 / GPIO4 |

完整引脚表见 [docs/board-pins.md](docs/board-pins.md)。

## 构建（ESP32-S3）

依赖：Docker（推荐）或 ESP-IDF 5.5.6；只有重新构建 Web UI 时才需要 Node.js 20+。

```bash
# 默认 160 MHz 构建（含 Web UI 和全部 S3 功能）
scripts/build_all.sh -t esp32s3 -j 8

# 可选 240 MHz 变体（启用 24 MHz SWD 档）
scripts/idf-docker.sh -B build/esp32s3-240 \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3.240mhz" \
  -D "SDKCONFIG=build/esp32s3-240/sdkconfig" set-target esp32s3
scripts/idf-docker.sh -B build/esp32s3-240 \
  -D "SDKCONFIG=build/esp32s3-240/sdkconfig" build

# 可选 OTA 变体（模块 5 + POST /api/ota/upload）
# 在 SDKCONFIG_DEFAULTS 中追加 sdkconfig.defaults.esp32s3.ota
```

240 MHz 变体镜像位于 `build/esp32s3-240/birdiebot_wifi_dap.bin`。

## 首次烧录

进入 ESP32-S3 ROM 下载模式（按住 **BOOT**，按一下 **RESET**，松开 BOOT），然后：

```bash
docker run --rm --device=/dev/ttyACM0 -v "$PWD:/project" -w /project \
  espressif/idf:v5.5.6 bash -lc \
  'idf.py -B build/esp32s3 -p /dev/ttyACM0 flash'
```

首次 USB 烧录后，如果启用了 OTA overlay，后续可用内置 OTA：

```bash
python3 scripts/ota_upload.py --host 192.168.7.1 \
  --file build/esp32s3/birdiebot_wifi_dap.bin --info
```

## Web UI

1. 上电后连接设备 AP `WiFi-Dap-XXXX`（默认口令 `12345678`）。
2. 打开 `http://192.168.1.1/`。
3. 在 **Wi-Fi** 页修改 AP 口令，按需接入自己的网络。

## 调试

- **OpenOCD/pyOCD/Keil**：标准 CMSIS-DAP 探针；默认 SWD 时钟 2 MHz。
- **OpenOCD over Wi-Fi**（构建 TCP 变体时）：`cmsis-dap backend tcp`，端口 **3241**。
- **USBIP/WinUSB**、elaphureLink：使用标准 CMSIS-DAP 工具，在调试器里选择该探针。
- 长线材建议 OpenOCD `adapter speed 16000`。

## 文档

入口：[docs/README.md](docs/README.md)

- [协议](docs/protocol.md)
- [引脚](docs/board-pins.md)
- [本地曲线采样](docs/curve-local-sampler.md)
- [版本](docs/versioning.md)

## 许可证与署名

本项目原创代码为 **Apache-2.0**（见 [LICENSE](LICENSE)）；部分脚本为 MIT。上游项目保留各自许可证，署名见 [NOTICE](NOTICE)：

- [windowsair/wireless-esp8266-dap](https://github.com/windowsair/wireless-esp8266-dap) — MIT
- [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) — Apache-2.0
- [kerms/wireless-esp32-tools-web-host](https://github.com/kerms/wireless-esp32-tools-web-host) — MIT
- ARM CMSIS-DAP — Apache-2.0

WiFi Dap 由本项目贡献者维护，见 [CONTRIBUTORS.md](CONTRIBUTORS.md)；开发协助：[DeepSeek](https://github.com/deepseek-ai)。上述上游作者是依赖并被署名，不是本仓库作者。
