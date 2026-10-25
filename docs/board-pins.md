# WiFi Dap 板级引脚表（WP-006 / `components/board`）

本文件是 **板级引脚与硬件默认值的唯一契约**（Wave 0，WP-006）。
固件中所有 SWD/JTAG、UART TCP 桥、状态 LED 的引脚都必须来自
[`components/board/include/birdiebot_board.h`](../components/board/include/birdiebot_board.h)
选中的板文件，不允许在驱动里再写死 `#ifdef CONFIG_IDF_TARGET_*` 引脚分支
（这正是旧 fork 在 ESP32C3 上编译失败、在 ESP32/ESP8266 上污染 Flash 引脚的原因，
见 `analysis/03-fork-build.md` §2.3/§4）。

- 组件：`components/board/`（IDF5 `idf_component_register`，header-only + 极小 `src/birdiebot_board.c`）
- 默认板：`BOARD_ZXR_DAP_S3`
- 关联：WP-101（DAP_config 引脚走 board 头）、WP-114（UART TCP 桥）、WP-007（G0 三目标构建）
- 状态：初版，引脚值沿用 A（旧 fork `zxr_dap`）实测配置 + C 树通用值；**上板前需按实际硬件复核**（集成计划 §5.2 ⚠）

---

## 1. 如何选择板卡

Kconfig 菜单位置：

```
idf.py menuconfig
  -> WiFi Dap board configuration
       -> Target board
            ( ) BOARD_ZXR_DAP_S3   (default, ESP32-S3 WiFi Dap)
            ( ) BOARD_GENERIC_S3
            ( ) BOARD_GENERIC_C3
            ( ) BOARD_GENERIC_C5   (compile-only, ESP32-C5)
            ( ) BOARD_GENERIC_ESP32
```

Kconfig 会按 `idf.py set-target` 自动给出默认值（`components/board/Kconfig`）：

| IDF target | 默认 board | 可选 board |
|---|---|---|
| `esp32s3` | `BIRDIEBOT_BOARD_ZXR_DAP_S3` | `..._ZXR_DAP_S3` / `..._GENERIC_S3` |
| `esp32c3` | `BIRDIEBOT_BOARD_GENERIC_C3` | `..._GENERIC_C3` |
| `esp32` | `BIRDIEBOT_BOARD_GENERIC_ESP32` | `..._GENERIC_ESP32` |
| `esp32c5` | `BIRDIEBOT_BOARD_GENERIC_C5` | `..._GENERIC_C5` |

对应 sdkconfig 宏：`CONFIG_BIRDIEBOT_BOARD_ZXR_DAP_S3=y` 等。
`components/board/Kconfig` 里用 `depends on IDF_TARGET_*` 隐藏了与当前 target
不匹配的选项；若在 `sdkconfig.defaults.esp32c3` 等处显式指定，请写整名
（如 `CONFIG_BIRDIEBOT_BOARD_GENERIC_C3=y`）。

> C3 / ESP32 / C5 在本项目只做**编译验收**（集成计划 §5.2；C5 见
> docs/esp32-c5-port.md），不做硬件验证。C5 板是 **Wi-Fi-only** 变体（该芯片
> 无 USB-OTG/NCM）。

---

## 2. 宏命名契约（与 A 的 DAP_config 兼容）

| 宏 | 含义 | 旧 A `DAP_config.h` / 驱动中的用法 |
|---|---|---|
| `PIN_SWCLK` | SWCLK / TCK | 原样兼容 |
| `PIN_SWDIO_MOSI` | 单线 SWDIO（输出+输入） | 原样兼容（A/C 的 `PIN_SWDIO_*_OUT/IN` 都用它） |
| `PIN_SWDIO` | 可选别名：默认 `#define PIN_SWDIO PIN_SWDIO_MOSI` | A 的 ESP8266/ESP32 双线 MISO 语义不再使用；板文件可先定义覆盖 |
| `PIN_TDO` | 目标 TDO → host 数据输入 | 原样兼容 |
| `PIN_TDI` | host 数据输出 → 目标 TDI | 原样兼容 |
| `PIN_nTRST` | 可选 JTAG TRST | 原样兼容 |
| `PIN_nRESET` | 目标 RESET | 原样兼容 |
| `PIN_LED_CONNECTED` | CMSIS-DAP Connected 灯 | 原样兼容；**必须**先判断 `BIRDIEBOT_BOARD_HAS_STATUS_LEDS` |
| `PIN_LED_RUNNING` | CMSIS-DAP Running 灯 | 同上 |
| `PIN_LED_WIFI_STATUS` | WiFi 连接状态灯（旧 `main/wifi_handle.c` 的宏名） | **必须**先判断 `BIRDIEBOT_BOARD_HAS_WIFI_LED` |
| `UART_BRIDGE_TX_PIN` / `UART_BRIDGE_RX_PIN` | UART TCP 桥 TX/RX | 旧 `main/uart_bridge.c` 宏名 |
| `BIRDIEBOT_UART_BRIDGE_UART_NUM` | UART 实例号（`uart_port_t` 整数；S3/C3=1，ESP32=2） | 新宏，替代 `UART_BRIDGE_TX/RX UART_NUM_x` |
| `BIRDIEBOT_UART_BRIDGE_PORT` | 1346（项目级默认） | 替代旧 `wifi_configuration.h` 的 `UART_BRIDGE_PORT` |
| `UART_BRIDGE_BAUDRATE` | 115200（项目级默认） | 原样兼容 |
| `BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ` | 2000000（2MHz，项目级默认，真机验证） | WP-101 映射到 `DAP_DEFAULT_SWJ_CLOCK` |
| `BIRDIEBOT_BOARD_ID` / `BIRDIEBOT_BOARD_NAME` | 机器名 / 可读名 | 新增；DAP_Info / 启动日志使用 |
| `BIRDIEBOT_BOARD_TARGET` | `"esp32s3"` / `"esp32c3"` / `"esp32"` | 新增；文档/日志用字符串 |
| `BIRDIEBOT_BOARD_TARGET_ID` | 1=esp32, 2=esp32c3, 3=esp32s3 | 新增；`src/birdiebot_board.c` 做 board/target 一致性检查（warning） |
| `BIRDIEBOT_BOARD_HAS_STATUS_LEDS` | 0/1，是否有 Connected/Running 灯 | **新增，必须使用**；0 时 LED 宏为 `_` |
| `BIRDIEBOT_BOARD_HAS_WIFI_LED` | 0/1，是否有 WiFi 状态灯 | **新增，必须使用** |

### LED 的 `_` 占位符规则

没有 LED 的板（`generic_*`）把三个 LED 宏定义为 `_`（沿用 A 的空桩写法），
并令 `BIRDIEBOT_BOARD_HAS_*_LED = 0`。**直接对 `_` 调用 GPIO 宏会编译报错**
——这是刻意保留的失败模式，保证“某个 target 没有灯”不会被静默成“驱动了某个随机 GPIO”。
正确写法：

```c
#include "birdiebot_board.h"

#if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
    GPIO_FUNCTION_SET(PIN_LED_CONNECTED);
    GPIO_SET_DIRECTION_NORMAL_OUT(PIN_LED_CONNECTED);
    GPIO_SET_LEVEL_HIGH(PIN_LED_CONNECTED);
#else
    /* no status LEDs on this board */
#endif
```

---

## 3. 板卡总览

| board | target | UART 桥实例 | 状态灯 | WiFi 灯 | 用途 |
|---|---|---|---|---|---|
| `BOARD_ZXR_DAP_S3` | ESP32-S3 | UART1 | ✅（5/6） | ✅（4） | 默认板，WiFi Dap 实机 |
| `BOARD_GENERIC_S3` | ESP32-S3 | UART1 | ❌ | ❌ | 上游通用 S3 接线（仅编译/参考） |
| `BOARD_GENERIC_C3` | ESP32-C3 | UART1 | ❌ | ❌ | 仅编译 |
| `BOARD_GENERIC_C5` | ESP32-C5 | UART1 | ❌ | ❌ | 仅编译；Wi-Fi-only（无 USB-OTG/NCM） |
| `BOARD_GENERIC_ESP32` | ESP32 | UART2 | ❌ | ❌ | 仅编译 |

板文件路径：`components/board/boards/board_<board_id>.h`；
所有板共用 `components/board/include/birdiebot_board.h` 入口。

---

## 4. 各板引脚表

### 4.1 `BOARD_ZXR_DAP_S3`（默认，ESP32-S3，8MB）

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

> 与旧 fork 的差异：旧 A 的 `DAP_config.h` 里 S3 的 TDO/TDI 是 9/10（`4e29366`
> 后改 15/16）、UART 是 10/9（`4e29366`），LED 是 5/6 + WiFi 4（`742f2de`）。
> 本表即 fork 的最新实测值，但 `PIN_TDO/TDI` 与 UART 的 9/10 复用关系仍需上板复核。

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

> 注意：旧 A fork 的 C3 引脚是 TDO8/TDI9/nRESET5；本表按 C 树（IDF5）通用值
> TDO3/TDI5/nRESET10，二者不一致。C3 只做编译验收，不做硬件验证。

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

> 遗留：旧 A 的 ESP32 参考电路是双线 SWDIO（MISO=GPIO12、MOSI=GPIO13）。本板
> 文件按新架构只给单线 `PIN_SWDIO_MOSI=13`，`PIN_SWDIO` 是它的别名。若实际使用
> 双线电路，WP-101 需要复核输入采样路径（或在板文件中先定义 `PIN_SWDIO 12`）。

### 4.5 `BOARD_GENERIC_C5`（ESP32-C5，仅编译）

| 功能 | 宏 | 引脚 |
|---|---|---|
| SWCLK / TCK | `PIN_SWCLK` | GPIO6（SPI2 FSPICLK 直连） |
| SWDIO / TMS | `PIN_SWDIO_MOSI` | GPIO7（SPI2 FSPID 直连；strapping，boot 后可用） |
| TDO → host | `PIN_TDO` | GPIO8 |
| host → TDI | `PIN_TDI` | GPIO9 |
| nTRST | `PIN_nTRST` | GPIO3（C5 自身 JTAG MTDI；USB-JTAG 调试时释放） |
| nRESET | `PIN_nRESET` | GPIO10（SPI2 FSPICS0，可作普通 GPIO） |
| UART 桥 TX | `UART_BRIDGE_TX_PIN` | GPIO23（UART1） |
| UART 桥 RX | `UART_BRIDGE_RX_PIN` | GPIO24（UART1） |
| LED | — | 无：`HAS_STATUS_LEDS=0`、`HAS_WIFI_LED=0`（宏为 `_`） |

> C5 引脚约束（datasheet §2.3/§2.4）：GPIO13/14 = USB Serial/JTAG（本变体只作
> console/下载，无 USB-OTG）；GPIO15~18/20~22 = flash/PSRAM；GPIO27 = VDD_SPI
> （QFN48）；GPIO2/3/7/25/26/27/28 为 strapping；GPIO2~5 为 C5 自身 JTAG。
> 该板为**编译验收**接线（无 C5 硬件验证），上板前必须按实际板复核；详见
> docs/esp32-c5-port.md。

---

## 5. 项目级默认值（与板无关）

| 项 | 默认值 | 归属/说明 |
|---|---|---|
| 默认 board | `BOARD_ZXR_DAP_S3` | 本组件；Kconfig `BIRDIEBOT_BOARD` |
| mDNS 主机名 | `wifi-dap-XXXX.local` | XXXX = STA MAC 最后两个字节（4 位小写十六进制），与 AP SSID `WiFi-Dap-XXXX` 同后缀；见 `wt_mdns_config.c` |
| AP SSID 前缀 | `WiFi-Dap-` | 后缀为 STA MAC 末 2 字节大写 hex（如 `WiFi-Dap-A1B2`），规则见 WiFi 多凭据契约 |
| SWD 默认时钟 | **2 MHz** | `BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ=2000000`；真机 STM32F407 验证稳定；4MHz/10MHz 无额外吞吐收益（USBIP/WiFi 延迟为瓶颈） |
| UART 桥端口 | **1346** | `BIRDIEBOT_UART_BRIDGE_PORT`（旧 A 的 1234 已废弃） |
| UART 桥波特率 | **115200** | `UART_BRIDGE_BAUDRATE`（旧 A 的 74880 已废弃） |
| 控制台 | UART0（S3：GPIO43/44） | 不被 UART 桥占用 |
| 固件服务端口 | 3240（USBIP/DAP）、80（HTTP）、1346（UART TCP 桥）、1900/5353（SSDP/mDNS） | 集成计划 §1.4；本期**不启用** OTA 3241 |

---

## 6. 使用示例

```c
/* 驱动/WP-101/WP-114 消费方式 */
#include "birdiebot_board.h"
#include "driver/gpio.h"

void board_bringup(void)
{
    /* SWD/JTAG 引脚：名字直接沿用旧 DAP_config.h 宏 */
    gpio_set_direction(PIN_SWCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_SWDIO_MOSI, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(PIN_TDO, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_TDI, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_nRESET, GPIO_MODE_OUTPUT);
#if BIRDIEBOT_BOARD_HAS_WIFI_LED
    gpio_set_direction(PIN_LED_WIFI_STATUS, GPIO_MODE_OUTPUT);
#endif

    /* UART TCP 桥（WP-114） */
    uart_set_pin(BIRDIEBOT_UART_BRIDGE_UART_NUM,
                 UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN, -1, -1);

    printf("board=%s id=%s target=%s\n",
           birdiebot_board_name, birdiebot_board_id, BIRDIEBOT_BOARD_TARGET);
}
```

板选择由 Kconfig 决定，驱动代码中**不应**再出现
`#if defined(CONFIG_IDF_TARGET_ESP32S3)` 之类引脚分支。

---

## 7. 新增一块板（checklist）

1. 新建 `components/board/boards/board_<id>.h`，定义 §2 表中所有必需宏
   （含 `BIRDIEBOT_BOARD_ID/NAME/TARGET/TARGET_ID/HAS_*` 与 UART 引脚）。
2. `components/board/Kconfig`：在 `choice BIRDIEBOT_BOARD` 内新增
   `config BIRDIEBOT_BOARD_<ID>`（`bool` + `depends on IDF_TARGET_*` + help），
   必要时补 per-target `default`。
3. `components/board/include/birdiebot_board.h`：在 board selection 的
   `#if/#elif` 链中加一行 `#include "../boards/board_<id>.h"`。
4. 在本文件 §3/§4 增加该板的表，保证文档值与宏一致。
5. 验证：见 §8 的独立语法检查 + 目标构建。

---

## 8. 验证与约束

### 8.1 本地静态验证（无需 IDF，本机即可）

```bash
# 默认板（无 sdkconfig 时回退 BOARD_ZXR_DAP_S3）
gcc -fsyntax-only -x c -I components/board/include - <<'EOF'
#include "birdiebot_board.h"
int main(void) { return BIRDIEBOT_BOARD_TARGET_ID + BIRDIEBOT_BOARD_HAS_STATUS_LEDS; }
EOF

# 四块板逐一语法检查（-D 模拟 Kconfig 选择）
for b in ZXR_DAP_S3 GENERIC_S3 GENERIC_C3 GENERIC_C5 GENERIC_ESP32; do
  gcc -fsyntax-only -x c -DCONFIG_BIRDIEBOT_BOARD_$b \
      -I components/board/include - <<'EOF'
#include "birdiebot_board.h"
EOF
done

# 组件源码（含 board/target 一致性 warning 路径）
gcc -fsyntax-only -I components/board/include components/board/src/birdiebot_board.c
```

### 8.2 目标构建（G0/WP-007，用 Docker IDF 5.2.1）

```bash
scripts/idf-docker.sh -C . set-target esp32s3 && scripts/idf-docker.sh build
scripts/idf-docker.sh -C . set-target esp32c3 && scripts/idf-docker.sh build
scripts/idf-docker.sh -C . set-target esp32     && scripts/idf-docker.sh build
```

### 8.3 约束

- 本组件只提供**编译期常量**，不做运行时 pin 重映射、不持有 GPIO 状态。
- `boards/*.h` 只允许写引脚/身份宏，不得 include 其他项目头文件（保持自洽）。
- 禁止再引入 ESP8266（已从项目删除）；也不新增 Flash 复用脚（GPIO6~11 在
  ESP32/ESP8266 上为 Flash 专用，旧 fork 的 `PIN_LED_* 10/11` 即反例）。
- 板引脚变更只改 `boards/*.h` + 本表，改完跑 §8.1/§8.2。

---

## 9. 变更记录

| 日期 | 变更 | 作者 |
|---|---|---|
| 2026-10-20 | WP-006 初版：4 块板（zxr_dap_s3 默认 + 3 通用）、Kconfig 选择、引脚/默认值契约 | board-scaffold |

### WiFi 状态灯

`BIRDIEBOT_BOARD_HAS_WIFI_LED=1` 的板子在 **STA 获取 IP 后**把 `PIN_LED_WIFI_STATUS` 拉高点亮（ZXR DAP S3 = GPIO4），STA 断开时拉低。实现位于 `project_components/wifi_manager/`（`wifi_manager_wifi_led_set`），不占用 Legacy LEDC 通道。
