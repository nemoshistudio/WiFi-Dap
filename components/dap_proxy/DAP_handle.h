#ifndef __DAP_HANDLE_H__
#define __DAP_HANDLE_H__

#include <stdint.h>

#include "components/USBIP/usbip_defs.h"

enum reset_handle_t
{
    NO_SIGNAL = 0,
    RESET_HANDLE = 1,
    DELETE_HANDLE = 2,
};

/*
 * Ring buffer ownership (WP-112)
 * ------------------------------
 * The DAP task (DAP_Thread) is the single owner of the DAP ring buffers:
 * free_dap_ringbuf()/malloc_dap_ringbuf() are only executed from it.
 * Other tasks post a request through dap_handle_request_reset() instead of
 * touching the ring buffers:
 *
 *   RESET_HANDLE  - free and recreate both ring buffers (USBIP session end or
 *                   transition back from a WebSocket/elaphureLink session).
 *   DELETE_HANDLE - free only; the caller (WebSocket/elaphureLink) runs DAP
 *                   commands inline in the TCP task and must not race with the
 *                   USBIP data path.
 *
 * A caller that is about to use DAP_ExecuteCommand() itself must wait until the
 * request has been applied with dap_handle_wait_reset() (bounded timeout).
 */
void dap_handle_request_reset(enum reset_handle_t mode);

/* Returns 0 when the last posted reset request was applied (or the DAP task has
 * not been started yet), -1 on timeout. */
int dap_handle_wait_reset(uint32_t timeout_ms);

/* A 2025: CMSIS-DAP status LED initialization (board pins from the board
 * component, no-op on boards without status LEDs). */
void dap_led_init(void);

void handle_dap_data_request(usbip_stage2_header *header, uint32_t length);
void handle_swo_trace_response(usbip_stage2_header *header);
void handle_dap_unlink();

int fast_reply(uint8_t *buf, uint32_t length, int dap_req_num);
void DAP_Thread(void *argument);

/* Kept for elaphureLink_protocol.c and legacy callers; the DAP task is the
 * only task that should call them directly. */
void malloc_dap_ringbuf(void);
void free_dap_ringbuf(void);

#endif
