/*
 * B1: USBIP multi-URB pipeline glue (FreeRTOS/lwIP side).
 *
 * The pairing rules live in usbip_pipeline_logic.c; this file owns the
 * per-session state, the raw 48-byte IN-URB headers, the reply scratch buffer
 * and the interaction with the DAP ring buffer / socket.
 *
 * Locking: a single mutex serializes the pipeline state.  Socket writes are
 * serialized globally by usbip_network_send() (usbip_server.c), which is also
 * used by the TCP task, so replies from the DAP task and ACK/RET messages from
 * the TCP task can never interleave on the TCP stream.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_timer.h"

#include "lwip/sockets.h"

#include "main/dap_configuration.h"
#include "DAP_handle.h"
#include "usbip_server.h"
#include "usbip_pipeline_logic.h"
#include "usbip_pipeline.h"

#if CONFIG_BIRDIEBOT_USBIP_PIPELINE

#define USBIP_PL_HDR_SIZE 48U
/* Header + payload scratch: one RET_SUBMIT with a full DAP response. */
#define USBIP_PL_SCRATCH_SIZE (USBIP_PL_HDR_SIZE + DAP_PACKET_SIZE)

typedef struct {
    uint32_t seq;
    uint8_t hdr[USBIP_PL_HDR_SIZE];
} usbip_pl_in_hdr_t;

static StaticSemaphore_t s_mux_buf;
static SemaphoreHandle_t s_mux = NULL;
static volatile bool s_active = false;

static usbip_pl_state_t s_state;
static usbip_pl_in_hdr_t s_in_hdrs[USBIP_PL_MAX_IN];
static uint8_t s_tx_buf[USBIP_PL_SCRATCH_SIZE];
static uint8_t s_resp_buf[DAP_PACKET_SIZE];

/* session statistics (printed once at session end, useful on hardware) */
static uint32_t s_stat_out;
static uint32_t s_stat_in;
static uint32_t s_stat_send_now;
static uint32_t s_stat_queue_in;
static uint32_t s_stat_queue_resp;
static uint32_t s_stat_drop;
static uint32_t s_stat_full;
static uint32_t s_stat_error;
static uint32_t s_stat_stray;
static uint32_t s_stat_stale;
static int64_t s_last_urb_us;

/* Raw USBIP header: base.command(4) seqnum(4) devid(4) dir(4) ep(4). */
static uint32_t raw_hdr_seqnum(const uint8_t *hdr)
{
    uint32_t seq;
    memcpy(&seq, &hdr[4], sizeof(seq));
    return ntohl(seq);
}

static uint32_t raw_hdr_devid(const uint8_t *hdr)
{
    uint32_t devid;
    memcpy(&devid, &hdr[8], sizeof(devid));
    return devid;
}

bool usbip_pipeline_enabled(void)
{
    return true;
}

bool usbip_pipeline_is_active(void)
{
    return s_active;
}

void usbip_pipeline_session_start(void)
{
    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutexStatic(&s_mux_buf);
    }
    if (s_mux == NULL) {
        printf("USBIP pipeline: cannot create mutex, using legacy path\r\n");
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    usbip_pl_reset(&s_state);
    memset(s_in_hdrs, 0, sizeof(s_in_hdrs));
    s_stat_out = s_stat_in = s_stat_send_now = s_stat_queue_in = 0;
    s_stat_queue_resp = s_stat_drop = s_stat_full = s_stat_error = 0;
    s_stat_stray = 0;
    s_stat_stale = 0;
    s_last_urb_us = esp_timer_get_time();

    /* Drop stale responses from the previous session: the previous TCP
     * connection may have been torn down before the DAP task applied its ring
     * buffer reset, and a stale item would shift the response/IN pairing. */
    {
        uint32_t stale_len = 0;
        while (dap_take_response(s_resp_buf, sizeof(s_resp_buf), &stale_len)) {
            /* drained */
        }
    }

    s_active = true;
    xSemaphoreGive(s_mux);

    printf("USBIP pipeline: active (in_max=%u cmd_max=%u)\r\n",
           (unsigned)USBIP_PL_MAX_IN, (unsigned)USBIP_PL_MAX_CMD);
}

void usbip_pipeline_session_end(void)
{
    if (s_mux == NULL || !s_active) {
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    usbip_pl_reset(&s_state);
    s_active = false;
    xSemaphoreGive(s_mux);

    printf("USBIP pipeline: stats out=%lu in=%lu send_now=%lu queue_in=%lu "
           "queue_resp=%lu drop=%lu ring_full=%lu error=%lu stray=%lu stale=%lu\r\n",
           (unsigned long)s_stat_out, (unsigned long)s_stat_in,
           (unsigned long)s_stat_send_now, (unsigned long)s_stat_queue_in,
           (unsigned long)s_stat_queue_resp, (unsigned long)s_stat_drop,
           (unsigned long)s_stat_full, (unsigned long)s_stat_error,
           (unsigned long)s_stat_stray, (unsigned long)s_stat_stale);
}

/* Drop pipeline leftovers from a previous host session.
 *
 * The USBIP TCP connection lives in the kernel vhci port and survives an
 * OpenOCD restart, so a new openocd process inherits the pairing state of the
 * previous one.  A pending read/write URB that the old process cancelled may
 * leave a parked IN or a stored response behind; the new session's first
 * command would then be paired with that stale entry and time out ("CMD_INFO
 * failed").
 *
 * Invariant: a live CMSIS-DAP host always has at most one round-trip in
 * flight per command, and keeps submitting URBs while a command is outstanding.
 * Therefore a leftover with no URB activity for STALE_IDLE_MS can only belong
 * to a previous session and is safe to discard.  Slow DAP operations are not
 * affected: the host has already submitted the matching IN URB, so the state is
 * paired before the long DAP processing starts.
 */
#define USBIP_PL_STALE_IDLE_US (2000 * 1000) /* > any plausible Wi-Fi RTT */

static void usbip_pipeline_reset_stale_locked(void)
{
    int64_t now = esp_timer_get_time();

    if (usbip_pl_idle(&s_state)) {
        s_last_urb_us = now;
        return;
    }
    if (s_last_urb_us != 0 && (now - s_last_urb_us) < USBIP_PL_STALE_IDLE_US) {
        return;
    }

    usbip_pl_reset(&s_state);
    memset(s_in_hdrs, 0, sizeof(s_in_hdrs));
    {
        uint32_t stale_len = 0;
        while (dap_take_response(s_resp_buf, sizeof(s_resp_buf), &stale_len)) {
            /* drained */
        }
    }
    s_stat_stale++;
    printf("USBIP pipeline: stale session state dropped after idle gap\r\n");
    s_last_urb_us = now;
}

static void usbip_pipeline_touch_locked(void)
{
    s_last_urb_us = esp_timer_get_time();
}

void usbip_pipeline_register_out(uint32_t seqnum)
{
    bool ok;

    if (!s_active || s_mux == NULL) {
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    usbip_pipeline_reset_stale_locked();
    ok = usbip_pl_register_cmd(&s_state, seqnum);
    s_stat_out++;
    usbip_pipeline_touch_locked();
    if (!ok) {
        /* Should not happen with the OpenOCD queue depth (3); if a host keeps
         * pushing commands faster than the DAP task drains them, finish the
         * session gracefully instead of desyncing the FIFOs. */
        s_stat_full++;
        s_stat_error++;
        s_active = false;
        printf("USBIP pipeline: command ring full, falling back to legacy\r\n");
    }
    xSemaphoreGive(s_mux);
}

bool usbip_pipeline_handle_in(uint8_t *raw_header48)
{
    usbip_pl_in_action_t action;
    unsigned slot = 0;
    uint32_t seq;
    bool handled = true;

    if (!s_active || s_mux == NULL || raw_header48 == NULL) {
        return false;
    }

    seq = raw_hdr_seqnum(raw_header48);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    usbip_pipeline_reset_stale_locked();
    s_stat_in++;
    usbip_pipeline_touch_locked();

    /* Stray read URB (OpenOCD cmsis_dap_flush_read sends reads before any
     * command): answer an empty RET_SUBMIT immediately, exactly like the
     * legacy fast_reply() path with dap_req_num==0.  Parking it would make it
     * swallow the first real DAP response (observed with OpenOCD master:
     * "CMD_INFO failed" after the flush). */
    if (usbip_pl_idle(&s_state)) {
        usbip_stage2_header *reply = (usbip_stage2_header *)raw_header48;

        reply->base.command = PP_HTONL(USBIP_STAGE2_RSP_SUBMIT);
        reply->base.direction = PP_HTONL(USBIP_DIR_OUT);
        reply->u.ret_submit.status = 0;
        reply->u.ret_submit.data_length = 0;
        reply->u.ret_submit.error_count = 0;
        usbip_network_send(kSock, raw_header48, sizeof(usbip_stage2_header), 0);
        s_stat_stray++;
        xSemaphoreGive(s_mux);
        return true;
    }

    action = usbip_pl_in(&s_state, seq, &slot);

    if (action == USBIP_PL_IN_QUEUE) {
        /* Park the IN URB; the DAP task will answer it when the response is
         * ready (or the TCP task on the next response/IN pairing). */
        s_in_hdrs[slot].seq = seq;
        memcpy(s_in_hdrs[slot].hdr, raw_header48, USBIP_PL_HDR_SIZE);
        s_stat_queue_in++;
    } else if (action == USBIP_PL_IN_SEND) {
        /* A response was already stored in the DAP OUT ring: answer now. */
        uint32_t len = 0;
        if (dap_take_response(s_resp_buf, sizeof(s_resp_buf), &len)) {
            memcpy(s_tx_buf, raw_header48, USBIP_PL_HDR_SIZE);
            send_stage2_submit_data_fast((usbip_stage2_header *)s_tx_buf,
                                         s_resp_buf, (int32_t)len);
            s_stat_send_now++;
        } else {
            /* Book-keeping says a response is queued but the ring is empty:
             * answer with an empty RET_SUBMIT instead of leaving the host URB
             * hanging, and stop the pipeline for this session. */
            usbip_stage2_header reply;
            memset(&reply, 0, sizeof(reply));
            reply.base.command = USBIP_STAGE2_RSP_SUBMIT;
            reply.base.seqnum = seq;
            reply.base.devid = ntohl(raw_hdr_devid(raw_header48));
            reply.base.direction = USBIP_DIR_OUT;
            reply.base.ep = 1;
            reply.u.ret_submit.status = -1;
            reply.u.ret_submit.data_length = 0;
            send_stage2_submit(&reply, -1, 0);
            s_stat_error++;
            s_active = false;
            printf("USBIP pipeline: response/IN desync, falling back to legacy\r\n");
        }
    } else { /* USBIP_PL_IN_FULL */
        usbip_stage2_header reply;
        memset(&reply, 0, sizeof(reply));
        reply.base.command = USBIP_STAGE2_RSP_SUBMIT;
        reply.base.seqnum = seq;
        reply.base.devid = ntohl(raw_hdr_devid(raw_header48));
        reply.base.direction = USBIP_DIR_OUT;
        reply.base.ep = 1;
        reply.u.ret_submit.status = -1;
        reply.u.ret_submit.data_length = 0;
        send_stage2_submit(&reply, -1, 0);
        s_stat_full++;
        s_stat_error++;
        printf("USBIP pipeline: IN ring full, URB failed\r\n");
    }

    xSemaphoreGive(s_mux);
    return handled;
}

bool usbip_pipeline_handle_unlink(uint8_t *header48)
{
    usbip_stage2_header *header = (usbip_stage2_header *)header48;
    bool matched;

    if (!s_active || s_mux == NULL || header == NULL) {
        return false;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    usbip_pipeline_reset_stale_locked();
    matched = usbip_pl_unlink(&s_state, header->u.cmd_unlink.seqnum);
    usbip_pipeline_touch_locked();
    xSemaphoreGive(s_mux);

    /* Always answer the unlink URB (status -1 = "unlinked" as before); the
     * pairing state machine already marked the target so the response stream
     * stays aligned. */
    send_stage2_unlink(header);
    if (!matched) {
        printf("USBIP pipeline: unlink seq=%lu not pending (already completed)\r\n",
               (unsigned long)header->u.cmd_unlink.seqnum);
    }
    return true;
}

void usbip_pipeline_complete_response(const uint8_t *data, uint32_t length)
{
    usbip_pl_resp_action_t action;
    unsigned slot = 0;
    uint32_t seq = 0;

    if (data == NULL) {
        return;
    }
    if (!s_active || s_mux == NULL) {
        /* The pipeline was deactivated between the caller's is_active() check
         * and this call (overflow/desync fallback): keep the response visible
         * to the legacy fast_reply() path instead of losing it. */
        if (!dap_push_response_data(data, length)) {
            printf("USBIP pipeline: fallback response store failed\r\n");
        }
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    usbip_pipeline_touch_locked();
    action = usbip_pl_response(&s_state, &slot, &seq);

    if (action == USBIP_PL_RESP_SEND) {
        if (s_in_hdrs[slot].seq == seq) {
            memcpy(s_tx_buf, s_in_hdrs[slot].hdr, USBIP_PL_HDR_SIZE);
            send_stage2_submit_data_fast((usbip_stage2_header *)s_tx_buf, data,
                                         (int32_t)length);
            s_stat_send_now++;
        } else {
            s_stat_error++;
        }
    } else if (action == USBIP_PL_RESP_QUEUE) {
        if (dap_push_response_data(data, length)) {
            s_stat_queue_resp++;
        } else {
            s_stat_error++;
        }
    } else { /* USBIP_PL_RESP_DROP */
        s_stat_drop++;
    }

    xSemaphoreGive(s_mux);
}

#else /* !CONFIG_BIRDIEBOT_USBIP_PIPELINE */

/* Compiled out: every entry point is a no-op and the legacy single-URB path in
 * usbip_server.c / DAP_handle.c is used unchanged. */
bool usbip_pipeline_enabled(void)
{
    return false;
}

bool usbip_pipeline_is_active(void)
{
    return false;
}

void usbip_pipeline_session_start(void) {}
void usbip_pipeline_session_end(void) {}
/* Drop pipeline leftovers from a previous host session.
 *
 * The USBIP TCP connection lives in the kernel vhci port and survives an
 * OpenOCD restart, so a new openocd process inherits the pairing state of the
 * previous one.  A pending read/write URB that the old process cancelled may
 * leave a parked IN or a stored response behind; the new session's first
 * command would then be paired with that stale entry and time out ("CMD_INFO
 * failed").
 *
 * Invariant: a live CMSIS-DAP host always has at most one round-trip in
 * flight per command, and keeps submitting URBs while a command is outstanding.
 * Therefore a leftover with no URB activity for STALE_IDLE_MS can only belong
 * to a previous session and is safe to discard.  Slow DAP operations are not
 * affected: the host has already submitted the matching IN URB, so the state is
 * paired before the long DAP processing starts.
 */
#define USBIP_PL_STALE_IDLE_US (2000 * 1000) /* > any plausible Wi-Fi RTT */

static void usbip_pipeline_reset_stale_locked(void)
{
    int64_t now = esp_timer_get_time();

    if (usbip_pl_idle(&s_state)) {
        s_last_urb_us = now;
        return;
    }
    if (s_last_urb_us != 0 && (now - s_last_urb_us) < USBIP_PL_STALE_IDLE_US) {
        return;
    }

    usbip_pl_reset(&s_state);
    memset(s_in_hdrs, 0, sizeof(s_in_hdrs));
    {
        uint32_t stale_len = 0;
        while (dap_take_response(s_resp_buf, sizeof(s_resp_buf), &stale_len)) {
            /* drained */
        }
    }
    s_stat_stale++;
    printf("USBIP pipeline: stale session state dropped after idle gap\r\n");
    s_last_urb_us = now;
}

static void usbip_pipeline_touch_locked(void)
{
    s_last_urb_us = esp_timer_get_time();
}

void usbip_pipeline_register_out(uint32_t seqnum) { (void)seqnum; }

bool usbip_pipeline_handle_in(uint8_t *raw_header48)
{
    (void)raw_header48;
    return false;
}

bool usbip_pipeline_handle_unlink(uint8_t *header48)
{
    (void)header48;
    return false;
}

void usbip_pipeline_complete_response(const uint8_t *data, uint32_t length)
{
    (void)data;
    (void)length;
}

#endif /* CONFIG_BIRDIEBOT_USBIP_PIPELINE */
