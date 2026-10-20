/**
 * @file board_generic_s3.h
 * @brief Pin map for BOARD_GENERIC_S3 - upstream generic ESP32-S3 wiring.
 *
 * Values match the upstream/C-tree ESP32-S3 wiring (TDO=9, TDI=10) and the
 * first S3 UART bridge attempt of the fork (TX=17, RX=18). No LEDs on this
 * reference wiring, therefore all LED macros are the A-tree "_" placeholder
 * (unused) and the HAS_* flags are 0. Never use a LED macro without
 * checking its HAS_* flag: "_" is an undeclared identifier on purpose so
 * misuse fails at compile time instead of silently driving a random GPIO.
 *
 * Wiring reference and caveats: docs/board-pins.md
 */
#ifndef BIRDIEBOT_BOARD_GENERIC_S3_H
#define BIRDIEBOT_BOARD_GENERIC_S3_H

/* ---- board identity ---- */
#define BIRDIEBOT_BOARD_ID        "generic_s3"
#define BIRDIEBOT_BOARD_NAME      "Generic ESP32-S3"
#define BIRDIEBOT_BOARD_TARGET    "esp32s3"
/* 1 = esp32, 2 = esp32c3, 3 = esp32s3: used by the target sanity check */
#define BIRDIEBOT_BOARD_TARGET_ID 3

/* ---- feature flags ---- */
#define BIRDIEBOT_BOARD_HAS_STATUS_LEDS 0
#define BIRDIEBOT_BOARD_HAS_WIFI_LED    0

/* ---- CMSIS-DAP SWD/JTAG pins (A DAP_config.h compatible names) ---- */
#define PIN_SWCLK      12
#define PIN_SWDIO_MOSI 11
#define PIN_TDO        9
#define PIN_TDI        10
#define PIN_nTRST      14
#define PIN_nRESET     13

/* ---- status LEDs: not populated on this board (see file header) ---- */
#define PIN_LED_CONNECTED   _ /* unused */
#define PIN_LED_RUNNING     _ /* unused */
#define PIN_LED_WIFI_STATUS _ /* unused */

/* ---- UART TCP bridge (UART1) ---- */
#define BIRDIEBOT_UART_BRIDGE_UART_NUM 1
#define UART_BRIDGE_TX_PIN 17
#define UART_BRIDGE_RX_PIN 18

#endif /* BIRDIEBOT_BOARD_GENERIC_S3_H */
