/**
 * @file board_generic_c5.h
 * @brief Pin map for BOARD_GENERIC_C5 - compile-only ESP32-C5 wiring.
 *
 * Compile-only board in this project (no C5 hardware validation in this
 * branch), see docs/board-pins.md and docs/esp32-c5-port.md. Values follow the
 * C5 SPI2 IO_MUX pin assignment (datasheet Table 2-8 / soc spi_pins.h):
 *
 *   SWCLK  = GPIO6  (SPI2 FSPICLK, P1 - required by the optional SPI backend)
 *   SWDIO  = GPIO7  (SPI2 FSPID,   P1 - required by the optional SPI backend)
 *   TDO    = GPIO8  (free P2 GPIO)
 *   TDI    = GPIO9  (free P2 GPIO)
 *   nTRST  = GPIO3  (C5 own JTAG MTDI; free when C5 is debugged via its
 *                    built-in USB-Serial/JTAG, which is the default)
 *   nRESET = GPIO10 (SPI2 FSPICS0, usable as plain GPIO)
 *
 * No LEDs on this reference wiring: LED macros use the A-tree "_" placeholder
 * and the HAS_* flags are 0. Always guard LED access with the HAS_* flag.
 *
 * Avoided pins: GPIO13/14 (USB Serial/JTAG), GPIO15-18/20-22 (SPI0/1
 * flash/PSRAM), GPIO27 (VDD_SPI on QFN48) and GPIO2/7/25/26/27/28 strapping
 * pins where a different choice is possible (GPIO7 is required for SWDIO and
 * is sampled as a strapping pin at reset; see docs/esp32-c5-port.md).
 */
#ifndef BIRDIEBOT_BOARD_GENERIC_C5_H
#define BIRDIEBOT_BOARD_GENERIC_C5_H

/* ---- board identity ---- */
#define BIRDIEBOT_BOARD_ID        "generic_c5"
#define BIRDIEBOT_BOARD_NAME      "Generic ESP32-C5"
#define BIRDIEBOT_BOARD_TARGET    "esp32c5"
/* 1 = esp32, 2 = esp32c3, 3 = esp32s3, 4 = esp32c5: target sanity check */
#define BIRDIEBOT_BOARD_TARGET_ID 4

/* ---- feature flags ---- */
#define BIRDIEBOT_BOARD_HAS_STATUS_LEDS 0
#define BIRDIEBOT_BOARD_HAS_WIFI_LED    0

/* ---- CMSIS-DAP SWD/JTAG pins (A DAP_config.h compatible names) ---- */
#define PIN_SWCLK      6
#define PIN_SWDIO_MOSI 7
#define PIN_TDO        8
#define PIN_TDI        9
#define PIN_nTRST      3
#define PIN_nRESET     10

/* ---- status LEDs: not populated on this reference wiring (compile-only) ---- */
#define PIN_LED_CONNECTED   _ /* unused */
#define PIN_LED_RUNNING     _ /* unused */
#define PIN_LED_WIFI_STATUS _ /* unused */

/* ---- UART TCP bridge (UART1) ---- */
#define BIRDIEBOT_UART_BRIDGE_UART_NUM 1
#define UART_BRIDGE_TX_PIN 23
#define UART_BRIDGE_RX_PIN 24

#endif /* BIRDIEBOT_BOARD_GENERIC_C5_H */
