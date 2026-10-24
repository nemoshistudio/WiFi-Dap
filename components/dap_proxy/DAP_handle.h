#ifndef __DAP_HANDLE_H__
#define __DAP_HANDLE_H__

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "components/USBIP/usbip_defs.h"

enum reset_handle_t
{
    NO_SIGNAL = 0,
    RESET_HANDLE = 1,
    DELETE_HANDLE = 2,
    /* OTA memory-pressure hand-off (task-66b): detach the USBIP data path, drop
     * the pending payloads and FREE both ring buffers so a memory-heavy
     * operation (POST /api/ota/upload needs a 5120-byte task stack and the
     * 1.68 MB streaming path) has the ~41 KiB back.  No re-create happens on
     * later RESET_HANDLE requests until RESTORE_HANDLE (or a reboot). */
    RELEASE_HANDLE = 3,
    /* Re-create the ring buffers after RELEASE_HANDLE (OTA upload aborted /
     * failed without a reboot) and restore the USBIP data path. */
    RESTORE_HANDLE = 4,
};

/*
 * DAP data path ownership (WP-112, extended by task-66)
 * -----------------------------------------------------
 * The DAP task (DAP_Thread) is the single owner of the DAP ring buffers:
 * free_dap_ringbuf()/malloc_dap_ringbuf() are only executed from it.
 * Other tasks post a request through dap_handle_request_reset() instead of
 * touching the ring buffers:
 *
 *   RESET_HANDLE  - restore the USBIP data path (USBIP session end or end of a
 *                   hand-off): drain both rings and clear the "detached" gate.
 *                   The rings are created once when the DAP task starts; they
 *                   are only re-created here when they were never created (very
 *                   early request / boot-time allocation failure) and when the
 *                   rings were not explicitly released (RELEASE_HANDLE).
 *   DELETE_HANDLE - detach the USBIP data path for a transport that runs
 *                   DAP_ExecuteCommand() inline in its own task (WebSocket-DAP,
 *                   elaphureLink, target-flash probe/flash, curve_sampler):
 *                   drain both rings, keep them allocated and gate the USBIP
 *                   request path until the matching RESET_HANDLE.
 *   RELEASE_HANDLE - explicitly free both ring buffers for a memory-heavy
 *                   operation (see dap_handle_release_ringbuffers()).
 *   RESTORE_HANDLE - re-create them after RELEASE_HANDLE (see
 *                   dap_handle_restore_ringbuffers()).
 *
 * Note (task-66): DELETE_HANDLE used to FREE the rings and the following
 * RESET_HANDLE re-created them.  On the Wi-Fi+NCM heap the 20,672-byte response
 * ring does not fit into the largest contiguous block (~20,480 B), so the
 * re-create failed and the next USBIP session died with "CMSIS-DAP command
 * CMD_INFO failed" (e.g. HTTP /api/target/info probe -> host usbip attach).
 * Keeping the buffers for the whole boot removes that allocation from the
 * hand-off path.
 *
 * Note (task-66b): OTA upload regressed with the rings resident for the whole
 * boot: the Wi-Fi+NCM image has `heap_largest` ~4.6 KiB with the rings
 * allocated, which is below the 5120-byte OTA task stack, so POST
 * /api/ota/upload answered 503 "ota service unavailable".  The OTA handler now
 * calls dap_handle_release_ringbuffers() before creating its task and
 * dap_handle_restore_ringbuffers() if the upload fails without a reboot
 * (successful OTA reboots, which re-creates the rings at DAP task start).
 *
 * A caller that is about to use DAP_ExecuteCommand() itself must wait until the
 * request has been applied with dap_handle_wait_reset() (bounded timeout).
 */
void dap_handle_request_reset(enum reset_handle_t mode);

/* Returns 0 when the last posted reset request was applied (or the DAP task has
 * not been started yet), -1 on timeout. */
int dap_handle_wait_reset(uint32_t timeout_ms);

/**
 * @brief Free both DAP ring buffers for a memory-heavy operation (OTA upload).
 *
 * Posts RELEASE_HANDLE and waits (bounded) for the DAP task to apply it: the
 * USBIP data path is detached, pending payloads are dropped and both ~20 KiB
 * ring buffers are freed, which restores the heap headroom the Wi-Fi+NCM image
 * needs for the 5120-byte OTA task stack.  Further RESET_HANDLE requests do not
 * re-create the rings until dap_handle_restore_ringbuffers() (a successful OTA
 * reboots instead, and the next boot creates them in DAP_Thread).
 *
 * @return ESP_OK when applied, ESP_ERR_TIMEOUT when the DAP task did not ack in
 *         time (bounded; caller may continue and report the operation failure).
 */
esp_err_t dap_handle_release_ringbuffers(uint32_t timeout_ms);

/**
 * @brief Re-create the ring buffers and restore the USBIP data path.
 *
 * Undoes dap_handle_release_ringbuffers() (best effort: the re-create can still
 * fail on a fragmented heap, exactly like the old RESET_HANDLE path; the
 * failure is logged and the DAP data path stays unavailable until reboot).
 *
 * @return ESP_OK when applied and the rings exist, ESP_ERR_TIMEOUT when the DAP
 *         task did not ack, ESP_ERR_NO_MEM when the re-create failed.
 */
esp_err_t dap_handle_restore_ringbuffers(uint32_t timeout_ms);

/** True when the ring buffers are currently freed by
 *  dap_handle_release_ringbuffers() (read-only diagnostics). */
bool dap_handle_ringbuffers_released(void);

/* A 2025: CMSIS-DAP status LED initialization (board pins from the board
 * component, no-op on boards without status LEDs). */
void dap_led_init(void);

/* ------------------------------------------------------------------------- */
/* P0-2: startup heap reservation for the DAP ring buffers                   */
/* ------------------------------------------------------------------------- */

/*
 * DEFAULT OFF (2026-10-25, real-device finding): reserving the two ~41 KiB ring
 * storages from the fresh boot heap made the unified NCM+WiFi image lose the
 * USB-NCM device (no 303a:4000 enumeration) and left the internal heap at
 * ~17 KiB/4.6 KiB largest (Dashboard pushed it to ~7 KiB/1.6 KiB).  The old
 * lazy xRingbufferCreate() path in DAP_Thread is the known-good behavior and is
 * back by default; the reservation stays implemented and can be re-enabled by
 * building with -DBIRDIEBOT_DAP_RING_PREALLOC=1 once its interaction with the
 * USB PHY/TinyUSB heap layout is understood.
 *
 * Keep the DAP ring re-create retry loop in DAP_Thread (it does not allocate
 * until a session needs the rings) as the fragmentation mitigation.
 */
#ifndef BIRDIEBOT_DAP_RING_PREALLOC
#define BIRDIEBOT_DAP_RING_PREALLOC 0
#endif

/**
 * @brief Reserve the two USBIP<->DAP ring buffer storage blocks EARLY.
 *
 * Called once from app_main() BEFORE the USB-NCM and Wi-Fi stacks are
 * initialized.  The unified NCM+WiFi image fragments the internal heap enough
 * (measured: 54 KiB free but only a 7.7 KiB largest block) that the normal
 * 20 KiB xRingbufferCreate() calls can fail later; the old failure mode was a
 * silent "DAP ringbuf re-create failed" + target/USBIP timeout.
 *
 * The storage is wrapped with xRingbufferCreateStatic(), so the DAP task's
 * DELETE_HANDLE free / RESET_HANDLE re-create cycle only creates tiny
 * Ringbuffer_t wrappers and never asks the heap for a contiguous 20 KiB block
 * again.  Memory total is the same as a normal session (the rings occupy the
 * heap anyway once created); the reservation just fixes the layout early.
 *
 * @return ESP_OK (also when the rings already exist or the build has no DAP
 *         task yet), ESP_ERR_NO_MEM when even the fresh boot heap refused the
 *         two blocks (the task then falls back to lazy heap ring creation).
 */
esp_err_t dap_handle_prealloc_ringbuffers(void);

/** True when the ring storage was reserved at startup (static wrappers). */
bool dap_handle_ringbuffers_preallocated(void);

/** Read-only ring buffer state for diagnostics: reserved bytes + live flags. */
typedef struct {
    uint32_t request_ring_bytes;   /**< request ring payload capacity */
    uint32_t response_ring_bytes;  /**< response ring payload capacity */
    bool preallocated;             /**< backed by the startup reservation */
    bool active;                   /**< rings currently created (handles != NULL) */
} dap_ring_info_t;

/** Snapshot the ring buffer diagnostics (never blocks on the DAP task). */
void dap_handle_get_ring_info(dap_ring_info_t *out);

/* ------------------------------------------------------------------------- */
/* A3: USBIP<->DAP transaction latency statistics (read-only diagnostics)    */
/* ------------------------------------------------------------------------- */

/**
 * One "transaction" = the time between an OUT URB being queued for the DAP task
 * and the IN URB that returns its response being answered.  This is the
 * device-side latency the host CMSIS-DAP driver pays per command; it is NOT a
 * network RTT (the firmware cannot measure the TCP/HTTP round trip).
 */
typedef struct {
    uint32_t transactions; /**< measured transactions since boot */
    uint32_t avg_us;       /**< mean device-side latency */
    uint32_t max_us;       /**< worst device-side latency */
    uint32_t last_us;      /**< last measured transaction */
} dap_xfer_stats_t;

/** Snapshot the transaction latency counters (all zero before the first DAP
 *  transaction). */
void dap_handle_get_xfer_stats(dap_xfer_stats_t *out);

/* Submit one USBIP OUT URB payload to the DAP task.
 * length = number of payload bytes actually received for this URB (validated
 * by usbip_server.c, <= DAP_PACKET_SIZE).  The fixed-size DAP request packet is
 * zero-filled behind `length` so stale bytes from a previous URB/session can
 * never reach DAP_ProcessCommand (docs/security-review-dap.md N8); the payload
 * is pushed to the request ring straight from the caller's RX buffer. */
void handle_dap_data_request(usbip_stage2_header *header, uint32_t length);

/* B1 pipeline helpers (usbip_pipeline.c): move a finished DAP response between
 * the ring buffer and the USBIP pipeline.  The response ring stores payload-only
 * items (item size == response length, RINGBUF_TYPE_NOSPLIT), so a response is
 * copied once when it has to wait for its IN URB and once more only when the
 * USBIP header+payload staging slot needs it (docs/dap-copy-budget.md).  Both
 * functions are only valid while a USBIP session owns the DAP data path. */
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
