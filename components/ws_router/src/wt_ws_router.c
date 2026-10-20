/**
 * @file wt_ws_router.c
 * @brief PLACEHOLDER — implementation owned by Wave2 WP-204 (components/dataflow team).
 *
 * Contract: docs/ws-binary-router.md. Do not implement business logic here;
 * see WP-204 task card. This stub only keeps the component linkable during scaffold.
 */
#include "wt_ws_router.h"

int wt_ws_router_register_rx(uint8_t module, wt_ws_rx_cb_t cb)
{
    (void) module;
    (void) cb;
    return -1;
}

int wt_ws_router_broadcast(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                           const uint8_t *payload, size_t len)
{
    (void) data_type; (void) module; (void) sub_mod; (void) payload; (void) len;
    return -1;
}


int wt_ws_router_set_tx_port(wt_ws_tx_port_t port)
{
    (void) port;
    return -1;
}

int wt_ws_router_rx(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                    const uint8_t *payload, size_t len)
{
    (void) data_type; (void) module; (void) sub_mod; (void) payload; (void) len;
    return -1;
}

size_t wt_ws_router_max_payload(void)
{
    return 0;
}

int wt_ws_router_set_queue_depth(uint8_t depth)
{
    (void) depth;
    return -1;
}
