#ifndef __DAP_HANDLE_H__
#define __DAP_HANDLE_H__

#include <stdbool.h>
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

/* Submit one USBIP OUT URB payload to the DAP task.
 * length = number of payload bytes actually received for this URB (validated
 * by usbip_server.c, <= DAP_PACKET_SIZE).  The fixed-size DAP packet is
 * zero-filled behind `length` so stale bytes from a previous URB/session can
 * never reach DAP_ProcessCommand (docs/security-review-dap.md N8). */
void handle_dap_data_request(usbip_stage2_header *header, uint32_t length);

/* B1 pipeline helpers (usbip_pipeline.c): move a finished DAP response between
 * the ring buffer and the USBIP pipeline without exposing the internal
 * DapPacket_t layout. Both functions are only valid while a USBIP session owns
 * the DAP data path. */
bool dap_take_response(uint8_t *buf, uint32_t buf_size, uint32_t *length);
bool dap_push_response_data(const uint8_t *data, uint32_t length);
void handle_swo_trace_response(usbip_stage2_header *header);
void handle_dap_unlink();

int fast_reply(uint8_t *buf, uint32_t length, int dap_req_num);
void DAP_Thread(void *argument);

/* Kept for elaphureLink_protocol.c and legacy callers; the DAP task is the
 * only task that should call them directly. */
void malloc_dap_ringbuf(void);
void free_dap_ringbuf(void);

#endif
