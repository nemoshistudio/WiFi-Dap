# WiFi Dap

[English](README.md) | **中文**

基于 **ESP32-S3** 的无线 CMSIS-DAP 调试器，并内嵌一套完整的 Web 管理面板：一根 USB 线烧好固件后，即可通过 Wi-Fi 完成 SWD/JTAG 调试、串口转发、设备管理与浏览器本地 OTA。

项目由 [windowsair/wireless-esp8266-dap](https://github.com/windowsair/wireless-esp8266-dap)（MIT）与 [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools)（平台层）合并重构而来，升级到 **ESP-IDF 5.2.1**，并加入 Web UI 与 JSON/WebSocket 协议。完整文档见 **[docs/README.md](docs/README.md)**。

> ⚠️ **安全默认**：Web/JSON API、WebSocket-DAP、UART TCP 桥、OTA 上传默认**均无鉴权**，且 HTTP 为明文。请仅部署在**可信局域网**，并阅读 [§9 安全默认与加固](#9-安全默认与加固)。

---

## 目录

1. [项目简介](#1-项目简介)
2. [功能矩阵](#2-功能矩阵)
3. [硬件与引脚](#3-硬件与引脚)
4. [构建与烧录](#4-构建与烧录)
5. [快速开始](#5-快速开始)
6. [Web 管理面板](#6-web-管理面板)
7. [调试工具接入](#7-调试工具接入)
8. [UART 与数据通路](#8-uart-与数据通路)
9. [安全默认与加固](#9-安全默认与加固)
10. [版本与协议](#10-版本与协议)
11. [与上游的关系 / 变更说明](#11-与上游的关系--变更说明)
12. [仓库结构](#12-仓库结构)
13. [测试与验证](#13-测试与验证)
14. [故障排查](#14-故障排查)
15. [文档、反馈与许可证](#15-文档反馈与许可证)

---

## 1. 项目简介

- **定位**：把一块 ESP32-S3（8 MB Flash）变成无线 CMSIS-DAP 调试探针 + 自带 Web UI 的调试小盒子。
- **调试链路**：USBIP/WinUSB（CMSIS-DAP v2）、elaphureLink（Keil 免驱）、WebSocket-DAP（浏览器工具）三种传输，均走 **TCP 3240**。
- **管理链路**：内嵌单页 Web UI（Vue 3 + Vite 构建，gzip 后嵌入固件），通过 **HTTP 80** 提供 Wi-Fi 配置、UART 终端、Widget 面板、OTA 上传、版本信息与反馈页。
- **发布目标**：**ESP32-S3 8 MB**（默认板 `BOARD_ZXR_DAP_S3`，双 OTA 槽）。ESP32-C3 / ESP32（4 MB）仅做**编译验收**，不支持 OTA、未做硬件验证。
- **已移除**：ESP8266 支持、KCP 实验传输、corsacOTA（3241 端口）。

---

## 2. 功能矩阵

| 类别 | 功能 | 状态 | 说明 |
|---|---|---|---|
| 目标芯片 | ESP32-S3 | ✅ 硬件验证目标 | 8 MB Flash，默认板 `BOARD_ZXR_DAP_S3`，双 OTA 槽 |
| 目标芯片 | ESP32-C3 / ESP32 | 🧪 仅编译 | 4 MB 单 app 分区，无 OTA，未做硬件验证 |
| 调试协议 | SWD / JTAG | ✅ | CMSIS-DAP 实现，SWD/JTAG 引脚见 [docs/board-pins.md](docs/board-pins.md) |
| USB 传输 | USBIP / WinUSB | ✅ | CMSIS-DAP v2，TCP **3240**；Windows 推荐 usbip-win 0.3.0 `attach_ude` |
| 工具链 | OpenOCD / pyOCD / Keil | ✅ | 通过 CMSIS-DAP v2 使用（上游能力，DAP 核心未改协议） |
| 免驱传输 | elaphureLink / OpenOCD-elaphureLink | ✅ | Keil 插件或 OpenOCD 直连，TCP **3240**，无需 USBIP 驱动 |
| 浏览器传输 | WebSocket-DAP（legacy） | ✅（可关） | TCP **3240**，二进制帧 = CMSIS-DAP 命令包；`CONFIG_BIRDIEBOT_DAP_WS_ENABLE` |
| Web UI | 内嵌管理面板（zh/en） | ✅ | HTTP **80**，SharedWorker 单 WS 连接；Wi-Fi / UART / Widget / Update / About / Feedback |
| Wi-Fi | AP + STA（Smart APSTA） | ✅ | 默认 AP `WiFi-Dap-XXXX` / `12345678`；STA 连上 30 s 后关 AP、断开 10 s 后重开 |
| Wi-Fi | 多凭据记忆 | ✅ | 最多 8 个网络，自动记忆 + 上电/断线轮换（[契约](docs/wifi-credential-memory.md)） |
| 串口 | Web UART 终端 + WS 二进制 | ✅ | 默认 115200-8N1，可配数据位/校验/停止位/流控 |
| 串口 | UART TCP 桥 | ✅（可关） | TCP **1346**；首包文本可切换波特率；`CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE` |
| 发现 | mDNS / SSDP | ✅ | mDNS `_http._tcp`；SSDP 1900/UDP + `GET /SSDP.xml` |
| OTA | 浏览器本地上传（双槽 + 回滚） | ✅ 仅 S3 8 MB | `POST /api/ota/upload`，见 [契约](docs/ota-local-upload.md) |
| 诊断 | coredump 到 Flash | ✅ | 64 KB `coredump` 分区（`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`） |
| 安全 | 可选 Web 鉴权 | ✅ 默认关 | `BIRDIEBOT_WEB_AUTH`，见 [§9](#9-安全默认与加固) |
| 版本 | 固件 semver + 协议 v1 | ✅ | About 页 / `SYSTEM/GET_FM_INFO`，见 [docs/versioning.md](docs/versioning.md) |

---

## 3. 硬件与引脚

- 板级引脚与默认值是**冻结契约**：[docs/board-pins.md](docs/board-pins.md)（唯一权威来源）。
- 板卡选择：`idf.py menuconfig → WiFi Dap board configuration → Target board`；Kconfig 会按 `set-target` 自动给默认板。

| IDF target | 默认 board | 说明 |
|---|---|---|
| `esp32s3` | `BOARD_ZXR_DAP_S3` | 默认板，WiFi Dap 实机，8 MB |
| `esp32s3` | `BOARD_GENERIC_S3` | 通用 S3 接线，无状态灯，仅编译/参考 |
| `esp32c3` | `BOARD_GENERIC_C3` | 仅编译 |
| `esp32` | `BOARD_GENERIC_ESP32` | 仅编译 |

默认板 `BOARD_ZXR_DAP_S3` 关键引脚：

| 功能 | 引脚 | 备注 |
|---|---|---|
| SWCLK / TCK | GPIO12 | 勿改 |
| SWDIO / TMS | GPIO11 | 单线 SWDIO |
| TDO（目标→host） | GPIO15 | JTAG |
| TDI（host→目标） | GPIO16 | JTAG |
| nTRST / nRESET | GPIO14 / GPIO13 | nTRST 可选 |
| Connected / Running LED | GPIO5 / GPIO6 | 由 CMSIS-DAP HostStatus 控制 |
| Wi-Fi 状态 LED | GPIO4 | |
| UART 桥 TX / RX | GPIO10 / GPIO9 | UART1；控制台仍在 UART0（S3：GPIO43/44） |

> 通用 C3/ESP32 板的引脚、以及 LED 缺失板（宏为 `_`）的约定见 [docs/board-pins.md §4](docs/board-pins.md)，不要在其他代码里写死 `#ifdef CONFIG_IDF_TARGET_*` 引脚分支。

**电源**：目标板 3V3 / GND 需与 DAP 共地；调试目标建议由目标板自供电，DAP 的 GPIO 电压为 3.3 V。

---

## 4. 构建与烧录

### 4.1 环境准备

- **推荐**：Docker（`espressif/idf:v5.2.1` 镜像）+ `bash`、`flock`、`python3`、`gcc`。
- 也可直接安装 **ESP-IDF v5.2.1** 本地构建（`main/idf_component.yml` 要求 `idf >= 5.2.0`）。
- 仅当需要重新构建 Web UI 时才需要 Node.js（20+）与 npm；固件默认使用已嵌入的 `project_components/html/*.gz`。

```bash
docker pull espressif/idf:v5.2.1
```

### 4.2 一键构建（Docker）

```bash
# 三目标（S3 8MB + C3 4MB + ESP32 4MB；含分区门禁）
scripts/build_all.sh

# 只构建发布目标 / 干净重建
scripts/build_all.sh -t esp32s3
scripts/build_all.sh --clean -t esp32s3
```

- 每个目标独立目录与 SDKCONFIG：`build/<target>/`、`build/<target>/sdkconfig`，日志 `build/logs/<target>.log`。
- 构建后自动校验 app 大小/分区表，并解析 `esp_app_desc.version`（不能为空）。
- 更多参数与手工构建方式见 [scripts/README.md](scripts/README.md)。

### 4.3 手动构建（等价）

```bash
scripts/idf-docker.sh -B build/esp32s3 \
    -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 \
    -D SDKCONFIG=build/esp32s3/sdkconfig set-target esp32s3
scripts/idf-docker.sh -B build/esp32s3 -D SDKCONFIG=build/esp32s3/sdkconfig build
```

> 三个目标共用工程根 `sdkconfig` 会互相覆盖；务必按上面的方式把 SDKCONFIG 放到各自构建目录（`build_all.sh` 已处理）。

### 4.4 分区与镜像（关键）

| 目标 | 分区表 | 布局 | OTA |
|---|---|---|---|
| ESP32-S3（8 MB，发布） | [`partitions/partitions_8mb_ota.csv`](partitions/partitions_8mb_ota.csv) | bootloader + ptable@0x8000 + nvs 24K + phy 4K + otadata 8K + `ota_0` 3M@0x20000 + `ota_1` 3M@0x320000 + wt_nvs 64K + coredump 64K | ✅ 双槽 + 回滚 |
| ESP32-C3 / ESP32（4 MB） | [`partitions/partitions_4mb_single.csv`](partitions/partitions_4mb_single.csv) | nvs + phy + `factory` 3.75M + wt_nvs + coredump | ❌ 无 OTA |

约束细节（app 上限、otadata、回滚语义）见 [docs/ota-local-upload.md §1](docs/ota-local-upload.md)。

### 4.5 首次烧录（ESP32-S3 8 MB）

只写 **bootloader + 分区表 + ota_0**，`otadata` 保持空（0xFF），bootloader 会回退扫描启动 `ota_0`：

```bash
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  --device=/dev/ttyUSB0 -v "$PWD:/project" -w /project espressif/idf:v5.2.1 \
  bash -c 'esptool.py --chip esp32s3 -p /dev/ttyUSB0 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 build/esp32s3/bootloader/bootloader.bin \
    0x8000 build/esp32s3/partition_table/partition-table.bin \
    0x20000 build/esp32s3/birdiebot_wifi_dap.bin'
```

- ❌ **不要**写 `build/esp32s3/ota_data_initial.bin`，也**不要**用 `merge_bin` 全量包首烧（会初始化 otadata，掩盖首烧回退逻辑）。
- 串口控制台：UART0，115200（S3：GPIO43/44）。
- 之后升级请使用 Web UI 的 OTA 或 `scripts/ota_upload.py`，不要再整片重烧。

### 4.6 发布包（可选）

```bash
scripts/release_local.sh                 # 严格模式：要求干净工作区 + vX.Y.Z tag
scripts/release_local.sh --allow-dirty   # 本机验证用
```

产物在 `build/release/`：`birdiebot_wifi_dap.bin`（唯一可 OTA 上传的 app 镜像）、bootloader、分区表、`flash.sh`、Web gz 资源、`SHA256SUMS` 等，详见 [scripts/README.md](scripts/README.md)。

### 4.7 重新构建 Web UI（可选）

```bash
cd web && ./build.sh             # 输出 web/dist 与 web/dist-gz
cd .. && scripts/gen_web_assets.sh --install   # 显式写入 project_components/html/
```

> `gen_web_assets.sh` 默认只写到 `build/web-assets/` 暂存区，`--install` 才会更新固件内嵌资源；`--check` 可校验已入库资源是否与当前 Web UI 一致。

---

## 5. 快速开始

1. 按 [§4.5](#45-首次烧录esp32-s3-8-mb) 首次烧录，串口（UART0 115200）确认启动日志无 panic。
2. 电脑/手机连接设备热点：
   - SSID：`WiFi-Dap-XXXX`（`XXXX` = STA MAC 末 2 字节大写 hex，例如 `WiFi-Dap-A1B2`）
   - 默认口令：`12345678`（**首次使用请立即在 Wi-Fi 页修改**，见 [§9](#9-安全默认与加固)）
3. 浏览器打开 **<http://192.168.1.1/>**（AP 模式默认网关/设备地址）。
   - 若已通过 Wi-Fi 页把设备接入局域网，则改用路由器分配给它的 IP；
   - mDNS：设备注册 `_http._tcp` 服务，主机名默认 `dap`（见 `project_components/wt_mdns/`，板级契约目标为 `wifi-dap`），可尝试 `http://dap.local/`；以实际网络扫描结果为准。
4. 在 **Wi-Fi** 页填写并连接你的局域网（支持多网络记忆与自动切换），之后即可在局域网内访问 Web UI、调试器端口与 UART 桥。
5. 打开 **UART** 页，选择波特率（默认 115200-8N1）即可看目标串口输出；或直接使用 USBIP/elaphureLink 开始调试（[§7](#7-调试工具接入)）。

> 设备默认工作在 Smart APSTA：STA 未连接时 AP 保持开启；STA 连上 30 s 后自动关闭 AP，STA 断开 10 s 后重新开启 AP（延迟可在 Wi-Fi 页/JSON API 调整）。

---

## 6. Web 管理面板

入口：`http://<设备IP>/`（默认 `/#/uart`）。前端与固件通过单条 WebSocket（`ws://<ip>/ws`）通信，文本帧为 JSON，二进制帧为 4 字节头 + payload；固件每 **1500 ms** 发送空 TEXT 帧心跳。协议细节见 [docs/protocol.md](docs/protocol.md) 与 [docs/ws-binary-router.md](docs/ws-binary-router.md)。

| 页面 | 路径 | 功能 |
|---|---|---|
| **UART** | `/#/uart` | 串口终端（TX/RX）、波特率/数据位/校验/停止位/流控配置；数据走 WS 二进制 `RAW`/`RAW_BROADCAST` 帧 |
| **Wi-Fi** | `/#/wifi` | STA 扫描/连接、已保存网络（最多 8 个，可增删/自动保存）、AP 名称与口令、静态 IP/DNS、AP/STA 模式与 Smart APSTA 延迟 |
| **Widget** | `/#/widget` | 可拖拽/缩放的网格仪表盘，内置 UART/AT 命令等小组件；布局保存在浏览器本地 |
| **Update** | `/#/update` | 浏览器本地 OTA：选择 app `.bin` 上传、实时进度、校验与回滚（仅 S3 8 MB；4 MB 目标显示不支持） |
| **About** | `/#/about` | 固件版本 / 协议版本 / Web UI 版本 / 硬件版本 / SN / IDF 版本等信息 |
| **Feedback** | `/#/feedback` | 反馈入口（作者主页/项目主页链接） |

- 语言：中文 / English 可切换（i18n 314 个 key，见 [docs/i18n-keys.md](docs/i18n-keys.md)）。
- 鉴权：启用 `BIRDIEBOT_WEB_AUTH` 后，首次打开会弹出配对码输入框（见 [§9](#9-安全默认与加固)）。
- 图片预览：[Widget 面板](web/assets/widgetPannel.png) · [UART 终端](web/assets/uart.png)

### OTA（仅 ESP32-S3 8 MB）

1. `Update` 页选择 `build/esp32s3/birdiebot_wifi_dap.bin`（**只上传 app 镜像**，不要上传 merge/全量包/zip）。
2. 上传过程中设备把镜像写入**空闲槽**（`ota_0` ↔ `ota_1`），状态：`IDLE → RECEIVING → VERIFYING → READY → APPLYING`，成功后约 1 s 重启。
3. 新镜像首启为 `PENDING_VERIFY`，`main` 在 10 s 后确认；若 10 s 内崩溃/复位，bootloader 自动回滚旧槽。
4. 若当前 Web 构建未启用 Update 页（构建开关 `VITE_ENABLE_OTA`），可用等价的 HTTP/脚本方式上传：
   - `python3 scripts/ota_upload.py --host <ip> --file <app.bin> --info`
   - 或 `curl` 直传，见 [docs/hardware-test-ota.md §2/§3](docs/hardware-test-ota.md)。

---

## 7. 调试工具接入

所有 DAP 传输共用 **TCP 3240**（USBIP / elaphureLink / WebSocket-DAP 由首包自动识别）。

### 7.1 USBIP / WinUSB（Windows 推荐）

1. 安装 [usbip-win 0.3.0（kmdf/ude）](https://github.com/cezanne/usbip-win)。
2. 确认 PC 与设备同网（AP 模式直连，或设备已接入同一局域网）：

```bash
# WinUSB 模式（推荐）
usbip.exe attach_ude -r <设备IP> -b 1-1

# 旧版 usbip 用法（HID 模式构建；本固件默认 WinUSB，HID 需改 USE_WINUSB=0 重新构建）
usbip.exe -D -a <设备IP> 1-1
```

Windows 通过 WCID/MS OS 2.0 描述符自动绑定 WinUSB；设备在软件中显示为 CMSIS-DAP 调试器（VID:PID = `C251:F00A`）。Linux 内核自带 usbip 客户端，但本项目未纳入 Linux 验证范围。

### 7.2 elaphureLink（Keil 免驱）

- Keil 安装 [elaphureLink](https://github.com/windowsair/elaphureLink) 插件，连接 `TCP:3240` 即可，无需 USBIP 驱动。
- OpenOCD 用户可使用 [OpenOCD-elaphureLink](https://github.com/windowsair/openocd-elaphurelink)。

### 7.3 WebSocket-DAP（legacy，浏览器工具）

- 同样在 3240 端口：先以 HTTP `GET ` 完成 WebSocket 升级，之后的**二进制帧 payload 即 CMSIS-DAP 命令包**（见 [components/dap_proxy/](components/dap_proxy/)）。
- 可通过 `CONFIG_BIRDIEBOT_DAP_WS_ENABLE=n` 关闭（USBIP/elaphureLink 不受影响，端口仍监听）。
- 适合 [elaphure-dap.js](https://github.com/windowsair/elaphure-dap.js) 等浏览器端工具。

### 7.4 OpenOCD / pyOCD / Keil

设备对主机表现为标准 **CMSIS-DAP v2** 探针（SWD/JTAG），OpenOCD、pyOCD、Keil 等按 CMSIS-DAP 选型即可。DAP 默认 SWD 时钟为 **1 MHz**（`BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ`，见 [docs/board-pins.md §5](docs/board-pins.md)），高时钟由调试器侧选择。

---

## 8. UART 与数据通路

- **Web UART**：UART 页直接收发（WS 二进制帧），默认 115200-8N1。
- **UART TCP 桥**：裸 TCP 服务器，**端口 1346**，`数据 → TCP → 目标 UART TX`、`目标 UART RX → TCP → 数据`。
  - 新建连接后的**首个文本包**若为合法波特率（2–7 位数字，如 `115200`），桥会切换到该波特率；否则按普通数据透传。
  - 默认波特率 115200（旧版 1234/74880 已废弃）；可通过 `CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n` 关闭。
  - 例：`nc <设备IP> 1346`。
- **控制台 UART0**（S3：GPIO43/44，115200）只用于固件日志，不被 UART 桥/Web UART 占用。
- Web UART 与 TCP 桥共用同一 UART 服务，可同时连接；多客户端广播语义见 [docs/hardware-test-uart-dataflow.md](docs/hardware-test-uart-dataflow.md)。

---

## 9. 安全默认与加固

### 9.1 默认状态（实验室模式）

| 面 | 默认 | 风险 |
|---|---|---|
| Web UI / JSON API / WS 数据面 | 无鉴权 | 同网段任意主机可读配置、控制 UART、发起 OTA |
| OTA 上传（`POST /api/ota/upload`） | 无鉴权 | 可写入任意固件（无签名校验） |
| USBIP / elaphureLink / WS-DAP（3240） | 无鉴权 | 可 attach 探针并读写目标芯片 |
| UART TCP 桥（1346） | 无鉴权 | 可直连目标串口 |
| 传输加密 | 无（HTTP/WS/TCP 均明文） | 可被同网段嗅探/篡改 |
| Wi-Fi 凭据存储 | NVS 明文（无 flash encryption） | 物理读取可获取；`GET_AP_INFO`/`GET_STA_INFO` 会回传口令 |
| AP 口令 | `12345678` | 请首次使用即修改 |

> 建议：仅在**可信局域网/隔离 VLAN** 中使用；不要把调试口暴露到办公网或互联网。

### 9.2 启用 Web 鉴权

`idf.py menuconfig → WiFi Dap web server → Enable Web UI/API token authentication (`BIRDIEBOT_WEB_AUTH`)`：

- **配对码（pair code）**：默认每次启动随机生成 8 位 hex 并持久化到 `wt_nvs`，启动日志会打印
  `Web auth ENABLED; pairing token: <code>`；可用 `BIRDIEBOT_WEB_TOKEN` 固定（URL-safe，≤256 字符）。
- **客户端携带方式**：
  - WebSocket：`ws://<ip>/ws?token=<code>`，或首帧 `{"module":0,"cmd":0,"token":"<code>"}`；
  - HTTP：`Authorization: Bearer <code>` 或 `?token=<code>`；未通过返回 HTTP 401 + `code=7`。
- **Host/Origin 校验**：默认只接受 IP 直连；若通过主机名（mDNS/反向代理）访问，需在 `BIRDIEBOT_WEB_ALLOWED_ORIGIN` 加入该来源（如 `http://wifi-dap.local`）。
- 脚本支持：`scripts/ota_upload.py --token <code>`、`scripts/e2e_ws_client.py --token <code>`。

### 9.3 关闭不需要的传输

```text
idf.py menuconfig
  → WiFi Dap proxy (USBIP / WebSocket-DAP) → BIRDIEBOT_DAP_WS_ENABLE = n
  → WiFi Dap UART TCP bridge                  → BIRDIEBOT_UART_BRIDGE_ENABLE = n
```

USBIP 与 elaphureLink 没有单独开关（3240 端口保持监听）；如不需要调试功能，请在网络层限制 3240/1346 端口访问。

### 9.4 其它建议

- 首次使用后修改 AP 口令（Wi-Fi 页 → AP 设置）。
- 只在可信网络配置 STA；避免把设备接入公共热点。
- 发布固件时开启 Secure Boot/Flash Encryption（本项目默认未启用）；启用后 `esp_ota_end()` 会校验镜像签名。
- coredump 可能包含内存中的敏感数据（含 Wi-Fi 口令），转储文件请按敏感信息处理。

---

## 10. 版本与协议

| 版本 | 来源 | 运行时可见 |
|---|---|---|
| 固件版本 `vMAJOR.MINOR.PATCH` | 本地 git tag（`git describe`，回退 `version.txt`/`v1.0.0`） | 启动日志、About 页、`SYSTEM/GET_FM_INFO.fm_ver`、`OTA/GET_INFO.running_ver` |
| 协议版本 `protocol_version = 1` | `project_components/wt_common/wt_protocol_version.h`（唯一来源） | 启动日志、`GET_FM_INFO.protocol_version`、About 页 |
| Web UI 版本 `1.0.0` | `web/package.json`，构建时注入 | About 页、`build-info.json`、`web_version.txt` |

- 连接后前端发送 `SYSTEM/GET_FM_INFO` 做版本握手；协议不匹配时提示“固件与网页版本不匹配”，并禁用危险操作（如 OTA）。
- 发布一致性由 `scripts/check-release-versions.sh` 等门禁脚本校验，详见 [docs/versioning.md](docs/versioning.md) 与 [scripts/README.md](scripts/README.md)。

---

## 11. 与上游的关系 / 变更说明

三方来源（细节与逐文件映射见 [docs/merge-map.md](docs/merge-map.md)）：

| 代号 | 仓库 | 许可 | 贡献 |
|---|---|---|---|
| **A** | [nemoshistudio/wireless-esp8266-dap](https://github.com/nemoshistudio/wireless-esp8266-dap)（windowsair 上游 fork） | MIT | CMSIS-DAP 核心、USBIP/elaphureLink 代理、LED/S3 UART 等 fork 改动 |
| **C** | [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) | Apache-2.0 | ESP-IDF 5.2.1 平台层：wifi_manager / web_server / wt_nvs / memory_pool / SSDP / UART service / DataFlow |
| **D** | [kerms/wireless-esp32-tools-web-host](https://github.com/kerms/wireless-esp32-tools-web-host) | MIT | Vue 3 + Vite Web UI（本项目在其基础上扩展 OTA/WiFi 管理/鉴权/i18n） |

本仓库（WiFi Dap）的主要变更：

- 升级到 **ESP-IDF 5.2.1**，按组件拆分（`components/` + `project_components/`），板级引脚抽象为 `components/board`（`BOARD_*` + Kconfig，去掉按 target 写死的引脚分支）。
- 新增 **Web UI + JSON/WS 协议 v1**（[docs/protocol.md](docs/protocol.md)）、Wi-Fi 多凭据记忆、浏览器本地 OTA（8 MB 双槽 + 回滚）、可选 Web 鉴权、SSDP/mDNS、coredump、UART service/DataFlow/Widget 数据面。
- 保留并修复上游 DAP 代理层（USBIP / elaphureLink / legacy WebSocket-DAP）：DAP ring buffer 生命周期归 DAP 任务、socket fd 泄漏修复、错误路径处理等。
- 默认值调整：SWD 默认时钟 1 MHz、UART 桥 1346/115200、AP SSID `WiFi-Dap-XXXX`、移除 ESP8266/KCP/corsacOTA(3241)。
- 默认板 `BOARD_ZXR_DAP_S3`；C3/ESP32 仅编译验收。

---

## 12. 仓库结构

```text
.
├── main/                  # app_main、DAP 配置入口
├── components/            # 固件组件（DAP、USBIP、dap_proxy、board、wifi 底层、SSDP、wt_ota…）
├── project_components/    # IDF5 工程组件（wifi_manager、web_server、api_router、html、wt_mdns…）
├── web/                   # Vue 3 Web UI 源码与构建脚本（产物 gzip 后嵌入固件）
├── partitions/            # 8MB 双 OTA / 4MB 单 app 分区表
├── scripts/               # Docker 构建、分区/板级校验、Web 资产、发布、OTA/E2E 测试脚本
├── docs/                  # 契约与测试文档（索引：docs/README.md）
├── sdkconfig.defaults.*   # 三目标默认配置
└── CMakeLists.txt         # 版本注入 + 工程定义
```

构建产物目录 `build/`、`build-*/` 均被 gitignore；`project_components/html/*.gz` 是入库的 Web 资产。

---

## 13. 测试与验证

### 13.1 本地校验（无需硬件）

```bash
scripts/build_all.sh                         # 三目标构建 + 分区/app 门禁
python3 scripts/check_partitions.py          # 分区表契约 + app 大小 + 内嵌版本
python3 scripts/check_board_pins.py          # 板文件 vs docs/board-pins.md 契约
scripts/gen_web_assets.sh --check            # 入库 Web gz 是否与 web/ 构建一致
```

### 13.2 WebSocket / UART / OTA 自动化（需设备）

```bash
# 连接 + 版本握手
python3 scripts/e2e_ws_client.py --host <ip> ping
# Wi-Fi 扫描 / UART 回环 / DataFlow / OTA 信息 / 任意 JSON
python3 scripts/e2e_ws_client.py --host <ip> wifi-scan
python3 scripts/e2e_ws_client.py --host <ip> uart-echo --payload 48656c6c6f --hex
python3 scripts/e2e_ws_client.py --host <ip> dataflow-list
python3 scripts/e2e_ws_client.py --host <ip> ota-info
# OTA 上传（也可 --info 轮询新版本）
python3 scripts/ota_upload.py --host <ip> --file build/esp32s3/birdiebot_wifi_dap.bin --info
```

### 13.3 硬件验收清单

- **OTA**（8 MB S3）：[docs/hardware-test-ota.md](docs/hardware-test-ota.md) —— 首烧、Web/HTTP 上传、错误注入、回滚验证与记录模板。
- **UART/DataFlow/WS 二进制**：[docs/hardware-test-uart-dataflow.md](docs/hardware-test-uart-dataflow.md) —— T1–T8 场景（连通/配置/回环/双客户端广播等）。
- **DAP/USBIP 真机验证**：清单由 dap-dev 产出中，入库后补入 [docs/README.md](docs/README.md)。

---

## 14. 故障排查

| 现象 | 优先检查 |
|---|---|
| 找不到 `WiFi-Dap-XXXX` 热点 | 是否已首次烧录；板卡 board 选择是否正确（`sdkconfig.defaults.*`）；串口日志是否有 Wi-Fi 初始化错误；STA 已连接时 AP 会在 30 s 后关闭（正常） |
| 打开 `192.168.1.1` 无响应 | 手机/PC 是否连的是设备 AP；是否被 Smart APSTA 关闭了 AP；改连 STA 网段的设备 IP |
| `dap.local` 无法解析 | 设备/系统 mDNS 支持；改用 IP 访问（mDNS 主机名以实现/启动日志为准） |
| Web UI 提示在线但操作 401/断开 | 是否开启了 `BIRDIEBOT_WEB_AUTH`：输入串口打印的配对码；主机名访问需配置 `BIRDIEBOT_WEB_ALLOWED_ORIGIN` |
| `usbip.exe attach_ude` 失败 | usbip-win 版本是否 0.3.0+；设备 IP/网段是否可达；3240 是否被防火墙拦截；busid 固定为 `1-1` |
| DAP 断连/很慢 | Wi-Fi 环境与距离；2.4 GHz 干扰；调试器时钟过高；同网段广播风暴 |
| UART 页/桥无数据 | UART 页波特率与目标一致？引脚是否为板级 TX/RX（见 board-pins）？目标是否真的在输出？控制台 UART0 不经过桥 |
| OTA 返回 404 | 当前固件是否包含 OTA 端点（旧固件/构建裁剪）；重新构建烧录 |
| OTA 返回 409 | 已有上传会话（RECEIVING/VERIFYING/READY/APPLYING）；等待超时或发送 ABORT |
| OTA 返回 413 / 422 | 文件超过槽大小（3 MB）；或上传的是 merge/zip 而非 app bin；检查文件与 SHA-256 |
| OTA 后版本回退 | 新镜像 10 s 内崩溃/复位 → bootloader 回滚；看串口 reset reason/panic，修复后重传 |
| 上传后无法启动 | 串口日志 + coredump：`idf.py coredump-info`（需与构建相同版本 IDF）；必要时 `esptool.py erase_flash` 后按 §4.5 重烧 |
| 版本显示 `v1.0.0`/空 | 构建环境无 `vX.Y.Z` tag 且无 `version.txt`（开发构建回退属预期），见 [docs/versioning.md](docs/versioning.md) |

更多 OTA 错误码与状态映射见 [docs/hardware-test-ota.md §5/§6](docs/hardware-test-ota.md)。

---

## 15. 文档、反馈与许可证

- **文档索引**：[docs/README.md](docs/README.md)（冻结契约、测试清单、待产出文档与维护规则）。
- **关键契约**：[protocol.md](docs/protocol.md)（JSON/WS 协议）、[ota-local-upload.md](docs/ota-local-upload.md)、[wifi-credential-memory.md](docs/wifi-credential-memory.md)、[versioning.md](docs/versioning.md)、[board-pins.md](docs/board-pins.md)。
- **反馈**：Web UI 的 Feedback 页；作者主页 <https://yunsi.studio/>；项目主页 <https://yunsi.studio/wireless-debugger>。
- **许可证**：
  - 本仓库整体以 **Apache License 2.0** 发布（根目录 [LICENSE](LICENSE)）。
  - `components/DAP`、`components/USBIP`、`components/elaphureLink`、`components/dap_proxy` 保留上游 windowsair 的 **MIT** 许可（各目录内 `LICENSE`）。
  - `web/` 前端来自 kerms 的 **MIT** 许可项目（[web/LICENCE.txt](web/LICENCE.txt)）。
  - 各上游版权与出处见 [§11](#11-与上游的关系--变更说明) 与 [docs/merge-map.md](docs/merge-map.md)。
