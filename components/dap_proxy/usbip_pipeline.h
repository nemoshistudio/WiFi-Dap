#ifndef __USBIP_PIPELINE_H__
#define __USBIP_PIPELINE_H__
/*
 * B1: USBIP multi-URB pipeline (Kconfig BIRDIEBOT_USBIP_PIPELINE).
 *
 * Why: the USBIP transport is latency-bound over Wi-Fi.  A CMSIS-DAP
 * transaction is an OUT URB (command) plus an IN URB (response); the host
 * (kernel vhci + OpenOCD cmsis-dap, MAX_PENDING_REQUESTS=3) may keep several
 * such URBs in flight.  The legacy firmware path handles one URB at a time and
 * blocks inside fast_reply() until the DAP answer is ready, so the Wi-Fi
 * round-trip of the next URB cannot overlap the DAP processing of the previous
 * one.
 *
 * This pipeline decouples the two directions:
 *   - TCP task: keeps reading CMD_SUBMIT URBs, ACKs OUT URBs immediately and
 *     parks IN URBs in a small FIFO (no blocking on the DAP answer);
 *   - DAP task: when a response is ready, it sends the RET_SUBMIT for the
 *     matching parked IN URB directly (the socket write is serialized with the
 *     TCP task by usbip_network_send()).
 *
 * Responses that are ready before their IN URB arrives are stored in the DAP
 * OUT ring buffer (the same buffer the legacy path uses), so no extra payload
 * memory is needed in the pipeline.
 *
 * UNLINK handling keeps the response/IN FIFOs aligned: a cancelled IN URB
 * leaves a "drop one response" slot behind, a cancelled OUT command marks its
 * ticket so its response is discarded.  On session teardown / any overflow the
 * pipeline is deactivated and the legacy single-URB path is used again.
 *
 * The pure pairing state machine lives in usbip_pipeline_logic.c and is
 * unit-tested on the host (components/dap_proxy/test).
 */
#include <stdbool.h>
#include <stdint.h>

/* True when the feature is compiled in and the session pipeline is running. */
bool usbip_pipeline_enabled(void);
bool usbip_pipeline_is_active(void);

/* Begin/end a USBIP session (called by usbip_server.c around the URB loop). */
void usbip_pipeline_session_start(void);
void usbip_pipeline_session_end(void);

/* SUBMIT/DIR_OUT of endpoint 1: register the command ticket before the payload
 * is pushed into the DAP request ring. */
void usbip_pipeline_register_out(uint32_t seqnum);

/* SUBMIT/DIR_IN of endpoint 1: returns true when the pipeline handled the URB
 * (answered or parked), false when the caller must use the legacy fast_reply()
 * path (pipeline inactive or internal fallback). */
bool usbip_pipeline_handle_in(uint8_t *raw_header48);

/* CMD_UNLINK (header already converted to host byte order by the caller):
 * returns true when the pipeline handled the cancellation and sent RET_UNLINK,
 * false when the caller must run the legacy unlink path. */
bool usbip_pipeline_handle_unlink(uint8_t *header48);

/* DAP task: one finished DAP response (send, store or drop per pairing). */
void usbip_pipeline_complete_response(const uint8_t *data, uint32_t length);

#endif /* __USBIP_PIPELINE_H__ */
