/*
 * B1 pipeline pure-logic unit test (host build, no ESP-IDF).
 *
 * Simulates the URB stream seen by the firmware and checks the pairing rules:
 *  - ordered OUT commands / IN URBs / DAP responses pairing (FIFO),
 *  - pipelined interleavings (writes then reads, alternating),
 *  - IN ring full / command ring full behaviour,
 *  - UNLINK of a pending IN URB and of a pending OUT command,
 *  - wrap-around of both rings,
 *  - response queued before the IN URB arrives.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "usbip_pipeline_logic.h"

static int checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

/* Simulated session: push commands and check response pairing. */
static usbip_pl_state_t st;

static void reset(void)
{
    usbip_pl_reset(&st);
}

/* Typical OpenOCD pattern: N writes, then N reads, then N responses. */
static int test_writes_then_reads(void)
{
    unsigned slot;
    uint32_t seq;

    reset();
    /* The host submitted OUT 10,11,12 (commands) first. */
    CHECK(usbip_pl_register_cmd(&st, 10));
    CHECK(usbip_pl_register_cmd(&st, 11));
    CHECK(usbip_pl_register_cmd(&st, 12));
    CHECK(usbip_pl_pending_in(&st) == 0);
    CHECK(usbip_pl_pending_resp(&st) == 0);

    /* Responses are produced first (device processed them while the host was
     * still writing): all must be queued. */
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_QUEUE);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_QUEUE);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_QUEUE);
    CHECK(usbip_pl_pending_resp(&st) == 3);

    /* Now the IN URBs arrive: each gets one queued response in order. */
    CHECK(usbip_pl_in(&st, 20, &slot) == USBIP_PL_IN_SEND);
    CHECK(usbip_pl_in(&st, 21, &slot) == USBIP_PL_IN_SEND);
    CHECK(usbip_pl_in(&st, 22, &slot) == USBIP_PL_IN_SEND);
    CHECK(usbip_pl_pending_resp(&st) == 0);
    CHECK(usbip_pl_pending_in(&st) == 0);
    return 0;
}

/* Alternating command/response/IN: every response pairs immediately. */
static int test_ping_pong(void)
{
    unsigned slot;
    uint32_t seq;

    reset();
    CHECK(usbip_pl_register_cmd(&st, 100));
    CHECK(usbip_pl_in(&st, 200, &slot) == USBIP_PL_IN_QUEUE);
    CHECK(usbip_pl_pending_in(&st) == 1);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(seq == 200);
    CHECK(slot == 0);
    CHECK(usbip_pl_pending_in(&st) == 0);
    CHECK(usbip_pl_pending_resp(&st) == 0);
    return 0;
}

/* Interleaved pipeline: IN URBs arrive before responses are produced. */
static int test_reads_before_responses(void)
{
    unsigned slot;
    uint32_t seq;

    reset();
    CHECK(usbip_pl_register_cmd(&st, 1));
    CHECK(usbip_pl_register_cmd(&st, 2));
    CHECK(usbip_pl_register_cmd(&st, 3));
    CHECK(usbip_pl_in(&st, 11, &slot) == USBIP_PL_IN_QUEUE);
    CHECK(slot == 0);
    CHECK(usbip_pl_in(&st, 12, &slot) == USBIP_PL_IN_QUEUE);
    CHECK(slot == 1);
    CHECK(usbip_pl_in(&st, 13, &slot) == USBIP_PL_IN_QUEUE);
    CHECK(slot == 2);

    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(seq == 11 && slot == 0);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(seq == 12 && slot == 1);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(seq == 13 && slot == 2);
    CHECK(usbip_pl_pending_in(&st) == 0);
    return 0;
}

/* Unlink a pending IN: its response must be dropped, later responses stay
 * paired with their own IN URBs (no shift). */
static int test_unlink_pending_in(void)
{
    unsigned slot;
    uint32_t seq;

    reset();
    CHECK(usbip_pl_register_cmd(&st, 1));
    CHECK(usbip_pl_register_cmd(&st, 2));
    CHECK(usbip_pl_in(&st, 11, &slot) == USBIP_PL_IN_QUEUE); /* slot 0 */
    CHECK(usbip_pl_in(&st, 12, &slot) == USBIP_PL_IN_QUEUE); /* slot 1 */

    CHECK(usbip_pl_unlink(&st, 11));
    /* duplicate unlink of a still-pending (cancelled) IN is idempotent */
    CHECK(usbip_pl_unlink(&st, 11));

    /* response #1 pairs with the cancelled slot: dropped */
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_DROP);
    /* response #2 goes to the still-valid IN 12 (slot 1) */
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(seq == 12);
    CHECK(usbip_pl_pending_in(&st) == 0);
    return 0;
}

/* Unlink a pending OUT command: its response is dropped, no IN gets it. */
static int test_unlink_pending_cmd(void)
{
    unsigned slot;
    uint32_t seq;

    reset();
    CHECK(usbip_pl_register_cmd(&st, 1));
    CHECK(usbip_pl_register_cmd(&st, 2));
    CHECK(usbip_pl_in(&st, 11, &slot) == USBIP_PL_IN_QUEUE);
    CHECK(usbip_pl_in(&st, 12, &slot) == USBIP_PL_IN_QUEUE);

    CHECK(usbip_pl_unlink(&st, 1)); /* first OUT command cancelled */
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_DROP);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(seq == 11); /* IN 11 belongs to the second (still valid) command */
    CHECK(usbip_pl_pending_in(&st) == 1);
    CHECK(usbip_pl_unlink(&st, 999) == false);
    return 0;
}

/* Full IN ring: the caller gets USBIP_PL_IN_FULL and can answer with an error
 * RET_SUBMIT; once a response drains a slot, queueing works again. */
static int test_in_ring_full(void)
{
    unsigned slot;
    uint32_t seq;
    unsigned i;

    reset();
    for (i = 0; i < USBIP_PL_MAX_IN; i++) {
        CHECK(usbip_pl_register_cmd(&st, i + 1));
        CHECK(usbip_pl_in(&st, 100 + i, &slot) == USBIP_PL_IN_QUEUE);
        CHECK(slot == i);
    }
    CHECK(usbip_pl_in(&st, 999, &slot) == USBIP_PL_IN_FULL);
    CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
    CHECK(usbip_pl_in(&st, 999, &slot) == USBIP_PL_IN_QUEUE);
    return 0;
}

/* Wrap-around: use more entries than the ring size over time. */
static int test_wrap_around(void)
{
    unsigned slot;
    uint32_t seq;
    unsigned i;

    reset();
    for (i = 0; i < (USBIP_PL_MAX_CMD * 3U); i++) {
        CHECK(usbip_pl_register_cmd(&st, 1000 + i));
        CHECK(usbip_pl_in(&st, 5000 + i, &slot) == USBIP_PL_IN_QUEUE);
        CHECK(usbip_pl_response(&st, &slot, &seq) == USBIP_PL_RESP_SEND);
        CHECK(seq == 5000 + i);
    }
    CHECK(usbip_pl_pending_in(&st) == 0);
    return 0;
}

/* Command ring full: registration must fail instead of overwriting. */
static int test_cmd_ring_full(void)
{
    unsigned i;

    reset();
    for (i = 0; i < USBIP_PL_MAX_CMD; i++) {
        CHECK(usbip_pl_register_cmd(&st, i + 1));
    }
    CHECK(usbip_pl_register_cmd(&st, 999) == false);
    reset();
    CHECK(usbip_pl_register_cmd(&st, 1)); /* reset recovers */
    return 0;
}

int main(void)
{
    checks = 0;
    if (test_writes_then_reads() || test_ping_pong() || test_reads_before_responses() ||
        test_unlink_pending_in() || test_unlink_pending_cmd() ||
        test_in_ring_full() || test_wrap_around() || test_cmd_ring_full()) {
        return 1;
    }
    printf("ok - usbip pipeline logic (%d checks)\n", checks);
    return 0;
}
