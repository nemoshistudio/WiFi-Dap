#ifndef _UART_BRIDGE_H_
#define _UART_BRIDGE_H_

/*
 * UART TCP bridge (WP-114 / WP-203)
 * --------------------------------
 * A plain TCP server on port 1346 (default) that forwards bytes to/from the
 * target UART at 115200-8N1.  The first text packet of a new connection may be
 * a decimal baud rate (2-7 digits) that reconfigures the UART.
 *
 * The bridge is a *client* of components/uart_service, which is the single
 * owner of the target UART (pins/port from components/board).  The bridge no
 * longer installs the UART driver itself, so the Web UART (JSON module 4 and
 * the WS binary router) can use the same UART at the same time:
 *   TCP -> uart_service_write()
 *   UART RX -> uart_service RX fan-out -> bridge stream buffer -> TCP
 *
 * Port 1346 is BIRDIEBOT_UART_BRIDGE_PORT from the board contract; the baud and
 * line settings live in the UART service (default 115200-8N1, deprecated A
 * values 1234/74880 are not used).
 *
 * The bridge can be compiled out with CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n
 * (default y); the functions below then exist as no-ops so that the fixed
 * app_main task creation keeps linking, and the UART service is not touched
 * (Web UART keeps working).
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
