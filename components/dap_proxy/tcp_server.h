#ifndef __TCP_SERVER_H__
#define __TCP_SERVER_H__

void tcp_server_task(void *pvParameters);

/**
 * @brief Drop the currently active DAP proxy session (USBIP / elaphureLink /
 *        WS-DAP) so another browser-side client can take over the DAP.
 *
 * Called from the HTTP layer (POST /api/dap/kick, used by the Web UI variable
 * curve page before it opens a WS-DAP connection).  Kicking an idle or absent
 * session is a no-op.  The accepted-socket loop then returns to accept() and
 * the DAP task applies its normal RESET hand-off.
 *
 * @note internal integration hook; not part of the USBIP wire protocol.
 */
void tcp_server_kick_session(void);

#endif