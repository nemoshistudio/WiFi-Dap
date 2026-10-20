/**
 * @file dap_led.c
 * @brief CMSIS-DAP status LED initialization (merged from the A 2025 tree)
 *
 * The fork's 2025 DAP_handle.c grew a dap_led_init() that configured the
 * "Connected" / "Running" LEDs as outputs and switched them off at boot.  The
 * merged tree keeps that behavior but takes the pin numbers from the selected
 * board header (WP-006) instead of hardcoding ESP32-S3 GPIO 5/6:
 *
 *   #if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
 *       GPIO_FUNCTION_SET(PIN_LED_CONNECTED);
 *       ...
 *   #endif
 *
 * Boards without status LEDs define the pins as "_" and set
 * BIRDIEBOT_BOARD_HAS_STATUS_LEDS = 0, so the GPIO macros are never expanded
 * with "_" (that failure mode broke the ESP32-C3 target in the fork).
 *
 * The file is intentionally tiny and separate from DAP_handle.c: it includes
 * the board header + gpio_op.h only, not the whole DAP_config.h, so it stays
 * independent of the CMSIS-DAP configuration port (WP-101/WP-102).
 *
 * @copyright Copyright (c) 2021-2025
 *
 */
#include "sdkconfig.h"

#include "birdiebot_board.h"
#include "cmsis-dap/include/gpio_op.h"

#include "DAP_handle.h"

void dap_led_init(void)
{
#if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
    /* Same sequence as the A-tree dap_led_init(): GPIO function, output
     * direction, then drive both LEDs low (off). */
    GPIO_FUNCTION_SET(PIN_LED_CONNECTED);
    GPIO_FUNCTION_SET(PIN_LED_RUNNING);
    GPIO_SET_DIRECTION_NORMAL_OUT(PIN_LED_CONNECTED);
    GPIO_SET_DIRECTION_NORMAL_OUT(PIN_LED_RUNNING);

    GPIO_SET_LEVEL_LOW(PIN_LED_CONNECTED);
    GPIO_SET_LEVEL_LOW(PIN_LED_RUNNING);
#else
    /* No status LEDs on the selected board (BIRDIEBOT_BOARD_HAS_STATUS_LEDS=0,
     * PIN_LED_* are "_"): nothing to initialize. */
#endif
}
