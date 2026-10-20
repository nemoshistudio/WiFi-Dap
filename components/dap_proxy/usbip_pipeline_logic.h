#ifndef __USBIP_PIPELINE_LOGIC_H__
#define __USBIP_PIPELINE_LOGIC_H__
/*
 * B1: pure URB pairing state machine for the USBIP multi-URB pipeline.
 *
 * The host (OpenOCD cmsis-dap: up to 3 pending request blocks per
 * MAX_PENDING_REQUESTS) may have several USBIP URBs in flight at the same
 * time.  A CMSIS-DAP transaction consists of an OUT URB (command) followed by
 * an IN URB (response).  The firmware therefore has to pair the *ordered*
 * stream of DAP responses with the *ordered* stream of pending IN URBs while
 * still handling unlinks (aborted URBs) and a full queue.
 *
 * This module contains that logic only: no FreeRTOS/lwIP/DAP dependencies, so
 * it can be compiled and unit-tested on the host (see test/).
 *
 * Rings (FIFO, fixed size, wrap-around):
 *   cmd  : one ticket per SUBMIT/DIR_OUT URB that was pushed into the DAP input
 *          ring; popped one-by-one when the DAP task finishes a response.
 *          "unlinked" marks a ticket whose URB was cancelled by the host: the
 *          response for it must be dropped instead of being sent to an IN URB.
 *   in   : one entry per SUBMIT/DIR_IN URB (response request) that arrived
 *          before its response was ready.  "cancelled" marks an unlinked IN:
 *          its slot is kept so the matching response is dropped, otherwise the
 *          response stream would shift by one and desync every later reply.
 *
 * Actions returned by usbip_pl_response()/usbip_pl_in() are executed by the
 * caller (which owns the ring buffers and the TCP socket).
 */

#include <stdbool.h>
#include <stdint.h>

#ifndef USBIP_PL_MAX_IN
#define USBIP_PL_MAX_IN 8
#endif
#ifndef USBIP_PL_MAX_CMD
#define USBIP_PL_MAX_CMD 24
#endif

typedef enum {
    USBIP_PL_RESP_DROP = 0, /* response belongs to a cancelled URB: discard   */
    USBIP_PL_RESP_SEND,     /* IN URB waiting: send the response now          */
    USBIP_PL_RESP_QUEUE,    /* no IN URB yet: store response for a later IN   */
} usbip_pl_resp_action_t;

typedef enum {
    USBIP_PL_IN_SEND = 0, /* response already stored: reply immediately      */
    USBIP_PL_IN_QUEUE,    /* response not ready: park the IN URB            */
    USBIP_PL_IN_FULL,     /* IN ring full: caller must fail the URB         */
} usbip_pl_in_action_t;

typedef struct {
    uint32_t seq;
    bool cancelled;
} usbip_pl_in_entry_t;

typedef struct {
    uint32_t seq;
    bool unlinked;
} usbip_pl_cmd_entry_t;

typedef struct {
    usbip_pl_in_entry_t in[USBIP_PL_MAX_IN];
    unsigned in_head;
    unsigned in_tail;
    unsigned in_count;

    usbip_pl_cmd_entry_t cmd[USBIP_PL_MAX_CMD];
    unsigned cmd_head;
    unsigned cmd_tail;
    unsigned cmd_count;

    unsigned resp_queued; /* responses stored waiting for an IN URB */
} usbip_pl_state_t;

void usbip_pl_reset(usbip_pl_state_t *st);

/* Register one SUBMIT/DIR_OUT command before it enters the DAP request ring.
 * Returns false when the ticket ring is full (caller must not queue the
 * command / must reset the session). */
bool usbip_pl_register_cmd(usbip_pl_state_t *st, uint32_t seq);

/* The DAP task produced one response.  On USBIP_PL_RESP_SEND, *out_slot is the
 * index of the matching queued IN URB (the caller stored its 48-byte USBIP
 * header at that slot) and *out_seq is its seqnum. */
usbip_pl_resp_action_t usbip_pl_response(usbip_pl_state_t *st, unsigned *out_slot,
                                         uint32_t *out_seq);

/* A SUBMIT/DIR_IN URB arrived.  On USBIP_PL_IN_QUEUE, *out_slot is the slot the
 * caller must store the raw 48-byte USBIP header in.  On USBIP_PL_IN_SEND the
 * caller immediately sends the next stored response to this URB. */
usbip_pl_in_action_t usbip_pl_in(usbip_pl_state_t *st, uint32_t seq,
                                 unsigned *out_slot);

/* Host cancelled the URB with seqnum `seq` (USBIP_CMD_UNLINK).  Marks the
 * matching pending IN URB (cancelled) or OUT command (unlinked).  Returns true
 * when the seqnum was still pending (the RET_UNLINK is a real cancellation). */
bool usbip_pl_unlink(usbip_pl_state_t *st, uint32_t seq);

unsigned usbip_pl_pending_in(const usbip_pl_state_t *st);
unsigned usbip_pl_pending_resp(const usbip_pl_state_t *st);

#endif /* __USBIP_PIPELINE_LOGIC_H__ */
