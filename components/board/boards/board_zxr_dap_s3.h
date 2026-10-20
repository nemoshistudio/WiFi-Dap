/**
 * @file board_zxr_dap_s3.h
 * @brief Pin map for BOARD_ZXR_DAP_S3 - default WiFi Dap board.
 *
 * MCU: ESP32-S3 (8MB flash, dual OTA layout reserved by WP-007).
 * Select with: idf.py menuconfig -> "WiFi Dap board configuration" -> "Target board".
 * Wiring reference and caveats: docs/board-pins.md
 *
 * All PIN_* names are kept compatible with the old
 * components/DAP/config/DAP_config.h macros from the wireless-esp8266-dap
 * fork (A tree), so the IDF5 DAP_config can consume them unchanged.
 */
#ifndef BIRDIEBOT_BOARD_ZXR_DAP_S3_H
#define BIRDIEBOT_BOARD_ZXR_DAP_S3_H

/* ---- board identity ---- */
#define BIRDIEBOT_BOARD_ID        "zxr_dap_s3"
#define BIRDIEBOT_BOARD_NAME      "ZXR DAP S3"
#define BIRDIEBOT_BOARD_TARGET    "esp32s3"
/* 1 = esp32, 2 = esp32c3, 3 = esp32s3: used by the target sanity check */
#define BIRDIEBOT_BOARD_TARGET_ID 3

/* ---- feature flags ---- */
#define BIRDIEBOT_BOARD_HAS_STATUS_LEDS 1
#define BIRDIEBOT_BOARD_HAS_WIFI_LED    1

/* ---- CMSIS-DAP SWD/JTAG pins (A DAP_config.h compatible names) ---- */
#define PIN_SWCLK      12 /* TCK / SWCLK */
#define PIN_SWDIO_MOSI 11 /* SWDIO (single-wire: output and input) */
#define PIN_TDO        15 /* device TDO -> host data input */
#define PIN_TDI        16
#define PIN_nTRST      14 /* optional */
#define PIN_nRESET     13

/* ---- status LEDs (only this board has them) ---- */
#define PIN_LED_CONNECTED   5
#define PIN_LED_RUNNING     6
#define PIN_LED_WIFI_STATUS 4

/* ---- UART TCP bridge (dedicated UART1; console stays on UART0) ---- */
#define BIRDIEBOT_UART_BRIDGE_UART_NUM 1
#define UART_BRIDGE_TX_PIN 10
#define UART_BRIDGE_RX_PIN 9

#endif /* BIRDIEBOT_BOARD_ZXR_DAP_S3_H */
