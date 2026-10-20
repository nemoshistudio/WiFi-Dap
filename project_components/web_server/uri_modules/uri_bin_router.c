/*
 * SPDX-FileCopyrightText: 2026 WiFi Dap contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file uri_bin_router.c
 * @brief WebSocket binary data path — UART module (module 4), WP-204
 *
 * Implements the UART part of docs/ws-binary-router.md and docs/protocol.md §3:
 *
 *   browser binary frame (4B header + payload)
 *     -> uri_ws.c (FIN=1, length checked)
 *     -> wt_ws_router_rx()  -> [this file] UART rx callback
 *     -> uart_service_write()  -> target UART TX
 *
 *   target UART RX
 *     -> uart_service RX fan-out -> [this file] rx subscriber
 *     -> wt_ws_router_broadcast(RAW_BROADCAST, module=4, sub_mod, data, len)
 *     -> all connected WS clients
 *
 * The router core (components/ws_router) is owned by the Web team; this file is
 * a self-registering client only:
 *   - registration happens from a global constructor, i.e. before
 *     start_webserver() as required by the router contract;
 *   - the inbound callback runs in the httpd/WS task context and never blocks
 *     (uart_service_write with timeout 0);
 *   - the outbound path runs in the uart_service RX task context and only
 *     enqueues (wt_ws_router_broadcast never blocks).
 */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#include "wt_ws_router.h"
#include "wt_data_def.h"
#include "uart_service.h"

#define TAG "uri_bin_router"

/* WS -> device: CMSIS-DAP header conventions from docs/protocol.md §3. */
static int uart_bin_router_rx_cb(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                                 const uint8_t *payload, size_t len)
{
    /* The router core already rejects non-RAW client->device frames; keep the
     * checks here as documentation of the module contract. */
    if (data_type != WT_DATA_RAW) {
        return -1;
    }
    if (module != UART_SERVICE_MODULE_ID) {
        return -1;
    }
    if (sub_mod >= (uint8_t)uart_service_instance_count()) {
        return -1;
    }
    if (payload == NULL || len == 0) {
        return 0; /* empty frame: nothing to write, not an error */
    }

    /* Never block the httpd/WS task: timeout 0, backpressure is reported by
     * the return value (uri_ws.c already acknowledged the transport frame). */
    if (uart_service_write(payload, len, 0) != ESP_OK) {
        ESP_LOGW(TAG, "UART TX queue full, dropped %u bytes", (unsigned)len);
        return -1;
    }

    return 0;
}

/* Device -> WS: UART RX is broadcast to every connected client. */
static void uart_bin_router_fwd_to_ws(uint8_t sub_mod, const uint8_t *data, size_t len, void *arg)
{
    (void)arg;

    if (data == NULL || len == 0) {
        return;
    }

    if (wt_ws_router_broadcast(WT_DATA_RAW_BROADCAST, UART_SERVICE_MODULE_ID,
                               sub_mod, data, len) != 0) {
        /* Router queue full: contract allows dropping (UART data is a
         * best-effort stream; the JSON layer remains responsive). */
        ESP_LOGD(TAG, "WS broadcast queue full, dropped %u bytes", (unsigned)len);
    }
}

/**
 * Self-registration (docs/ws-binary-router.md §3.6: registration happens before
 * start_webserver(), i.e. from the global constructor phase).  Both callees are
 * safe before the FreeRTOS scheduler starts:
 *   - wt_ws_router_register_rx() only writes a spinlock-protected table;
 *   - uart_service_subscribe_rx() only writes its subscriber table.
 */
__attribute__((constructor)) static void uart_bin_router_register(void)
{
    if (wt_ws_router_register_rx(UART_SERVICE_MODULE_ID, uart_bin_router_rx_cb) != 0) {
        ESP_LOGE(TAG, "failed to register WS binary rx callback for module %d",
                 UART_SERVICE_MODULE_ID);
    }

    if (uart_service_subscribe_rx(uart_bin_router_fwd_to_ws, NULL) < 0) {
        ESP_LOGE(TAG, "failed to subscribe to uart_service RX");
    }
}
