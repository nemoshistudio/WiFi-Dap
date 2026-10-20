/**
 * @file board_generic_esp32.h
 * @brief Pin map for BOARD_GENERIC_ESP32 - upstream generic ESP32 wiring.
 *
 * Compile-only board in this project (no hardware validation in G0), see
 * docs/board-pins.md. Values match the C-tree ESP32 pin block except for the
 * legacy separate SWDIO input pin: the new single-wire abstraction drives and
 * samples SWDIO on PIN_SWDIO_MOSI (GPIO13); PIN_SWDIO is provided as an alias
 * by birdiebot_board.h. If the old two-wire reference circuit (MISO=12) is
 * used on real hardware, the input path must be reviewed in WP-101.
 *
 * No LEDs on this reference wiring: LED macros use the A-tree "_" placeholder
 * and the HAS_* flags are 0. Always guard LED access with the HAS_* flag.
 */
#ifndef BIRDIEBOT_BOARD_GENERIC_ESP32_H
#define BIRDIEBOT_BOARD_GENERIC_ESP32_H

/* ---- board identity ---- */
#define BIRDIEBOT_BOARD_ID        "generic_esp32"
#define BIRDIEBOT_BOARD_NAME      "Generic ESP32"
#define BIRDIEBOT_BOARD_TARGET    "esp32"
/* 1 = esp32, 2 = esp32c3, 3 = esp32s3: used by the target sanity check */
#define BIRDIEBOT_BOARD_TARGET_ID 1

/* ---- feature flags ---- */
#define BIRDIEBOT_BOARD_HAS_STATUS_LEDS 0
#define BIRDIEBOT_BOARD_HAS_WIFI_LED    0

/* ---- CMSIS-DAP SWD/JTAG pins (A DAP_config.h compatible names) ---- */
#define PIN_SWCLK      14
#define PIN_SWDIO_MOSI 13
#define PIN_TDO        19
#define PIN_TDI        18
#define PIN_nTRST      25
#define PIN_nRESET     26

/* ---- status LEDs: not populated on this board (see file header) ---- */
#define PIN_LED_CONNECTED   _ /* unused */
#define PIN_LED_RUNNING     _ /* unused */
#define PIN_LED_WIFI_STATUS _ /* unused */

/* ---- UART TCP bridge (UART2) ---- */
#define BIRDIEBOT_UART_BRIDGE_UART_NUM 2
#define UART_BRIDGE_TX_PIN 23
#define UART_BRIDGE_RX_PIN 22

#endif /* BIRDIEBOT_BOARD_GENERIC_ESP32_H */
