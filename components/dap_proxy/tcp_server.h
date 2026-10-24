#ifndef __TCP_SERVER_H__
#define __TCP_SERVER_H__

#include <stdbool.h>

void tcp_server_task(void *pvParameters);

/**
 * @brief Read-only query: is a DAP proxy session currently accepted/active?
 *
 * Returns true while @c kSock >= 0, i.e. a USBIP / elaphureLink / WebSocket-DAP
 * client has an accepted socket.  The query only reads the session variable; it
 * never kicks, closes or otherwise perturbs the session (use
 * tcp_server_kick_session() for that).  Safe to call from the HTTP task; used
 * by GET /api/target/status to expose @c dap_session_active for diagnostics.
 *
 * @note internal integration hook; not part of the USBIP wire protocol.
 */
bool tcp_server_session_active(void);

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