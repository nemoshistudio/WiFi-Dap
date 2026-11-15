# WiFi Dap

[English](README.md) | **中文**

基于 **ESP32-S3** 的无线 CMSIS-DAP 调试器，并内嵌一套 Web 管理面板：一根 USB 线烧好固件后，即可通过 Wi-Fi 完成 SWD/JTAG 调试、串口转发、设备状态查看与网页端目标烧录。**final slim 构建已无 Web OTA**——固件更新走 USB-JTAG / OpenOCD / UART0。

项目由 [windowsair/wireless-esp8266-dap](https://github.com/windowsair/wireless-esp8266-dap)（MIT）与 [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools)（Apache-2.0 平台层）合并重构而来，升级到 **ESP-IDF 5.5.6**，并加入 Web UI 与 JSON/WebSocket 协议。完整文档见 **[docs/README.md](docs/README.md)**。

> ✅ **final slim（当前树，2026-10-26）**：本树即已发布的**最终精简**固件/Web UI。
> 相对完整功能树（tag `backup/full-features-2026-10-26`）：浏览器本地 OTA 上传与 OTA
> JSON module 5 **已移除**（A1/A2；OTA 分区与 10 s 回滚确认保留）、SSDP/UPnP
> **已移除**（H1）、`GET /api/discover` + Web 设备发现卡 **已移除**（H3/B2）、
> 首次使用引导（B3）、在线手册入口（B5）、Ozone 兼容工具（J1）**已移除**；
> 浏览器 Host/Origin 校验**默认关闭**（M1，用户已接受实验室局域网风险，见 [§9](#9-安全默认与加固)）；
> 曲线系列上限 **256 → 64**（C4）。**新增**：独立 `/#/status` 设备状态页 + 全站
> compact 状态条（见 [docs/web-status-ui.md](docs/web-status-ui.md)）。
> USBIP/DAP、目标烧录、变量曲线、UART、WiFi、mDNS（TXT `name`/`serial`/`chip`/`fw`）
> 与 About 保留。逐项 Kconfig 开关/恢复方式/资源收益/归档路径见
> **[docs/feature-flags.md](docs/feature-flags.md)**；已移除功能的历史文档见
> **docs/archive/removed-features/**。

> ⚠️ **安全默认**：Web/JSON API、WebSocket-DAP、UART TCP 桥默认**均无鉴权**，HTTP 明文，且 slim 构建的浏览器 Host/Origin 校验**默认关闭**。请仅部署在**可信局域网**，并阅读 [§9 安全默认与加固](#9-安全默认与加固)。

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
- **管理链路**：内嵌单页 Web UI（Vue 3 + Vite 构建，gzip 后嵌入固件），通过 **HTTP 80** 提供设备状态、Wi-Fi 配置、UART 终端、目标检测/烧录、变量曲线与版本信息。**final slim 构建无 Web OTA / Update 页**，固件更新走 USB-JTAG / OpenOCD / UART0 ROM 下载（见 [§4.5](#45-首次烧录esp32-s3-8-mb) 与 [§6](#6-web-管理面板)）。
- **发布目标**：**ESP32-S3 8 MB**（默认板 `BOARD_WIFI_DAP_S3`，双槽分区布局）。ESP32-C3 / ESP32（4 MB）仅做**编译验收**（单 app 分区、未做硬件验证）。
- **ESP32-C5 变体**：IDF **v5.5.6**、**真机验证（非发布目标）、Wi-Fi-only**（`BOARD_GENERIC_C5`，8 MB 双槽分区布局）。C5 **无 USB-OTG**，物理上不可能做 USB-NCM/有线 DAP；保留 Wi-Fi USBIP + Web/曲线/UART，通过内置 USB Serial/JTAG + OpenOCD 刷写/调试；UART1 桥引脚为 **TX=GPIO25 / RX=GPIO26**（115200 回环实测通过）。见 docs/esp32-c5-port.md。
- **final slim（2026-10-26）移除**：Web OTA/Update 页与端点、设备发现卡 + `GET /api/discover`、SSDP/UPnP、首次使用引导、在线手册入口、Ozone 兼容 PoC。分区表、回滚确认与 OTA 组件源码保留；恢复方式：tag `backup/full-features-2026-10-26` + [docs/feature-flags.md](docs/feature-flags.md)。更早移除：ESP8266 支持、KCP 实验传输、corsacOTA（3241）、Widget 仪表盘。

---

## 2. 功能矩阵

| 类别 | 功能 | 状态 | 说明 |
|---|---|---|---|
| 目标芯片 | ESP32-S3 | ✅ 硬件验证目标 | 8 MB Flash，默认板 `BOARD_WIFI_DAP_S3`，双槽分区布局（slim 不暴露 Web OTA） |
| 目标芯片 | ESP32-C3 / ESP32 | 🧪 仅编译 | 4 MB 单 app 分区，无 OTA 分区，未做硬件验证 |
| 目标芯片 | ESP32-C5 | ✅ 真机验证（非发布目标）、Wi-Fi-only | ESP-IDF **v5.5.6**，8 MB 双槽布局，**无 USB-OTG/NCM**；Wi-Fi USBIP + Web/曲线/UART；UART1 桥 GPIO25/26；slim 无 Web OTA；经内置 USB Serial/JTAG + OpenOCD 刷写；实验室 bring-up + final slim HIL 有记录（非发布目标） |
| 调试协议 | SWD / JTAG | ✅ | CMSIS-DAP 实现，引脚见 [docs/board-pins.md](docs/board-pins.md)，默认 SWD 时钟 **2 MHz** |
| 调试协议 | SWD 全速率引擎（M5） | ✅ H723 目标验证（16 MHz 稳定；20 MHz 边缘） | ESP32-S3 专用 GPIO 引擎是**唯一 SWD 后端**：默认 160 MHz CPU 下 **100 kHz–20 MHz**，240 MHz 变体到 **24 MHz**；旧 GPIO/SPI SWD 后端在 S3 编译移除（无回退）；H723 上 100k–20M 全档验证、目标擦写与 GDB 变量通过；Web UI SWD 时钟可选 500 kHz–24 MHz，WS-DAP bring-up 自动从 24→1.9 MHz 逐档尝试；Kconfig 默认打开；见 docs/swd-23mhz-engine.md |
| 管理传输 | CMSIS-DAP over TCP 后端（M3） | 🧪 默认关闭，HIL 待办 | 端口 **3241**，OpenOCD `cmsis-dap backend tcp` 帧格式 + DAP 执行器桥接（`main/dap_tcp_bridge.c`）；OpenOCD master + STM32H723 目标级联调已真机验证（2026-11-15）；见 docs/dap-tcp-backend.md |
| 目标烧录 | Web 直接烧录 / STM32 识别 | ✅ F4/H7 实板验证 | 浏览器上传 `.bin`/`.elf`/`.hex`/`.srec`（前端转裸镜像），ESP32 经 SWD 流式写入（扇区擦除、x64/x32、SHA-256 校验 + SWD 复位）；**全 STM32 系列识别**（DEV_ID 表）；见 [docs/stm32-web-flash-plan.md](docs/stm32-web-flash-plan.md) |
| 传输性能 | USBIP/DAP 调优 | ✅ | A3：Wi-Fi 下 64 KB 读约 43 KiB/s（延迟受限）；目标烧录时暂停 UART 桥，Wi-Fi/HTTP 保持在线 |
| USB 传输 | USBIP / WinUSB | ✅ | CMSIS-DAP v2，TCP **3240**；Windows 推荐 usbip-win 0.3.0 `attach_ude` |
| USB 传输 | USB-NCM（S3 有线） | ✅ | 设备固定 IP **192.168.7.1/24** + 内置 DHCP；主机：`usbip attach -r 192.168.7.1 -b 1-1`（TCP 3240 不变）；**与 USB-JTAG/USJ 互斥**（同一 USB-OTG PHY；S3 NCM 构建关闭 USJ 次级控制台、保留 UART0 控制台） |
| 工具链 | OpenOCD / pyOCD / Keil | ✅ | 通过 CMSIS-DAP v2（上游能力）；建议 OpenOCD master 以获得 2.4× 读速（[docs/openocd-master.md](docs/openocd-master.md)） |
| 免驱传输 | elaphureLink / OpenOCD-elaphureLink | ✅ | Keil 插件或 OpenOCD 直连，TCP **3240**，无需 USBIP 驱动 |
| 浏览器传输 | WebSocket-DAP（legacy） | ✅（可关） | TCP **3240**，二进制帧 = CMSIS-DAP 命令包；`CONFIG_BIRDIEBOT_DAP_WS_ENABLE` |
| Web UI | 内嵌管理面板（zh/en） | ✅ | HTTP **80**，SharedWorker 单 WS 连接；页面：**目标烧录 / 设备状态 / 变量曲线 / UART / Wi-Fi / About**；除状态页外每页一条 compact 状态条；i18n 947 key |
| Wi-Fi | AP + STA（Smart APSTA） | ✅ | 默认 AP `WiFi-Dap-XXXX` / `12345678`；STA 连上 30 s 后关 AP、断开 10 s 后重开；mDNS TXT `name`/`serial`/`chip`/`fw` |
| Wi-Fi | 多凭据记忆 | ✅ | 最多 8 个网络，自动记忆 + 上电/断线轮换（[契约](docs/wifi-credential-memory.md)） |
| 串口 | Web UART 终端 + WS 二进制 | ✅ | 默认 115200-8N1，可配数据位/校验/停止位/流控 |
| 串口 | UART TCP 桥 | ✅（可关） | TCP **1346**；首包文本可切换波特率；`CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE`；C5 使用 UART1 GPIO25/26（回环实测） |
| 发现 | 仅 mDNS | ✅（SSDP 已移除） | `_http._tcp` 服务 + TXT `name`/`serial`/`chip`/`fw`（`serial=WiFi-Dap-<12HEX>`，每台唯一）；SSDP/UPnP（H1）与 `GET /api/discover`（H3）已在 slim 移除，见 归档索引 |
| 固件更新 | USB-JTAG / OpenOCD / UART0 | ✅ | **slim 无 Web OTA**（A1/A2）；经 UART0 或内置 USB Serial/JTAG 重烧（S3：BOOT+RESET ROM 下载；C5：OpenOCD + USB-JTAG）；OTA 分区 + 10 s 回滚确认保留；完整 Web OTA 可从 `backup/full-features-2026-10-26` 恢复 |
| 变量曲线 | Web 采样 | ✅ | 设备本地采样上限 **64 系列**（原 256，C4）；WS-DAP 模式 24 系列；协议分组格式不变 |
| 诊断 | coredump 到 Flash | ✅ | 64 KB `coredump` 分区（`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`） |
| 安全 | 无登录/鉴权（实验室模式）；**slim 默认关闭 Host/Origin 校验** | ⚠️ 实验室模式 | M1 风险已接受；可用 `CONFIG_BIRDIEBOT_WEB_HOST_GUARD=y` 恢复；见 [§9](#9-安全默认与加固) |
| 版本 | 固件 semver + 协议 v1 | ✅ | About 页 / `SYSTEM/GET_FM_INFO`，见 [docs/versioning.md](docs/versioning.md) |
| 身份 | 探针唯一序列号/多探针 | ✅ | `WiFi-Dap-<12HEX>`（WiFi STA MAC 派生）；DAP_Info(SER_NUM)、USBIP iSerial、mDNS TXT `serial` 同源（`components/probe_identity/`） |

## 3. 硬件与引脚

- 板级引脚与默认值是**冻结契约**：[docs/board-pins.md](docs/board-pins.md)（唯一权威来源）。
- 板卡选择：`idf.py menuconfig → WiFi Dap board configuration → Target board`；Kconfig 会按 `set-target` 自动给默认板。

| IDF target | 默认 board | 说明 |
|---|---|---|
| `esp32s3` | `BOARD_WIFI_DAP_S3` | 默认板，WiFi DAP S3 实机，8 MB |
| `esp32s3` | `BOARD_GENERIC_S3` | 通用 S3 接线，无状态灯，仅编译/参考 |
| `esp32c3` | `BOARD_GENERIC_C3` | 仅编译 |
| `esp32c5` | `BOARD_GENERIC_C5` | Wi-Fi-only，真机验证（非发布目标；IDF v5.5.6；无 USB-OTG/NCM） |
| `esp32` | `BOARD_GENERIC_ESP32` | 仅编译 |

默认板 `BOARD_WIFI_DAP_S3` 关键引脚：

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

**C5 UART 桥**：`UART_BRIDGE_TX_PIN=GPIO25` / `UART_BRIDGE_RX_PIN=GPIO26`（UART1 矩阵映射；115200-8N1 回环实测通过）。C5 细节见 docs/esp32-c5-port.md（由 C5 bring-up owner 维护）。

**电源**：目标板 3V3 / GND 需与 DAP 共地；调试目标建议由目标板自供电，DAP 的 GPIO 电压为 3.3 V。

---

## 4. 构建与烧录

### 4.1 环境准备

- **推荐**：Docker（`espressif/idf:v5.5.6` 镜像）+ `bash`、`flock`、`python3`、`gcc`。
- 也可直接安装 **ESP-IDF v5.5.6** 本地构建（`main/idf_component.yml` 要求 `idf >= 5.5.2`）。
- 仅当需要重新构建 Web UI 时才需要 Node.js（20+）与 npm；固件默认使用已嵌入的 `project_components/html/*.gz`。

```bash
docker pull espressif/idf:v5.5.6
```

### 4.2 一键构建（Docker）

```bash
# 三目标（S3 8MB + C3 4MB + ESP32 4MB；含分区门禁）
scripts/build_all.sh

# 只构建发布目标 / 干净重建
scripts/build_all.sh -t esp32s3
scripts/build_all.sh --clean -t esp32s3

# ESP32-C5（Wi-Fi-only、真机验证/非发布目标；C5 为 opt-in，不在默认矩阵）
scripts/build_all.sh -t esp32c5
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
| ESP32-C5（8 MB，Wi-Fi-only） | [`partitions/partitions_8mb_ota.csv`](partitions/partitions_8mb_ota.csv) | 与 S3 相同的 8 MB 双槽布局 | ✅ 双槽 + 回滚布局（slim 无 Web OTA UI/端点；真机验证，非发布目标） |

约束细节（app 上限、otadata、回滚语义）属完整功能构建历史契约，见 归档 ota-local-upload.md §1；slim 的端点/UI 移除与恢复方式见 [docs/feature-flags.md](docs/feature-flags.md)。

### 4.5 首次烧录（ESP32-S3 8 MB）

只写 **bootloader + 分区表 + ota_0**，`otadata` 保持空（0xFF），bootloader 会回退扫描启动 `ota_0`：

```bash
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  --device=/dev/ttyUSB0 -v "$PWD:/project" -w /project espressif/idf:v5.5.6 \
  bash -c 'esptool.py --chip esp32s3 -p /dev/ttyUSB0 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 build/esp32s3/bootloader/bootloader.bin \
    0x8000 build/esp32s3/partition_table/partition-table.bin \
    0x20000 build/esp32s3/birdiebot_wifi_dap.bin'
```

- ❌ **不要**写 `build/esp32s3/ota_data_initial.bin`，也**不要**用 `merge_bin` 全量包首烧（会初始化 otadata，掩盖首烧回退逻辑）。
- 串口控制台：UART0，115200（S3：GPIO43/44）。
- **之后的固件更新**：可选 OTA overlay（`sdkconfig.defaults.esp32s3.ota`）启用内置 OTA 上传（`scripts/ota_upload.py --host <设备> --file <app.bin> --info`）；不启用时通过 UART0 或内置 USB Serial/JTAG 重烧 `ota_0`（S3 按住 BOOT+RESET；C5 用 OpenOCD）。

### 4.6 发布包（可选）

```bash
scripts/release_local.sh                 # 严格模式：要求干净工作区 + vX.Y.Z tag
scripts/release_local.sh --allow-dirty   # 本机验证用
```

产物在 `build/release/`：`birdiebot_wifi_dap.bin`（app 镜像；供完整功能 OTA 构建使用）、bootloader、分区表、`flash.sh`、Web gz 资源、`SHA256SUMS` 等，详见 [scripts/README.md](scripts/README.md)。

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
   - mDNS：设备注册 `_http._tcp` 服务，主机名 `wifi-dap-XXXX.local`，TXT 记录 `name`/`serial`/`chip`/`fw`（`name=WiFi-Dap-XXXX`，**XXXX = STA MAC 最后两个字节的 4 位十六进制**；`serial` 为唯一序列号 `WiFi-Dap-<12HEX>`）（与默认 AP SSID 同后缀；例如 MAC 末两字节 CA:60 → `http://wifi-dap-ca60.local/`）；若解析失败请直接使用设备 IP。slim 已移除 SSDP/UPnP 发现。
4. 在 **Wi-Fi** 页填写并连接你的局域网（支持多网络记忆与自动切换），之后即可在局域网内访问 Web UI、调试器端口与 UART 桥。
5. 打开 **UART** 页，选择波特率（默认 115200-8N1）即可看目标串口输出；或直接使用 USBIP/elaphureLink 开始调试（[§7](#7-调试工具接入)）。

> 设备默认工作在 Smart APSTA：STA 未连接时 AP 保持开启；STA 连上 30 s 后自动关闭 AP，STA 断开 10 s 后重新开启 AP（延迟可在 Wi-Fi 页/JSON API 调整）。

---

## 6. Web 管理面板

入口：`http://<设备IP>/`（重定向到 `/#/target`）。前端与固件通过单条 WebSocket（`ws://<ip>/ws`）通信，文本帧为 JSON，二进制帧为 4 字节头 + payload；固件每 **1500 ms** 发送空 TEXT 帧心跳。协议细节见 [docs/protocol.md](docs/protocol.md) 与 [docs/ws-binary-router.md](docs/ws-binary-router.md)。

**final slim** Web UI 保留调试工作流，移除了旧的 OTA/Update 页、设备发现卡、首次使用引导、在线手册入口与 Ozone 兼容（见 [docs/feature-flags.md](docs/feature-flags.md)）。除设备状态页外，每页顶部有一条 **compact 状态条**（WebSocket 状态 · Wi-Fi SSID+RSSI · 目标 detected+UID 摘要 · 最近烧录结果）；点击进入设备状态页。

| 页面 | 路径 | 功能 |
|---|---|---|
| **目标烧录** | `/#/target` | 默认页：目标检测、SWD 时钟与网页端目标烧录（BIN/ELF/HEX/SREC → 裸镜像，SHA-256 + SWD 复位）。只有烧录流程 + compact 状态条，详细状态卡已迁到 `/#/status` |
| **设备状态** | `/#/status` | 只读详情页：DAP 会话、Wi-Fi、目标检测、物理链路融合、链路健康（浏览器实测 RTT / SWD 时钟 / heap）、错误摘要，以及诊断/恢复（DAP kick、目标复位、刷新、复制 JSON）。唯一运行周期性链路轮询的页面 |
| **变量曲线** | `/#/debug-curve` | 上传 ELF（解析 DWARF 类型：源变量名/类型/结构体成员/数组）选择变量，经 WS-DAP 最高 100 Hz 采样并实时画曲线，支持 CSV/PNG 导出（会接管 DAP 会话，见 docs/web-variable-curve.md）。设备本地采样上限 **64 系列**（C4） |
| **UART** | `/#/uart` | 串口终端（TX/RX）、波特率/数据位/校验/停止位/流控配置；数据走 WS 二进制 `RAW`/`RAW_BROADCAST` 帧 |
| **Wi-Fi** | `/#/wifi` | STA 扫描/连接、已保存网络（最多 8 个，可增删/自动保存）、AP 名称与口令、静态 IP/DNS、AP/STA 模式与 Smart APSTA 延迟 |
| **About** | `/#/about` | 固件版本 / 协议版本 / Web UI 版本 / 硬件版本 / SN / IDF 版本等信息 |

- 语言：中文 / English 可切换（i18n 947 key，见 [docs/i18n-keys.md](docs/i18n-keys.md)）。
- 状态页/compact 状态条说明：[docs/web-status-ui.md](docs/web-status-ui.md)；final slim HIL 证据：docs/hil-gate.md §11。
- 安全：按需求**无登录/鉴权**；Web UI 仅可在可信局域网使用（见 [§9](#9-安全默认与加固)）。
- 图片预览：[UART 终端](web/assets/uart.png)
- 更早移除：Widget / `#/widget` 仪表盘（2026-10-21），UART 保留数据显示器/宏命令等全部功能；slim 移除：`/#/update`（OTA）、首次引导、设备发现卡。

### 固件更新（final slim：无 Web OTA）

final slim 构建**没有浏览器 OTA / Update 页**（`POST /api/ota/upload` → 405，OTA JSON module 5 不注册；前端 Update 页与 OTA 客户端已删除）。

| 方式 | 操作 |
|---|---|
| **USB-JTAG / esptool（S3）** | 按住 **BOOT(GPIO0) + RESET** 进入 ROM 下载模式，S3 内置 USB Serial/JTAG 会枚举串口，然后按 [§4.5](#45-首次烧录esp32-s3-8-mb) 用 `esptool.py`/`idf.py -p <port> flash`。NCM/Wi-Fi slim 固件同样可用（ROM 下载模式与应用 USB 配置无关） |
| **UART0 ROM 下载** | BOOT+RESET + UART0（GPIO43/44）配合 esptool |
| **OpenOCD + USB-JTAG（C5）** | C5 无 USB-OTG，经内置 USB Serial/JTAG 用 OpenOCD 刷写，见 docs/esp32-c5-port.md |
| **完整功能 OTA（历史）** | 仅在从 `backup/full-features-2026-10-26` 恢复的构建可用；契约归档 docs/archive/removed-features/ota-local-upload.md |

恢复/重开步骤见 [docs/feature-flags.md](docs/feature-flags.md)（A1/A2）与 docs/archive/removed-features/README.md。

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

- 同样在 3240 端口：先以 HTTP `GET ` 完成 WebSocket 升级，之后的**二进制帧 payload 即 CMSIS-DAP 命令包**（见 components/dap_proxy/）。
- 可通过 `CONFIG_BIRDIEBOT_DAP_WS_ENABLE=n` 关闭（USBIP/elaphureLink 不受影响，端口仍监听）。
- 适合 [elaphure-dap.js](https://github.com/windowsair/elaphure-dap.js) 等浏览器端工具。

### 7.4 OpenOCD / pyOCD / Keil

设备对主机表现为标准 **CMSIS-DAP v2** 探针（SWD/JTAG），OpenOCD、pyOCD、Keil 等按 CMSIS-DAP 选型即可。DAP 默认 SWD 时钟为 **2 MHz**（`BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ`，见 [docs/board-pins.md §5](docs/board-pins.md)），高时钟由调试器侧选择。

---

## 8. UART 与数据通路

- **Web UART**：UART 页直接收发（WS 二进制帧），默认 115200-8N1。
- **UART TCP 桥**：裸 TCP 服务器，**端口 1346**，`数据 → TCP → 目标 UART TX`、`目标 UART RX → TCP → 数据`。
  - 新建连接后的**首个文本包**若为合法波特率（2–7 位数字，如 `115200`），桥会切换到该波特率；否则按普通数据透传。
  - 默认波特率 115200（旧版 1234/74880 已废弃）；可通过 `CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n` 关闭。
  - 例：`nc <设备IP> 1346`。
- **控制台 UART0**（S3：GPIO43/44，115200）只用于固件日志，不被 UART 桥/Web UART 占用。
- Web UART 与 TCP 桥共用同一 UART 服务，可同时连接；多客户端广播语义见 docs/hardware-test-uart-dataflow.md。

---

## 9. 安全默认与加固

### 9.1 默认状态（实验室模式）

| 面 | 默认 | 风险 |
|---|---|---|
| Web UI / JSON API / WS 数据面 | 无鉴权 | 同网段任意主机可读配置、控制 UART、操作探针 |
| 固件更新 | **slim 无 Web OTA 端点**（405）；重烧需物理/网络访问（USB-JTAG/UART0） | 有本地/网络访问即可覆盖固件；完整构建的 OTA 无签名 |
| USBIP / elaphureLink / WS-DAP（3240） | 无鉴权 | 可 attach 探针并读写目标芯片 |
| UART TCP 桥（1346） | 无鉴权 | 可直连目标串口 |
| Host/Origin 校验 | **slim 默认关闭**（M1）；任意 Host/Origin 放行 | 同网段浏览器可 CSWSH / DNS-rebinding |
| 传输加密 | 无（HTTP/WS/TCP 均明文） | 可被同网段嗅探/篡改 |
| Wi-Fi 凭据存储 | NVS 明文（无 flash encryption） | 物理读取可获取；`GET_AP_INFO`/`GET_STA_INFO` 会回传口令 |
| AP 口令 | `12345678` | 请首次使用即修改 |

> 建议：仅在**可信局域网/隔离 VLAN** 中使用；不要把调试口暴露到办公网或互联网。

### 9.2 已知安全限制（Known security limitations，实验室模式）

默认固件按“实验室模式”设计，以下限制在加固前**仍然存在**。完整分级、证据行号与修复建议见 **[docs/security-review-web.md](docs/security-review-web.md)**（历史 full 构建审查：P0 现状 0 / 条件性 P0 1 / P1 4 / P2 10；slim 差异：OTA/discover 端点已移除，Host/Origin 校验**默认关闭**）：

- **默认无鉴权**：HTTP **80**（`/`、`/ws`、`/api`）、TCP **3240**（USBIP / elaphureLink / WS-DAP）、TCP **1346**（UART TCP 桥）默认全部无鉴权，同一网络内即可完全控制设备与目标 MCU。
- **默认 AP 口令 `12345678`**：物理邻近即可加入设备热点，进而获得上述全部能力；首次使用必须修改。
- **完全无登录/鉴权（按需求移除）**：配对码/token 功能已删除；80/3240/1346 在可信内网开放。
- **slim 默认关闭 Host/Origin 校验（用户已接受）**：任意网页可 `new WebSocket("ws://<设备IP>/ws")` 跨源驱动设备（CSWSH），攻击者控制的 DNS 名可同源读写设备（DNS rebinding）。需要浏览器来源防护时设 `CONFIG_BIRDIEBOT_WEB_HOST_GUARD=y`。
- **完整构建的 OTA 无签名 / 无防降级**：`esp_ota_end()` 只做镜像格式校验，未启用 anti-rollback/Secure Boot。

> 注：`M1`（Host/Origin guard）在 final slim 中**默认关闭**；见 [docs/feature-flags.md](docs/feature-flags.md) §7。加固部署请重新开启（`CONFIG_BIRDIEBOT_WEB_HOST_GUARD=y`）并把实际来源加入白名单。部署加固清单见 [docs/security.md](docs/security.md)。

**Accepted risks (studio LAN)**：设备按设计**无登录/鉴权**（80/3240/1346 开放）、默认 AP 口令 `12345678` 未改，且 slim 默认关闭 Host/Origin 校验；这些风险在**可信实验室内网**下接受。使用约束与 N1–N9 状态见 [docs/security.md](docs/security.md)。**勿把设备暴露到公网或不可信 Wi-Fi。**

### 9.3 Host/Origin 校验（无登录；slim 默认关闭）

登录/配对码/token 鉴权已**全部移除**。完整功能构建使用“始终启用”的浏览器来源校验；**final slim 默认关闭**（`CONFIG_BIRDIEBOT_WEB_HOST_GUARD=n`，用户已接受实验室风险，见 [docs/feature-flags.md](docs/feature-flags.md) §7）：

- 开启时（`=y`）：`Host` 必须是 IP literal、`CONFIG_LWIP_LOCAL_HOSTNAME` 或 `BIRDIEBOT_WEB_ALLOWED_ORIGIN` 名单（防 DNS rebinding）；浏览器带 `Origin` 时必须同源或命中名单（防 CSWSH）；非浏览器客户端不带 Origin，仍可访问——**这不是身份认证**。
- 关闭时（slim 默认）：完全没有 Host/Origin 校验，任意来源都可访问设备；仅在可信局域网使用。

脚本不再需要/支持 `--token`。

### 9.4 关闭不需要的传输

```text
idf.py menuconfig
  → WiFi Dap proxy (USBIP / WebSocket-DAP) → BIRDIEBOT_DAP_WS_ENABLE = n
  → WiFi Dap UART TCP bridge                  → BIRDIEBOT_UART_BRIDGE_ENABLE = n
```

USBIP 与 elaphureLink 没有单独开关（3240 端口保持监听）；如不需要调试功能，请在网络层限制 3240/1346 端口访问。

### 9.5 其它建议

- 首次使用后修改 AP 口令（Wi-Fi 页 → AP 设置）。
- 只在可信网络配置 STA；避免把设备接入公共热点。
- 发布固件时开启 Secure Boot/Flash Encryption（本项目默认未启用）；启用后 `esp_ota_end()` 会校验镜像签名。
- coredump 可能包含内存中的敏感数据（含 Wi-Fi 口令），转储文件请按敏感信息处理。

---

## 10. 版本与协议

| 版本 | 来源 | 运行时可见 |
|---|---|---|
| 固件版本 `vMAJOR.MINOR.PATCH` | 本地 git tag（`git describe`，回退 `version.txt`/`v1.0.0`） | 启动日志、About 页、`SYSTEM/GET_FM_INFO.fm_ver`（OTA `GET_INFO` 仅完整构建） |
| 协议版本 `protocol_version = 1` | `project_components/wt_common/wt_protocol_version.h`（唯一来源） | 启动日志、`GET_FM_INFO.protocol_version`、About 页 |
| Web UI 版本 `1.0.0` | `web/package.json`，构建时注入 | About 页、`build-info.json`、`web_version.txt` |

- 连接后前端发送 `SYSTEM/GET_FM_INFO` 做版本握手；协议不匹配时提示“固件与网页版本不匹配”，提示刷新页面或升级固件（slim 无 Web OTA 可禁用）。
- 发布一致性由 `scripts/check-release-versions.sh` 等门禁脚本校验，详见 [docs/versioning.md](docs/versioning.md) 与 [scripts/README.md](scripts/README.md)。

---

## 11. 与上游的关系 / 变更说明

三方来源（细节与逐文件映射见 [docs/merge-map.md](docs/merge-map.md)）：

| 代号 | 仓库 | 许可 | 贡献 |
|---|---|---|---|
| **A** | [nemoshistudio/wireless-esp8266-dap](https://github.com/nemoshistudio/wireless-esp8266-dap)（windowsair 上游 fork） | MIT | CMSIS-DAP 核心、USBIP/elaphureLink 代理、LED/S3 UART 等 fork 改动 |
| **C** | [kerms/wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) | Apache-2.0 | ESP-IDF 5.5.6 平台层：wifi_manager / web_server / wt_nvs / memory_pool / SSDP / UART service / DataFlow |
| **D** | [kerms/wireless-esp32-tools-web-host](https://github.com/kerms/wireless-esp32-tools-web-host) | MIT | Vue 3 + Vite Web UI（本项目在其基础上扩展 OTA/WiFi 管理/鉴权/i18n） |

本仓库（WiFi Dap）的主要变更：

- 升级到 **ESP-IDF 5.5.6**，按组件拆分（`components/` + `project_components/`），板级引脚抽象为 `components/board`（`BOARD_*` + Kconfig，去掉按 target 写死的引脚分支）。
- 新增 **Web UI + JSON/WS 协议 v1**（[docs/protocol.md](docs/protocol.md)）、Wi-Fi 多凭据记忆、浏览器本地 OTA（8 MB 双槽 + 回滚；**final slim 已移除**，见 [docs/feature-flags.md](docs/feature-flags.md)）、可选 Web 鉴权、SSDP/mDNS（SSDP **final slim 已移除**）、coredump、UART service/DataFlow 数据面。
- 保留并修复上游 DAP 代理层（USBIP / elaphureLink / legacy WebSocket-DAP）：DAP ring buffer 生命周期归 DAP 任务、socket fd 泄漏修复、错误路径处理等。
- 默认值调整：SWD 默认时钟 **2 MHz**（v1.0.0 曾为 1 MHz）、UART 桥 1346/115200、AP SSID `WiFi-Dap-XXXX`、移除 ESP8266/KCP/corsacOTA(3241)。
- 默认板 `BOARD_WIFI_DAP_S3`（"WiFi DAP S3"）；C3/ESP32 仅编译验收。

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

### 13.2 WebSocket / UART 自动化（需设备）

```bash
# 连接 + 版本握手
python3 scripts/e2e_ws_client.py --host <ip> ping
# Wi-Fi 扫描 / UART 回环 / DataFlow / 任意 JSON
python3 scripts/e2e_ws_client.py --host <ip> wifi-scan
python3 scripts/e2e_ws_client.py --host <ip> uart-echo --payload 48656c6c6f --hex
python3 scripts/e2e_ws_client.py --host <ip> dataflow-list
```

> `e2e_ws_client.py ota-info` 与 `scripts/ota_upload.py` 仅适用于**完整功能构建**（从 `backup/full-features-2026-10-26` 恢复）；final slim 固件已移除 OTA 端点/module。脚本保留用于 full 构建回归。

### 13.3 硬件验收清单与 final slim HIL

- **final slim HIL（推荐）**：docs/hil-gate.md §11 —— `--profile auto` 自动识别 slim 固件、断言 OTA/discover 端点不存在、6 条 hash 路由（`/status`、`/target`、`/debug-curve`、`/uart`、`/wifi`、`/about`）、状态页 + compact 状态条、WS/mDNS（TXT `name`/`serial`/`chip`/`fw`）。设备证据：`analysis/slim-hil/` —— **S3-F407 600 s soak clean**（110 cycles，0 failures）与 **C5-H7 180 s soak clean**（33 cycles，0 failures），USBIP ×2 与 SWD 时钟矩阵 PASS。
- **UART/DataFlow/WS 二进制**：docs/hardware-test-uart-dataflow.md —— T1–T8 场景。
- **DAP/USBIP 真机验证**：docs/hardware-test-dap.md。
- **OTA（历史，仅完整构建）**：docs/archive/removed-features/hardware-test-ota.md。

---

## 14. 故障排查

| 现象 | 优先检查 |
|---|---|
| 找不到 `WiFi-Dap-XXXX` 热点 | 是否已首次烧录；板卡 board 选择是否正确（`sdkconfig.defaults.*`）；串口日志是否有 Wi-Fi 初始化错误；STA 已连接时 AP 会在 30 s 后关闭（正常） |
| 打开 `192.168.1.1` 无响应 | 手机/PC 是否连的是设备 AP；是否被 Smart APSTA 关闭了 AP；改连 STA 网段的设备 IP |
| `wifi-dap-XXXX.local` 无法解析 | 设备/客户端 mDNS 支持；检查 TXT `name`/`serial`/`chip`/`fw`；改用 IP 访问（主机名见 `wt_mdns_config.h`，XXXX 为 MAC 后两字节）。slim 已移除 SSDP 发现 |
| Web UI 提示在线但操作被拒（HTTP 403） | 只在重新开启 Host/Origin guard 的加固构建可能出现：用 IP 访问，或把主机名加入 `BIRDIEBOT_WEB_ALLOWED_ORIGIN`（slim 默认关闭 guard） |
| `/#/update` 返回 404 | 预期：final slim 无 Web OTA/Update 页。用 USB-JTAG/UART0 重烧（见 §4.5），或恢复完整功能构建 |
| `POST /api/ota/upload` 返回 405/404 | 预期：slim 已移除 OTA 端点。改用 USB-JTAG/OpenOCD/UART0 |
| `GET /api/discover` 返回 HTML/404 | 预期：slim 已移除发现端点；mDNS 是唯一发现路径 |
| `usbip.exe attach_ude` 失败 | usbip-win 版本是否 0.3.0+；设备 IP/网段是否可达；3240 是否被防火墙拦截；busid 固定为 `1-1` |
| DAP 断连/很慢 | Wi-Fi 环境与距离；2.4 GHz 干扰；调试器时钟过高；同网段广播风暴 |
| UART 页/桥无数据 | UART 页波特率与目标一致？引脚是否为板级 TX/RX（S3 GPIO10/9；C5 GPIO25/26，见 [docs/board-pins.md](docs/board-pins.md)）？目标是否真的在输出？控制台 UART0 不经过桥 |
| 重烧后无法启动 | 串口日志 + coredump：`idf.py coredump-info`（需与构建相同版本 IDF）；必要时 `esptool.py erase_flash` 后按 §4.5 重烧 |
| 版本显示 `v1.0.0`/空 | 构建环境无 `vX.Y.Z` tag 且无 `version.txt`（开发构建回退属预期），见 [docs/versioning.md](docs/versioning.md) |

完整构建的 OTA 错误码与回滚步骤属历史内容：docs/archive/removed-features/hardware-test-ota.md §5/§6。

---

## 15. 文档、反馈与许可证

- **文档索引**：[docs/README.md](docs/README.md)（当前文档地图、冻结契约、测试清单与维护规则）。
- **当前功能状态**：[docs/feature-flags.md](docs/feature-flags.md)（final slim 移除项、Kconfig 开关、备份 tag、恢复步骤）；docs/archive/README.md（历史证据/已移除功能）。
- **关键契约**：[protocol.md](docs/protocol.md)（JSON/WS 协议）、[wifi-credential-memory.md](docs/wifi-credential-memory.md)、[versioning.md](docs/versioning.md)、[board-pins.md](docs/board-pins.md)、[web-status-ui.md](docs/web-status-ui.md)（状态页 + compact 状态条）。
- **安全**：[docs/security.md](docs/security.md)（部署加固指南）；[docs/security-review-web.md](docs/security-review-web.md)（历史 full 构建 Web/OTA/WiFi 安全审查——slim 已移除 OTA/discover 端点且默认关闭 Host/Origin guard）。
- **反馈**：请在项目仓库提 Issue。
- **作者**：本仓库由本项目贡献者维护；下列上游项目是依赖并已署名，并非本仓库作者。
- **许可证**（混合许可；完整声明见 [NOTICE](NOTICE)，全文见 LICENSES/）：
  - **本项目原创部分**：**Apache License 2.0**（根目录 [LICENSE](LICENSE) / [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)）；个别文件头另行标注（如部分 `scripts/` 为 SPDX MIT）。
  - **第三方组件按各自原许可证**：
    - windowsair/wireless-esp8266-dap —— **MIT**，覆盖 `components/DAP`、`components/USBIP`、`components/elaphureLink`、`components/dap_proxy`（各目录 `LICENSE`）；
    - kerms/wireless-esp32-tools —— **Apache-2.0**，覆盖 ESP-IDF 5.5.6 平台层组件（源文件 SPDX 头）；
    - kerms/wireless-esp32-tools-web-host —— **MIT**，覆盖 `web/` 前端（[web/LICENCE.txt](web/LICENCE.txt)）；
    - ARM CMSIS-DAP —— **Apache-2.0**，覆盖 `components/DAP/cmsis-dap/**`。
  - MIT 全文：[LICENSES/MIT.txt](LICENSES/MIT.txt)；版权署名与出处汇总：[NOTICE](NOTICE)；上游差异见 [§11](#11-与上游的关系--变更说明)。
