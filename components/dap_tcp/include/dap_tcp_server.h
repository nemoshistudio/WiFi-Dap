/*
 * SPDX-License-Identifier: MIT
 *
 * CMSIS-DAP over TCP server skeleton (M3).
 *
 * Listens on TCP 3241 and serves ONE debugger at a time with the CMSIS-DAP
 * TCP frame format used by OpenOCD's `cmsis-dap backend tcp` and the XRDAP
 * probes (see dap_tcp_frame.h and docs/dap-tcp-backend.md).
 *
 * SKELETON: this component deliberately does not own the CMSIS-DAP engine.
 * Every request frame is handed to the weak hook birdiebot_dap_tcp_execute().
 * The default implementation reports "not wired" and the server closes the
 * connection (a debugger would time out anyway; a clean close is honest and
 * does not touch the DAP state machine).  The integration that wires the DAP
 * engine is described in docs/dap-tcp-backend.md section 7.
 *
 * CONFIG_BIRDIEBOT_DAP_TCP_ENABLE is off by default.  When it is off, this
 * component compiles to the stubs below: nothing listens on the port and the
 * dap_tcp_server_*() calls are safe no-ops, so the caller does not need
 * preprocessor guards around the call sites.
 */
#ifndef DAP_TCP_SERVER_H
#define DAP_TCP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default listening port (XRDAP probes, OpenOCD `cmsis-dap tcp port`). */
#define DAP_TCP_DEFAULT_PORT 3241u

/**
 * @brief Weak hook: execute one CMSIS-DAP request packet.
 *
 * Called from the DAP TCP server task, one request at a time, in wire order.
 * The response must be the CMSIS-DAP response packet for @p request (the
 * command echo byte included), exactly as DAP_ExecuteCommand() produces it.
 *
 * Contract:
 *  - @p request has @p request_len bytes, 1..DAP_TCP_MAX_PAYLOAD (1024).
 *  - @p response has @p response_cap bytes (DAP_TCP_MAX_PAYLOAD).  Never write
 *    more than @p response_cap; the frame header is added in front of it.
 *  - return 0 and *response_len = 0 for a command with no response
 *    (e.g. DAP_TransferAbort 0x07): no TCP frame is sent.
 *  - return 0 and *response_len = 1..response_cap: a type-2 response frame is
 *    sent.
 *  - return non-zero when the request cannot be handled: the server logs and
 *    closes the connection.  The default weak implementation returns -1.
 *
 * The hook must run the engine with exclusive DAP ownership; the server does
 * not arbitrate between transports (XRDAP-Core rule "one link at a time").
 *
 * @param request       received CMSIS-DAP request packet.
 * @param request_len   its length in bytes.
 * @param response      response buffer to fill.
 * @param response_cap  capacity of @p response.
 * @param response_len  set to the response length (0 = no response frame).
 * @return 0 on success, non-zero when not handled.
 */
int birdiebot_dap_tcp_execute(const uint8_t *request, uint16_t request_len,
                              uint8_t *response, uint16_t response_cap,
                              uint16_t *response_len);

/**
 * @brief Weak hook: link state of the single TCP client.
 *
 * Called from the server task after accept() with @p connected = true and
 * after the connection is closed (or rejected) with @p connected = false.
 * The integration uses it to take and release exclusive DAP ownership
 * (docs/dap-tcp-backend.md section 7.2).  The default implementation is a
 * no-op.
 *
 * @param connected true when a debugger session starts, false when it ends.
 */
void birdiebot_dap_tcp_link_changed(bool connected);

/**
 * @brief Start the server task (idempotent).
 *
 * Creates the listening task once.  It binds the port as soon as the network
 * stack is up and retries every 2 s while the bind fails, so it can be called
 * before the Wi-Fi interface has an address.  When
 * CONFIG_BIRDIEBOT_DAP_TCP_ENABLE is off this returns ESP_ERR_NOT_SUPPORTED
 * and nothing is created.
 *
 * @return ESP_OK when the task is running (or was already started),
 *         ESP_ERR_NOT_SUPPORTED when the backend is compiled out,
 *         ESP_ERR_NO_MEM when the task could not be created.
 */
esp_err_t dap_tcp_server_start(void);

/**
 * @brief Is the server task running?
 *
 * @return true while the listening task exists (feature on and started).
 */
bool dap_tcp_server_running(void);

/**
 * @brief Is a debugger connected right now?
 *
 * Read-only; safe to call from another task (HTTP diagnostics).
 *
 * @return true between accept() and the session close.
 */
bool dap_tcp_server_client_connected(void);

/**
 * @brief Configured listening port.
 *
 * @return CONFIG_BIRDIEBOT_DAP_TCP_PORT when the feature is on, 0 when it is
 *         compiled out.
 */
uint16_t dap_tcp_server_port(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DAP_TCP_SERVER_H */
