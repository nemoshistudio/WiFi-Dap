/*
 * B1: pure URB pairing state machine for the USBIP multi-URB pipeline.
 * No FreeRTOS/lwIP/DAP dependencies: unit-tested on the host (test/).
 */
#include <string.h>

#include "usbip_pipeline_logic.h"

void usbip_pl_reset(usbip_pl_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

bool usbip_pl_register_cmd(usbip_pl_state_t *st, uint32_t seq)
{
    if (st->cmd_count >= USBIP_PL_MAX_CMD) {
        return false;
    }

    st->cmd[st->cmd_tail].seq = seq;
    st->cmd[st->cmd_tail].unlinked = false;
    st->cmd_tail = (st->cmd_tail + 1U) % USBIP_PL_MAX_CMD;
    st->cmd_count++;
    return true;
}

usbip_pl_resp_action_t usbip_pl_response(usbip_pl_state_t *st, unsigned *out_slot,
                                         uint32_t *out_seq)
{
    bool unlinked;

    if (out_slot) {
        *out_slot = 0U;
    }
    if (out_seq) {
        *out_seq = 0U;
    }

    /* No command ticket: the response cannot be paired reliably (should not
     * happen for a CMSIS-DAP command response).  Queue it so it is not lost. */
    if (st->cmd_count == 0U) {
        st->resp_queued++;
        return USBIP_PL_RESP_QUEUE;
    }

    unlinked = st->cmd[st->cmd_head].unlinked;
    st->cmd_head = (st->cmd_head + 1U) % USBIP_PL_MAX_CMD;
    st->cmd_count--;

    if (unlinked) {
        /* The OUT URB was cancelled: its response must never be sent. */
        return USBIP_PL_RESP_DROP;
    }

    if (st->in_count == 0U) {
        st->resp_queued++;
        return USBIP_PL_RESP_QUEUE;
    }

    {
        unsigned slot = st->in_head;
        bool cancelled = st->in[slot].cancelled;
        uint32_t seq = st->in[slot].seq;

        st->in_head = (st->in_head + 1U) % USBIP_PL_MAX_IN;
        st->in_count--;

        if (cancelled) {
            /* The IN URB was unlinked: drop exactly one response for it. */
            return USBIP_PL_RESP_DROP;
        }

        if (out_slot) {
            *out_slot = slot;
        }
        if (out_seq) {
            *out_seq = seq;
        }
        return USBIP_PL_RESP_SEND;
    }
}

usbip_pl_in_action_t usbip_pl_in(usbip_pl_state_t *st, uint32_t seq,
                                 unsigned *out_slot)
{
    if (out_slot) {
        *out_slot = 0U;
    }

    /* A stray response is already waiting: pair it with this IN URB now. */
    if (st->resp_queued > 0U) {
        st->resp_queued--;
        return USBIP_PL_IN_SEND;
    }

    if (st->in_count >= USBIP_PL_MAX_IN) {
        return USBIP_PL_IN_FULL;
    }

    if (out_slot) {
        *out_slot = st->in_tail;
    }
    st->in[st->in_tail].seq = seq;
    st->in[st->in_tail].cancelled = false;
    st->in_tail = (st->in_tail + 1U) % USBIP_PL_MAX_IN;
    st->in_count++;
    return USBIP_PL_IN_QUEUE;
}

bool usbip_pl_unlink(usbip_pl_state_t *st, uint32_t seq)
{
    unsigned i;

    /* Pending IN URB: keep the slot (mark cancelled) so the response stream
     * stays aligned; the response for it will be dropped. */
    for (i = 0; i < st->in_count; i++) {
        unsigned slot = (st->in_head + i) % USBIP_PL_MAX_IN;
        if (st->in[slot].seq == seq) {
            st->in[slot].cancelled = true;
            return true;
        }
    }

    /* Pending OUT command: its response must be dropped when it completes. */
    for (i = 0; i < st->cmd_count; i++) {
        unsigned slot = (st->cmd_head + i) % USBIP_PL_MAX_CMD;
        if (st->cmd[slot].seq == seq) {
            st->cmd[slot].unlinked = true;
            return true;
        }
    }

    return false;
}

unsigned usbip_pl_pending_in(const usbip_pl_state_t *st)
{
    return st->in_count;
}

unsigned usbip_pl_pending_resp(const usbip_pl_state_t *st)
{
    return st->resp_queued;
}

bool usbip_pl_idle(const usbip_pl_state_t *st)
{
    return (st->cmd_count == 0U) && (st->resp_queued == 0U) && (st->in_count == 0U);
}
