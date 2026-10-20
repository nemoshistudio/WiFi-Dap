#ifndef _UART_BRIDGE_H_
#define _UART_BRIDGE_H_

/*
 * UART TCP bridge (WP-114)
 * ------------------------
 * A 2025 implementation of the fork's main/uart_bridge.c, componentized:
 * a plain TCP server on port 1346 (default) forwards bytes to/from the target
 * UART at 115200-8N1.  The first text packet of a new connection may be a
 * decimal baud rate (2-7 digits) that reconfigures the bridge.
 *
 * UART instance, TX/RX pins, TCP port and baud rate are all project/board
 * contract values from components/board/include/birdiebot_board.h:
 *   BIRDIEBOT_UART_BRIDGE_UART_NUM  UART instance (1 on S3/C3, 2 on ESP32)
 *   UART_BRIDGE_TX_PIN / UART_BRIDGE_RX_PIN
 *   BIRDIEBOT_UART_BRIDGE_PORT      1346  (deprecated A value: 1234)
 *   UART_BRIDGE_BAUDRATE            115200 (deprecated A value: 74880)
 *
 * The bridge can be compiled out with CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n
 * (default y); the functions below then exist as no-ops so that the fixed
 * app_main task creation keeps linking.
 *
 * Security: the bridge is unauthenticated and plain-text.  It is enabled by
 * default for laboratory use only (docs/INTEGRATION-PLAN.md P7.1).
 */

#ifdef __cplusplus
extern "C" {
#endif

void uart_bridge_init(void);
void uart_bridge_close(void);

/* Kept with the original (unprototyped) declaration so that the fixed
 * app_main() can pass it directly to xTaskCreate() as a TaskFunction_t, exactly
 * like the C/A trees did (main/ is owned by a different work package). */
void uart_bridge_task();

#ifdef __cplusplus
}
#endif

#endif
