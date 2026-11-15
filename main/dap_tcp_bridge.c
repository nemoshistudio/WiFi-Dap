/*
 * SPDX-License-Identifier: MIT
 *
 * DAP executor bridge for the CMSIS-DAP over TCP backend (M3).
 *
 * The transport component (components/dap_tcp) intentionally implements only
 * framing/session handling and exposes two weak hooks:
 *   - birdiebot_dap_tcp_link_changed(bool): acquire/release exclusive DAP use
 *   - birdiebot_dap_tcp_execute(...):       run one CMSIS-DAP request inline
 *
 * This file provides the strong definitions used when
 * CONFIG_BIRDIEBOT_DAP_TCP_ENABLE=y (default off).  Ownership model follows the
 * existing WS-DAP / elaphureLink hand-off (components/dap_proxy/websocket_server.c):
 * on connect post DELETE_HANDLE and wait for the DAP task to detach the USBIP
 * data path; run DAP_ExecuteCommand() only after that succeeded; on disconnect
 * post RESET_HANDLE to restore the USBIP data path.
 *
 * dap_tcp_bridge_init() is called from app_main() before the TCP server starts.
 * Its only purpose is to make main.o reference this object: ESP-IDF archives
 * components as static libraries and the linker would otherwise drop an
 * unreferenced object, silently falling back to the weak stub hooks.
 */

#include "dap_tcp_bridge.h"

#include <sdkconfig.h>

#if defined(CONFIG_BIRDIEBOT_DAP_TCP_ENABLE) && CONFIG_BIRDIEBOT_DAP_TCP_ENABLE

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#include "DAP_handle.h"
#include "cmsis-dap/include/DAP.h"
#include "dap_tcp_server.h"
#include "main/dap_configuration.h"

static const char *TAG = "dap_tcp_bridge";

/* Bounded wait for the DAP task to acknowledge the ownership hand-off.  On
 * timeout the DAP task may still be executing a USBIP request, so running a
 * DAP command inline would race on the shared DAP_Data/GPIO/SPI state; the
 * session is then refused instead of "continuing anyway" (same rule as N7 for
 * WS-DAP). */
#define DAP_TCP_BRIDGE_RESET_WAIT_MS 1000U

/* The bridge passes the engine response buffer straight through, so the engine
 * packet size must not exceed what OpenOCD's TCP backend accepts.  The
 * configuration layer already forces 1024 when the TCP backend is enabled; this
 * guard catches a future reordering of that logic at compile time. */
#if (DAP_PACKET_SIZE > 1024U)
#error "BIRDIEBOT_DAP_TCP_ENABLE requires DAP_PACKET_SIZE <= 1024 (OpenOCD TCP backend)"
#endif

static uint8_t s_response[DAP_PACKET_SIZE];
static volatile bool s_session_owned;

void birdiebot_dap_tcp_link_changed(bool connected)
{
    if (connected) {
        s_session_owned = false;
        dap_handle_request_reset(DELETE_HANDLE);
        if (dap_handle_wait_reset(DAP_TCP_BRIDGE_RESET_WAIT_MS) != 0) {
            ESP_LOGE(TAG,
                     "DAP task did not release the USBIP data path within %u ms; "
                     "refusing the TCP session",
                     (unsigned)DAP_TCP_BRIDGE_RESET_WAIT_MS);
            return;
        }
        s_session_owned = true;
        ESP_LOGI(TAG, "DAP ownership acquired for CMSIS-DAP over TCP");
    } else {
        s_session_owned = false;
        dap_handle_request_reset(RESET_HANDLE);
        (void)dap_handle_wait_reset(DAP_TCP_BRIDGE_RESET_WAIT_MS);
        ESP_LOGI(TAG, "DAP ownership released; USBIP data path restored");
    }
}

int birdiebot_dap_tcp_execute(const uint8_t *request, uint16_t request_len,
                              uint8_t *response, uint16_t response_cap,
                              uint16_t *response_len)
{
    if (!s_session_owned || (request == NULL) || (response == NULL) ||
        (response_len == NULL) || (request_len == 0U) ||
        (request_len > DAP_PACKET_SIZE)) {
        return -1;
    }

    /* DAP_ExecuteCommand() returns (request_len << 16) | response_len, exactly
     * like DAP_ProcessCommand(); existing inline callers (WS-DAP,
     * elaphureLink) mask with 0xFFFF. */
    const uint16_t n =
        (uint16_t)(DAP_ExecuteCommand(request, s_response) & 0xFFFFU);

    /* The engine caps the response at DAP_PACKET_SIZE (<= 1024 in this build);
     * reject instead of truncating if a handler ever violates that contract. */
    if ((n > response_cap) || (n > DAP_PACKET_SIZE)) {
        ESP_LOGE(TAG, "DAP response of %u bytes does not fit the TCP frame",
                 (unsigned)n);
        return -1;
    }
    if (n != 0U) {
        memcpy(response, s_response, n);
    }
    *response_len = n; /* 0 = command without response (e.g. DAP_TransferAbort) */
    return 0;
}

#endif /* CONFIG_BIRDIEBOT_DAP_TCP_ENABLE */

/*
 * Defined for both configurations: app_main() references this symbol so the
 * linker keeps this object (and therefore the strong hooks above) when the
 * feature is enabled.  It has no side effects.
 */
void dap_tcp_bridge_init(void)
{
}
