/*
 * SPDX-License-Identifier: MIT
 *
 * DAP executor bridge for the CMSIS-DAP over TCP backend (M3).
 * See main/dap_tcp_bridge.c for the ownership model; the feature is default
 * off and compiled out when CONFIG_BIRDIEBOT_DAP_TCP_ENABLE is not set.
 */

#ifndef BIRDIEBOT_DAP_TCP_BRIDGE_H
#define BIRDIEBOT_DAP_TCP_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ensure the strong DAP bridge object is linked and ready.
 *
 * Called from app_main() before dap_tcp_server_start().  When the TCP backend
 * is compiled out this is an empty function (zero behavior change).
 */
void dap_tcp_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_DAP_TCP_BRIDGE_H */
