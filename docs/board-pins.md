# WiFi Dap 板级引脚表

本文件是 `components/board` 的引脚/板级默认值契约。默认且受支持的板卡是
`BOARD_WIFI_DAP_S3`；其它板卡仅用于编译或参考，不在 README 的支持范围内。

## 1. 选择板卡

在 `idf.py menuconfig → WiFi Dap board configuration → Target board` 中选择；
`idf.py set-target` 会自动给出与 IDF target 匹配的默认板卡。

| IDF target | 默认 board |
|---|---|
| `esp32s3` | `BOARD_WIFI_DAP_S3` |
| `esp32c3` | `BOARD_GENERIC_C3`（仅编译） |
| `esp32` | `BOARD_GENERIC_ESP32`（仅编译） |
| `esp32c5` | `BOARD_GENERIC_C5`（未测试） |

## 3. 板卡总览

| board | target | UART 桥实例 | 状态灯 | WiFi 灯 | 用途 |
|---|---|---|---|---|---|
| `BOARD_WIFI_DAP_S3` | ESP32-S3 | UART1 | ✅（5/6） | ✅（4） | 默认板，WiFi DAP S3 实机 |
| `BOARD_GENERIC_S3` | ESP32-S3 | UART1 | ❌ | ❌ | 上游通用 S3 接线（仅编译/参考） |
| `BOARD_GENERIC_C3` | ESP32-C3 | UART1 | ❌ | ❌ | 仅编译 |
| `BOARD_GENERIC_C5` | ESP32-C5 | UART1 | ❌ | ❌ | 真机验证（非发布目标）；Wi-Fi-only（无 USB-OTG/NCM） |
| `BOARD_GENERIC_ESP32` | ESP32 | UART2 | ❌ | ❌ | 仅编译 |

## 4. 引脚表

## 4. 各板引脚表
### 4.1 `BOARD_WIFI_DAP_S3`（默认，ESP32-S3，8MB）
| 功能 | 宏 | 引脚 | 说明 |
|---|---|---|---|
| SWCLK / TCK | `PIN_SWCLK` | GPIO12 | 勿改 |
| SWDIO / TMS | `PIN_SWDIO_MOSI` | GPIO11 | 单线 SWDIO；`PIN_SWDIO` 同值 |
| TDO → host | `PIN_TDO` | GPIO15 | JTAG 用 |
| host → TDI | `PIN_TDI` | GPIO16 | JTAG 用 |
| nTRST | `PIN_nTRST` | GPIO14 | 可选 |
| nRESET | `PIN_nRESET` | GPIO13 | |
| Connected LED | `PIN_LED_CONNECTED` | GPIO5 | `HAS_STATUS_LEDS=1` |
| Running LED | `PIN_LED_RUNNING` | GPIO6 | `HAS_STATUS_LEDS=1` |
| WiFi 状态 LED | `PIN_LED_WIFI_STATUS` | GPIO4 | `HAS_WIFI_LED=1` |
| UART 桥 TX | `UART_BRIDGE_TX_PIN` | GPIO10 | UART1（`UART_NUM_1`） |
| UART 桥 RX | `UART_BRIDGE_RX_PIN` | GPIO9 | UART1；控制台仍在 UART0 |
### 4.2 `BOARD_GENERIC_S3`（ESP32-S3，上游通用接线）
| 功能 | 宏 | 引脚 |
|---|---|---|
| SWCLK / TCK | `PIN_SWCLK` | GPIO12 |
| SWDIO / TMS | `PIN_SWDIO_MOSI` | GPIO11 |
| TDO → host | `PIN_TDO` | GPIO9 |
| host → TDI | `PIN_TDI` | GPIO10 |
| nTRST | `PIN_nTRST` | GPIO14 |
| nRESET | `PIN_nRESET` | GPIO13 |
| UART 桥 TX | `UART_BRIDGE_TX_PIN` | GPIO17（UART1） |
| UART 桥 RX | `UART_BRIDGE_RX_PIN` | GPIO18（UART1） |
| LED | — | 无：`HAS_STATUS_LEDS=0`、`HAS_WIFI_LED=0`（宏为 `_`） |
### 4.3 `BOARD_GENERIC_C3`（ESP32-C3，仅编译）
| 功能 | 宏 | 引脚 |
|---|---|---|
| SWCLK / TCK | `PIN_SWCLK` | GPIO6 |
| SWDIO / TMS | `PIN_SWDIO_MOSI` | GPIO7 |
| TDO → host | `PIN_TDO` | GPIO3 |
| host → TDI | `PIN_TDI` | GPIO5 |
| nTRST | `PIN_nTRST` | GPIO4 |
| nRESET | `PIN_nRESET` | GPIO10 |
| UART 桥 TX | `UART_BRIDGE_TX_PIN` | GPIO21（UART1） |
| UART 桥 RX | `UART_BRIDGE_RX_PIN` | GPIO20（UART1） |
| LED | — | 无：`HAS_STATUS_LEDS=0`、`HAS_WIFI_LED=0`（宏为 `_`） |
### 4.4 `BOARD_GENERIC_ESP32`（ESP32，仅编译）
| 功能 | 宏 | 引脚 |
|---|---|---|
| SWCLK / TCK | `PIN_SWCLK` | GPIO14 |
| SWDIO / TMS | `PIN_SWDIO_MOSI` | GPIO13 |
| TDO → host | `PIN_TDO` | GPIO19 |
| host → TDI | `PIN_TDI` | GPIO18 |
| nTRST | `PIN_nTRST` | GPIO25 |
| nRESET | `PIN_nRESET` | GPIO26 |
| UART 桥 TX | `UART_BRIDGE_TX_PIN` | GPIO23（UART2） |
| UART 桥 RX | `UART_BRIDGE_RX_PIN` | GPIO22（UART2） |
| LED | — | 无：`HAS_STATUS_LEDS=0`、`HAS_WIFI_LED=0`（宏为 `_`） |
### 4.5 `BOARD_GENERIC_C5`（ESP32-C5，真机验证，非发布目标）
| 功能 | 宏 | 引脚 |
|---|---|---|
| SWCLK / TCK | `PIN_SWCLK` | GPIO6（SPI2 FSPICLK 直连） |
| SWDIO / TMS | `PIN_SWDIO_MOSI` | GPIO7（SPI2 FSPID 直连；strapping，boot 后可用） |
| TDO → host | `PIN_TDO` | GPIO8 |
| host → TDI | `PIN_TDI` | GPIO9 |
| nTRST | `PIN_nTRST` | GPIO3（C5 自身 JTAG MTDI；USB-JTAG 调试时释放） |
| nRESET | `PIN_nRESET` | GPIO10（SPI2 FSPICS0，可作普通 GPIO） |
| UART 桥 TX | `UART_BRIDGE_TX_PIN` | GPIO25（UART1） |
| UART 桥 RX | `UART_BRIDGE_RX_PIN` | GPIO26（UART1） |
| LED | — | 无：`HAS_STATUS_LEDS=0`、`HAS_WIFI_LED=0`（宏为 `_`） |

## 5. 默认参数

- SWD 默认时钟：**2 MHz**（`BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ`）。
- UART 桥：端口 **1346**，波特率 **115200**。
- 状态 LED 与 Wi-Fi LED 仅默认 S3 板提供。

## 8. 主机语法检查

`scripts/check_board_pins.py` 会编译并比对本文件的引脚表；提交前运行：

```bash
python3 scripts/check_board_pins.py
```
