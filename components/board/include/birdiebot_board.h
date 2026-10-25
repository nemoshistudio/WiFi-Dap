/**
 * @file birdiebot_board.h
 * @brief WiFi Dap board abstraction - single source of truth for
 *        SWD/JTAG, UART-bridge and status-LED pins (WP-006).
 *
 * Board selection is done through Kconfig:
 *   idf.py menuconfig -> "WiFi Dap board configuration" -> "Target board"
 *   default: BIRDIEBOT_BOARD_ZXR_DAP_S3 (ESP32-S3 WiFi Dap)
 *
 * The selected board file (components/board/boards/board_<name>.h) defines the
 * actual pin numbers. All PIN_* names are kept compatible with the old
 * components/DAP/config/DAP_config.h macros from the wireless-esp8266-dap
 * fork, so the IDF5 DAP_config port (WP-101) and the UART TCP bridge port
 * (WP-114) can consume them without renaming:
 *   PIN_SWCLK, PIN_SWDIO_MOSI, PIN_SWDIO (optional alias), PIN_TDO, PIN_TDI,
 *   PIN_nTRST, PIN_nRESET, PIN_LED_CONNECTED, PIN_LED_RUNNING,
 *   PIN_LED_WIFI_STATUS, UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN,
 *   BIRDIEBOT_UART_BRIDGE_UART_NUM
 *
 * Project-wide defaults (not per board, no Kconfig needed yet):
 *   BIRDIEBOT_UART_BRIDGE_PORT   1346
 *   UART_BRIDGE_BAUDRATE         115200
 *   BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ 2000000 (2MHz, see docs/board-pins.md)
 *
 * The header is self-contained: it has no include dependencies besides an
 * optional "sdkconfig.h" (detected with __has_include). Without an IDF build
 * it falls back to the default board, which keeps host syntax checks and unit
 * tests working:
 *   gcc -fsyntax-only -x c -I components/board/include - <<'EOF'
 *   #include "birdiebot_board.h"
 *   EOF
 *
 * LED macros on boards without LEDs are the A-tree "_" (unused) placeholder.
 * ALWAYS guard LED access with the per-board flags below, otherwise the build
 * fails with an undeclared "_" (intentional: it is the same failure mode that
 * broke the ESP32-C3 target in the fork):
 *   #if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
 *     GPIO_SET_LEVEL_HIGH(PIN_LED_CONNECTED);
 *   #endif
 *
 * Pin tables, caveats and the "add a new board" checklist: docs/board-pins.md
 */
#ifndef BIRDIEBOT_BOARD_H
#define BIRDIEBOT_BOARD_H

/* ------------------------------------------------------------------------- */
/* Board selection (Kconfig)                                                 */
/* ------------------------------------------------------------------------- */
#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

#if defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_S3)
#  include "../boards/board_generic_s3.h"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_C3)
#  include "../boards/board_generic_c3.h"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_ESP32)
#  include "../boards/board_generic_esp32.h"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_C5)
#  include "../boards/board_generic_c5.h"
#else
/*
 * CONFIG_BIRDIEBOT_BOARD_ZXR_DAP_S3 (Kconfig default) or no sdkconfig.h at
 * all (host syntax check). Both cases use the default board.
 */
#  include "../boards/board_zxr_dap_s3.h"
#endif

/* ------------------------------------------------------------------------- */
/* Contract sanity checks - fail early with a readable error                  */
/* ------------------------------------------------------------------------- */
#ifndef BIRDIEBOT_BOARD_ID
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_BOARD_ID"
#endif
#ifndef BIRDIEBOT_BOARD_NAME
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_BOARD_NAME"
#endif
#ifndef BIRDIEBOT_BOARD_TARGET
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_BOARD_TARGET"
#endif
#ifndef BIRDIEBOT_BOARD_TARGET_ID
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_BOARD_TARGET_ID"
#endif
#ifndef BIRDIEBOT_BOARD_HAS_STATUS_LEDS
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_BOARD_HAS_STATUS_LEDS"
#endif
#ifndef BIRDIEBOT_BOARD_HAS_WIFI_LED
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_BOARD_HAS_WIFI_LED"
#endif

#ifndef PIN_SWCLK
#  error "birdiebot_board.h: selected board file does not define PIN_SWCLK"
#endif
#ifndef PIN_SWDIO_MOSI
#  error "birdiebot_board.h: selected board file does not define PIN_SWDIO_MOSI"
#endif
#ifndef PIN_TDO
#  error "birdiebot_board.h: selected board file does not define PIN_TDO"
#endif
#ifndef PIN_TDI
#  error "birdiebot_board.h: selected board file does not define PIN_TDI"
#endif
#ifndef PIN_nTRST
#  error "birdiebot_board.h: selected board file does not define PIN_nTRST"
#endif
#ifndef PIN_nRESET
#  error "birdiebot_board.h: selected board file does not define PIN_nRESET"
#endif

#if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
#  if !defined(PIN_LED_CONNECTED) || !defined(PIN_LED_RUNNING)
#    error "birdiebot_board.h: BIRDIEBOT_BOARD_HAS_STATUS_LEDS=1 requires PIN_LED_CONNECTED and PIN_LED_RUNNING"
#  endif
#endif
#if BIRDIEBOT_BOARD_HAS_WIFI_LED
#  if !defined(PIN_LED_WIFI_STATUS)
#    error "birdiebot_board.h: BIRDIEBOT_BOARD_HAS_WIFI_LED=1 requires PIN_LED_WIFI_STATUS"
#  endif
#endif

#ifndef BIRDIEBOT_UART_BRIDGE_UART_NUM
#  error "birdiebot_board.h: selected board file does not define BIRDIEBOT_UART_BRIDGE_UART_NUM"
#endif
#ifndef UART_BRIDGE_TX_PIN
#  error "birdiebot_board.h: selected board file does not define UART_BRIDGE_TX_PIN"
#endif
#ifndef UART_BRIDGE_RX_PIN
#  error "birdiebot_board.h: selected board file does not define UART_BRIDGE_RX_PIN"
#endif

/* ------------------------------------------------------------------------- */
/* A-tree DAP_config.h compatibility                                         */
/* ------------------------------------------------------------------------- */
/*
 * The old fork defined PIN_SWDIO for the ESP8266/ESP32 two-wire reference
 * circuit (SPI MISO + MOSI). The new architecture samples and drives SWDIO on
 * the single PIN_SWDIO_MOSI pin (see components/DAP/DAP_config.h in the C
 * tree), so PIN_SWDIO is only an alias here. A board that really has a
 * separate input pin may pre-define PIN_SWDIO in its own board file (this
 * #ifndef then keeps it).
 */
#ifndef PIN_SWDIO
#  define PIN_SWDIO PIN_SWDIO_MOSI
#endif

/* ------------------------------------------------------------------------- */
/* Project-wide defaults                                                     */
/* ------------------------------------------------------------------------- */
/* UART TCP bridge (A: main/uart_bridge.c, port 1346 since fork commit 7b4168c) */
#ifndef BIRDIEBOT_UART_BRIDGE_PORT
#  define BIRDIEBOT_UART_BRIDGE_PORT 1346
#endif
#ifndef UART_BRIDGE_BAUDRATE
#  define UART_BRIDGE_BAUDRATE 115200
#endif

/* DAP SWD clock contract: project default is 2MHz (hardware-verified stable
 * on STM32F407; 4MHz/10MHz give no extra throughput because USBIP/WiFi latency
 * dominates). WP-101 maps this to DAP_DEFAULT_SWJ_CLOCK. */
#ifndef BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ
#  define BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ 2000000
#endif

/* ------------------------------------------------------------------------- */
/* Board identity strings (defined in src/birdiebot_board.c)                  */
/* ------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

extern const char birdiebot_board_id[];   /* e.g. "zxr_dap_s3" */
extern const char birdiebot_board_name[]; /* e.g. "ZXR DAP S3" */

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_BOARD_H */
