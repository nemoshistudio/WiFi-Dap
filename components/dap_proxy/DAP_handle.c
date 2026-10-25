/**
 * @file DAP_handle.c
 * @brief Handle DAP packets and transaction push
 * @version 0.6
 * @change: 2020.02.04 first version
 *          2020.11.11 support WinUSB mode
 *          2021.02.17 support SWO
 *          2021.10.03 try to handle unlink behavior
 *          2025.xx.xx A (wireless-esp8266-dap fork): status LED init,
 *                     DAP_BUFFER_NUM 10/20 selection
 *
 * WiFi Dap merge notes (WP-112)
 * ------------------------------
 *  - Merged from the A 2025 version (dap_led_init, DAP_BUFFER_NUM) with the
 *    C/IDF5 platform layer (printf/component includes/BSD sockets).  The A tree
 *    configured DAP_BUFFER_NUM from USE_MDNS/USE_OTA; the merged tree always
 *    keeps the Wi-Fi/mDNS/web stack, so the 10-slot layout is used.
 *  - Ring buffer lifecycle is owned by this task only (see DAP_handle.h).
 *    The TCP/WebSocket tasks post reset requests; they never free the buffers.
 *    The buffers are created once when the task starts and are only drained
 *    afterwards: the Wi-Fi+NCM heap cannot reliably provide the 20,672-byte
 *    contiguous response ring again (task-66 "CMD_INFO failed" root cause).
 *  - DELETE_HANDLE detaches the USBIP data path (drain + gate) while another
 *    transport runs DAP_ExecuteCommand() inline; RESET_HANDLE restores it.
 *  - Host-side response clipping keeps an over-long DAP response (P0 in
 *    analysis/REPORT.md) from being pushed to the USBIP/WS transports.
 *  - The A-tree 8266 target is not supported by this project; no 8266 branch.
 *
 * @copyright Copyright (c) 2021
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "usbip_server.h"
#include "usbip_pipeline.h"
#include "DAP_handle.h"
#include "main/dap_configuration.h"
#include "cmsis-dap/include/DAP.h"
#include "cmsis-dap/include/swo.h"
#include "cmsis-dap/include/gpio_op.h"
#include "DAP_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "lwip/err.h"
#include "lwip/sockets.h"

/* Pipeline depth of the two USBIP<->DAP ring buffers.
 *
 * RAM budget is kept at ~41 KB whatever packet size B1 selects:
 *   DAP_PACKET_SIZE  1024 -> 20 slots
 *                    1280 -> 16 slots
 *                    4048 ->  5 slots
 * The host (OpenOCD cmsis-dap) keeps at most MAX_PENDING_REQUESTS=3 blocks in
 * flight and the B1 pipeline parks at most that many OUT commands / responses,
 * so 5 slots are the minimum with some slack.
 *
 * Copy budget (docs/dap-copy-budget.md): the two rings carry two different
 * payload shapes, which removes the old fixed 4-byte-length+1280-byte staging
 * item and its per-transaction memcpy/memset:
 *
 *  - request ring (BYTEBUF): items are exactly DAP_PACKET_SIZE bytes of DAP
 *    request payload.  The ring size is an exact multiple of the item size, so
 *    a byte-buffer item can never be split at the buffer wrap point and
 *    xRingbufferReceiveUpTo() always returns a whole request.  The TCP task
 *    writes the payload straight from its RX buffer (tail zero-filled for the
 *    N8 stale-byte guard) - no staging copy.
 *
 *  - response ring (NOSPLIT): one item per DAP response, item size == response
 *    length (0..DAP_PACKET_SIZE).  NOSPLIT guarantees the item is contiguous,
 *    so the response is pushed straight from the DAP command layer's output
 *    buffer - no staging copy - and the consumer gets the exact length from
 *    xRingbufferReceive().  The per-slot overhead covers the FreeRTOS item
 *    header and a possible wrap dummy header. */
#if DAP_PACKET_SIZE > 2048U
#define DAP_BUFFER_NUM 5
#elif DAP_PACKET_SIZE > 1024U
#if defined(CONFIG_IDF_TARGET_ESP32C5)
/* ESP32-C5 real hardware: 6 slots, allocated from PSRAM (see DAP_RING_USE_PSRAM). */
#define DAP_BUFFER_NUM 6
#else
/* ESP-IDF v5.5.6 + NCM leaves a largest free block of only ~11 KiB and
 * the two rings must be allocated sequentially: 4 slots (5,120 B request,
 * 5,216 B response) fit two blocks.  Host keeps <=3 URBs in flight. */
#define DAP_BUFFER_NUM 4
#endif
#else
#define DAP_BUFFER_NUM 20
#endif

#define DAP_REQUEST_RING_SIZE  ((size_t)DAP_PACKET_SIZE * DAP_BUFFER_NUM)
/* 8 bytes item header + 8 bytes wrap/dummy slack per slot (see
 * prvCheckItemFitsDefault/prvGetItemDefault in esp_ringbuf/ringbuf.c). */
#define DAP_RESPONSE_RING_SIZE (((size_t)DAP_PACKET_SIZE + 8U) * DAP_BUFFER_NUM + 64U)

#if defined(CONFIG_IDF_TARGET_ESP32C5) && defined(CONFIG_SPIRAM)
/* ESP32-C5 real hardware: the internal heap that is left after Wi-Fi + the web
 * server is not large enough for the two ring storages (see the internal-heap
 * budget in sdkconfig.defaults.esp32c5).  The N8R8 module has 8 MB PSRAM, and
 * the rings are pure CPU payload buffers (copied by memcpy, never DMA'd or
 * bit-banged), so they can live in PSRAM.  The DAP task stack stays internal
 * (FreeRTOS requirement by default).
 *
 * The ring handles remember which allocator was used: XRingbufferCreateWithCaps
 * rings must be released with vRingbufferDeleteWithCaps (IDF ringbuf API). */
#define DAP_RING_USE_PSRAM 1
#define DAP_RING_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
static bool s_req_ring_psram;
static bool s_resp_ring_psram;
#else
#define DAP_RING_USE_PSRAM 0
#endif

/* Bounded wait used when pushing a response into the OUT ring buffer, so that a
 * reset request (ring buffer ownership change) is always observed. */
#define DAP_RINGBUF_SEND_TIMEOUT_MS 20
/* Upper bound for keeping a full response in the DAP task while the host does
 * not drain the OUT ring; after this the response is dropped instead of
 * dead-locking the DAP/TCP tasks (analysis/REPORT.md P1 #4). */
#define DAP_RINGBUF_SEND_RETRIES 500

/* --------------------------------------------------------------------------
 * A3: USBIP<->DAP transaction latency statistics (always-on, tiny counters).
 *
 * The device-side latency the host CMSIS-DAP driver pays per command: the time
 * between "OUT URB queued for the DAP task" and "the IN URB that returns the
 * response is being answered".  This isolates transport/scheduling overhead
 * from the SWD bit-banging time and is exposed by GET /api/target/status as
 * dap_xfer_count/avg_us/max_us/last_us.  It is NOT a network RTT.
 *
 * Kept in RAM (4 counters) so diagnostics are available in release builds;
 * -DDAP_PROXY_STATS=1 additionally prints a periodic summary (old behavior).
 * -------------------------------------------------------------------------- */
#ifndef DAP_PROXY_STATS
#define DAP_PROXY_STATS 0
#endif

static int64_t s_stats_out_us;      /* 0 = no OUT URB waiting for its IN */
static uint32_t s_stats_count;      /* measured transactions since boot */
static uint32_t s_stats_sum_us;     /* sum of latency samples (u32 wrap ok) */
static uint32_t s_stats_max_us;
static uint32_t s_stats_last_us;

static inline void dap_stats_mark_out(void)
{
    s_stats_out_us = esp_timer_get_time();
}

static inline void dap_stats_mark_in(void)
{
    int64_t now_us;
    uint32_t dt_us;

    if (s_stats_out_us == 0) {
        return;
    }
    now_us = esp_timer_get_time();
    if (now_us <= s_stats_out_us) {
        s_stats_out_us = 0; /* clock wrap / reset: ignore this sample */
        return;
    }
    dt_us = (uint32_t)(now_us - s_stats_out_us);
    s_stats_out_us = 0;
    s_stats_count++;
    s_stats_sum_us += dt_us;
    s_stats_last_us = dt_us;
    if (dt_us > s_stats_max_us) {
        s_stats_max_us = dt_us;
    }
#if DAP_PROXY_STATS
    if ((s_stats_count % 128U) == 0U) {
        printf("DAP_STATS transactions=%lu avg=%lu us max=%lu us\r\n",
               (unsigned long)s_stats_count,
               (unsigned long)(s_stats_sum_us / s_stats_count),
               (unsigned long)s_stats_max_us);
    }
#endif
}

void dap_handle_get_xfer_stats(dap_xfer_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->transactions = s_stats_count;
    out->avg_us = (s_stats_count != 0U) ? (s_stats_sum_us / s_stats_count) : 0U;
    out->max_us = s_stats_max_us;
    out->last_us = s_stats_last_us;
}

int kRestartDAPHandle = NO_SIGNAL;
TaskHandle_t kDAPTaskHandle = NULL;


/* Output buffer of one DAP command.  The response is pushed to the OUT ring
 * (or straight into the USBIP staging slot) directly from here; the old
 * {length, buf[]} struct was only needed because the ring item had a fixed
 * staging layout. */
static uint8_t DAPDataProcessed[DAP_PACKET_SIZE];
static int dap_respond = 0;

// SWO Trace
static uint8_t *swo_data_to_send = NULL;
static uint32_t swo_data_num;

// DAP handle
static RingbufHandle_t dap_dataIN_handle = NULL;
static RingbufHandle_t dap_dataOUT_handle = NULL;
static SemaphoreHandle_t data_response_mux = NULL;
/* Signalled by the DAP task after a requested reset/hand-off has been applied;
 * see dap_handle_wait_reset(). */
static SemaphoreHandle_t dap_reset_done = NULL;

/* "Detached" data path (task-66).
 *
 * A DELETE_HANDLE request means another transport (WebSocket-DAP, elaphureLink,
 * target-flash probe/flash or curve_sampler) takes exclusive ownership of the
 * CMSIS-DAP engine and runs DAP_ExecuteCommand() inline.  While this flag is
 * set the USBIP request path must not feed the rings (the transport is being
 * torn down; its request gets the same empty RET_SUBMIT as before) and the DAP
 * task must not consume ring items, otherwise it would race the inline
 * executor on the global DAP/SWD state.
 *
 * The ring buffers themselves are NOT freed for a DELETE_HANDLE any more
 * (task-66): freeing them made the following RESET_HANDLE re-create fail on the
 * Wi-Fi+NCM-fragmented heap (the 20,672-byte response ring does not fit into the
 * measured 20,480-byte largest contiguous block), leaving the next USBIP session
 * with no data path and a "CMSIS-DAP command CMD_INFO failed" timeout.  They are
 * created once when the DAP task starts and only drained from then on.
 *
 * RELEASE_HANDLE (task-66b) is the explicit exception used by the OTA upload
 * path: it frees the rings on purpose (s_rings_released) because the OTA task
 * stack + upload path need that ~41 KiB; the rings come back on RESTORE_HANDLE
 * or at the next boot. */
static volatile bool s_data_path_detached = false;
static volatile bool s_task_started = false;
static char s_ring_error[96];

/* "Released" ring buffers (task-66b).
 *
 * Set by RELEASE_HANDLE / dap_handle_release_ringbuffers(): the OTA upload path
 * needs the ~41 KiB of the two DAP ring buffers back (the Wi-Fi+NCM image has
 * heap_largest ~4.6 KiB with them allocated, below the 5120-byte OTA task
 * stack).  While set, RESET_HANDLE (e.g. from a tearing-down USBIP session) does
 * NOT lazily re-create the rings: the memory must stay available until the OTA
 * upload either reboots the device or fails and calls RESTORE_HANDLE. */
static volatile bool s_rings_released = false;


/* ------------------------------------------------------------------------- */
/* P0-2: startup reservation of the ring storage blocks                      */
/* ------------------------------------------------------------------------- */

/* Raw storage + static wrapper for the two rings.  Reserved from the fresh
 * boot heap before USB-NCM/WiFi start; the normal xRingbufferCreate() path is
 * kept as fallback when the reservation failed. */
static uint8_t *s_prealloc_req_storage;
static uint8_t *s_prealloc_resp_storage;
static StaticRingbuffer_t s_prealloc_req_struct;
static StaticRingbuffer_t s_prealloc_resp_struct;
static bool s_prealloc_ready;

esp_err_t dap_handle_prealloc_ringbuffers(void)
{
    if (s_prealloc_ready) {
        return ESP_OK;
    }
    if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
        return ESP_OK; /* a session already created them (unexpected early) */
    }
    if (s_prealloc_req_storage == NULL) {
        s_prealloc_req_storage = (uint8_t *)heap_caps_malloc(
            DAP_REQUEST_RING_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (s_prealloc_resp_storage == NULL) {
        s_prealloc_resp_storage = (uint8_t *)heap_caps_malloc(
            DAP_RESPONSE_RING_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (s_prealloc_req_storage == NULL || s_prealloc_resp_storage == NULL) {
        free(s_prealloc_req_storage);
        free(s_prealloc_resp_storage);
        s_prealloc_req_storage = NULL;
        s_prealloc_resp_storage = NULL;
        printf("DAP: ring prealloc failed (no contiguous %u+%u B)\r\n",
               (unsigned)DAP_REQUEST_RING_SIZE,
               (unsigned)DAP_RESPONSE_RING_SIZE);
        return ESP_ERR_NO_MEM;
    }
    /* No FreeRTOS objects here: DAP_Thread creates the mutex/completion
     * semaphore itself.  Keeping this function to plain heap reservations
     * avoids touching unrelated subsystems before USB/WiFi finish init. */
    s_prealloc_ready = true;
    printf("DAP: ring storage reserved early: request %u B, response %u B "
           "(static wrappers, largest free now %u B)\r\n",
           (unsigned)DAP_REQUEST_RING_SIZE,
           (unsigned)DAP_RESPONSE_RING_SIZE,
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                      MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

bool dap_handle_ringbuffers_preallocated(void)
{
    return s_prealloc_ready;
}

void dap_handle_get_ring_info(dap_ring_info_t *out)
{
    if (out == NULL) {
        return;
    }
    out->request_ring_bytes = (uint32_t)DAP_REQUEST_RING_SIZE;
    out->response_ring_bytes = (uint32_t)DAP_RESPONSE_RING_SIZE;
    out->preallocated = s_prealloc_ready;
    out->active = (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL);
}

/** Create one ring: static storage when reserved early, PSRAM on the C5,
 *  internal heap otherwise.  @p from_psram is set when the caps allocator was
 *  used (the matching delete call differs). */
static RingbufHandle_t dap_ring_create(size_t bytes, RingbufferType_t type,
                                       uint8_t *static_storage,
                                       StaticRingbuffer_t *static_struct,
                                       bool *from_psram)
{
    if (from_psram != NULL) {
        *from_psram = false;
    }
    if (static_storage != NULL && static_struct != NULL) {
        return xRingbufferCreateStatic(bytes, type, static_storage,
                                       static_struct);
    }
#if DAP_RING_USE_PSRAM
    /* Try PSRAM first on the C5; fall back to the internal heap if PSRAM is not
     * available at runtime (external RAM disabled / not detected). */
    {
        RingbufHandle_t rb = xRingbufferCreateWithCaps(bytes, type,
                                                       DAP_RING_PSRAM_CAPS);
        if (rb != NULL) {
            if (from_psram != NULL) {
                *from_psram = true;
            }
            return rb;
        }
    }
#endif
    return xRingbufferCreate(bytes, type);
}

void malloc_dap_ringbuf(void) {
    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
    {
        if (dap_dataIN_handle == NULL) {
            /* Request ring: fixed DAP_PACKET_SIZE payload items (no split, the
             * ring size is an exact multiple of the item size). */
#if DAP_RING_USE_PSRAM
            s_req_ring_psram = false;
#endif
            dap_dataIN_handle = dap_ring_create(
                DAP_REQUEST_RING_SIZE, RINGBUF_TYPE_BYTEBUF,
                s_prealloc_ready ? s_prealloc_req_storage : NULL,
                s_prealloc_ready ? &s_prealloc_req_struct : NULL,
#if DAP_RING_USE_PSRAM
                &s_req_ring_psram
#else
                NULL
#endif
                );
        }
        if (dap_dataOUT_handle == NULL) {
            /* Response ring: variable-size payload items, each one contiguous. */
#if DAP_RING_USE_PSRAM
            s_resp_ring_psram = false;
#endif
            dap_dataOUT_handle = dap_ring_create(
                DAP_RESPONSE_RING_SIZE, RINGBUF_TYPE_NOSPLIT,
                s_prealloc_ready ? s_prealloc_resp_storage : NULL,
                s_prealloc_ready ? &s_prealloc_resp_struct : NULL,
#if DAP_RING_USE_PSRAM
                &s_resp_ring_psram
#else
                NULL
#endif
                );
        }

        xSemaphoreGive(data_response_mux);
    }
}

/* Drain one ring without freeing it.  Used by both reset modes: discarding the
 * payloads is enough to start a new session / hand the engine over, and keeping
 * the buffers allocated avoids a 20 KiB xRingbufferCreate() from the
 * Wi-Fi+NCM-fragmented heap.  The re-create used to fail there (response ring
 * 20,672 B > largest free block 20,480 B), which left the next USBIP session
 * without a data path ("CMSIS-DAP command CMD_INFO failed"): C1 second-session
 * bug (RESET_HANDLE) and task-66 (target/info probe handing the DAP off through
 * DELETE_HANDLE). */
static void drain_request_ringbuf(RingbufHandle_t rb)
{
    if (rb == NULL) {
        return;
    }
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(rb, &item_size, 0, DAP_PACKET_SIZE);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(rb, item);
    }
}

/* NOSPLIT rings return whole items only, so use the item-based API (the
 * xRingbufferReceiveUpTo() byte-buffer API asserts on NOSPLIT rings). */
static void drain_response_ringbuf(RingbufHandle_t rb)
{
    if (rb == NULL) {
        return;
    }
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(rb, &item_size, 0);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(rb, item);
    }
}

/* Discard pending payloads without touching the ring allocation; used when the
 * data path is detached or restored so a previous transport's payloads can
 * never be paired with a new session's URBs.  The DAP task calls this for both
 * reset modes; it must not hold data_response_mux. */
static void dap_reset_pending_frames(void)
{
    if (data_response_mux &&
        xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
        dap_respond = 0;
        xSemaphoreGive(data_response_mux);
    }
    drain_request_ringbuf(dap_dataIN_handle);
    drain_response_ringbuf(dap_dataOUT_handle);
}

void free_dap_ringbuf(void) {
    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
#if DAP_RING_USE_PSRAM
        if (dap_dataIN_handle) {
            if (s_req_ring_psram) {
                vRingbufferDeleteWithCaps(dap_dataIN_handle);
            } else {
                vRingbufferDelete(dap_dataIN_handle);
            }
            s_req_ring_psram = false;
        }
        if (dap_dataOUT_handle) {
            if (s_resp_ring_psram) {
                vRingbufferDeleteWithCaps(dap_dataOUT_handle);
            } else {
                vRingbufferDelete(dap_dataOUT_handle);
            }
            s_resp_ring_psram = false;
        }
#else
        if (dap_dataIN_handle) {
            vRingbufferDelete(dap_dataIN_handle);
        }
        if (dap_dataOUT_handle) {
            vRingbufferDelete(dap_dataOUT_handle);
        }
#endif

        dap_dataIN_handle = dap_dataOUT_handle = NULL;
        xSemaphoreGive(data_response_mux);
    }

}

/* Lazy-create fallback of the DAP task (only used when the rings were never
 * created, or after dap_handle_restore_ringbuffers()).  Retry a few times: a
 * transient allocation failure must not leave the device without a DAP data
 * path (the old failure path killed the DAP task, C1 second-session bug). */
static bool dap_create_rings_retry(void)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        malloc_dap_ringbuf();
        if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
            if (s_rings_released) {
                printf("DAP: ring buffers re-created after release\r\n");
            }
            return true;
        }
        free_dap_ringbuf();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("Can not create DAP ringbuf!\r\n");
    return false;
}

void dap_handle_request_reset(enum reset_handle_t mode)
{
    /* Drop a stale completion token before posting, so that a following
     * dap_handle_wait_reset() only observes the result of this request. */
    if (dap_reset_done)
        (void)xSemaphoreTake(dap_reset_done, 0);

    kRestartDAPHandle = mode;

    if (kDAPTaskHandle)
        xTaskNotifyGive(kDAPTaskHandle);
}

int dap_handle_wait_reset(uint32_t timeout_ms)
{
    if (dap_reset_done == NULL) {
        /* The DAP task has not been started yet; it applies the pending request
         * before it starts processing DAP commands. */
        return 0;
    }

    if (xSemaphoreTake(dap_reset_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        printf("DAP: timeout waiting for ring buffer reset\r\n");
        return -1;
    }

    return 0;
}

esp_err_t dap_handle_release_ringbuffers(uint32_t timeout_ms)
{
    dap_handle_request_reset(RELEASE_HANDLE);
    if (dap_handle_wait_reset(timeout_ms) != 0) {
        printf("DAP: ring buffer release not applied within %u ms\r\n",
               (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t dap_handle_restore_ringbuffers(uint32_t timeout_ms)
{
    dap_handle_request_reset(RESTORE_HANDLE);
    if (dap_handle_wait_reset(timeout_ms) != 0) {
        printf("DAP: ring buffer restore not applied within %u ms\r\n",
               (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL) {
        /* Re-create failed on the fragmented heap (documented best effort). */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool dap_handle_ringbuffers_released(void)
{
    return s_rings_released;
}

void handle_dap_data_request(usbip_stage2_header *header, uint32_t length)
{
    uint8_t *data_in = (uint8_t *)header + sizeof(usbip_stage2_header);
    /* Point to the beginning of the URB payload (inside the caller's RX buffer,
     * which is at least sizeof(header)+DAP_PACKET_SIZE bytes: usbip_urb_process
     * validates that, and dap_proxy/tcp_server.c provides a 4096-byte buffer). */

    /* N8 (docs/security-review-dap.md, P2): only the bytes actually received
     * belong to this request.  The DAP command parser may read a fixed-size
     * request based on the command byte, so everything behind `length` must be
     * zero.  We zero the tail in place instead of copying the request into a
     * separate zeroed staging packet: the ring buffer copies the whole fixed
     * DAP_PACKET_SIZE item anyway (docs/dap-copy-budget.md). */
    if (length > DAP_PACKET_SIZE) {
        length = DAP_PACKET_SIZE;
    }

    /* The DAP data path can legitimately be unavailable while a
     * WebSocket/elaphureLink/target-flash/curve transport owns the DAP engine
     * after DELETE_HANDLE (DAP_handle.h).  The USBIP transport is about to be
     * torn down in that case, so only answer the zero-length RET_SUBMIT and do
     * not touch the ring buffer. */
    send_stage2_submit_data_fast(header, NULL, 0);

    if (s_data_path_detached) {
        printf("DAP: data path detached for hand-off, request dropped\r\n");
        return;
    }
    if (dap_dataIN_handle == NULL) {
        printf("DAP: data IN ring buffer not available, request dropped\r\n");
        return;
    }

    if (length < DAP_PACKET_SIZE) {
        memset(data_in + length, 0, (size_t)DAP_PACKET_SIZE - length);
    }

    /* Push the fixed-size request payload straight from the RX buffer. */
    if (xRingbufferSend(dap_dataIN_handle, data_in, DAP_PACKET_SIZE,
                        portMAX_DELAY) != pdTRUE) {
        printf("DAP: data IN ring buffer full, request dropped\r\n");
        return;
    }
    if (kDAPTaskHandle)
        xTaskNotifyGive(kDAPTaskHandle);

    dap_stats_mark_out(); /* A3: start of the host-visible transaction latency */
}

void handle_swo_trace_response(usbip_stage2_header *header)
{
#if (SWO_FUNCTION_ENABLE == 1)
    if (kSwoTransferBusy)
    {
        // busy indicates that there is data to be send
        printf("swo use data\r\n");
        send_stage2_submit_data(header, 0, (void *)swo_data_to_send, swo_data_num);
        SWO_TransferComplete();
    }
    else
    {
        // nothing to send.
        send_stage2_submit(header, 0, 0);
    }
#else
    send_stage2_submit(header, 0, 0);
#endif
}

// SWO Data Queue Transfer
//   buf:    pointer to buffer with data
//   num:    number of bytes to transfer
void SWO_QueueTransfer(uint8_t *buf, uint32_t num)
{
    swo_data_to_send = buf;
    swo_data_num = num;
}

/**
 * @brief Push one processed DAP response into the response ring buffer.
 *
 * The response ring stores the payload only (item size == response length), so
 * this is a single copy from the caller's DAP output buffer: the old code first
 * copied into a fixed-size staging packet and then sent that.  The send is
 * bounded so that a pending reset request (the only way the ring buffers are
 * released) is always observed; if the host never drains the ring the response
 * is dropped after a bounded time instead of dead-locking the DAP and TCP
 * tasks.  Returns true when the response was queued.
 */
static bool dap_push_response(const uint8_t *data, size_t length)
{
    int retries = 0;

    while (dap_dataOUT_handle != NULL &&
           xRingbufferSend(dap_dataOUT_handle, (void *)data, length,
                           pdMS_TO_TICKS(DAP_RINGBUF_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        if (kRestartDAPHandle != NO_SIGNAL) {
            printf("DAP: response dropped, ring buffer reset requested\r\n");
            return false;
        }
        if (++retries >= DAP_RINGBUF_SEND_RETRIES) {
            printf("DAP: OUT ring buffer full for too long, response dropped\r\n");
            return false;
        }
    }

    if (dap_dataOUT_handle != NULL) {
        if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
            ++dap_respond;
            xSemaphoreGive(data_response_mux);
        }
        return true;
    }
    return false;
}

bool dap_push_response_data(const uint8_t *data, uint32_t length)
{
    if (dap_dataOUT_handle == NULL) {
        return false;
    }
    /* Boundary guard: the response ring can only hold DAP_PACKET_SIZE bytes per
     * item; the DAP layer clips the response before calling here, so an
     * oversized value means a firmware bug - drop it instead of corrupting the
     * ring item / the host response length. */
    if (length > (uint32_t)DAP_PACKET_SIZE) {
        printf("DAP: response length %lu exceeds DAP_PACKET_SIZE %u, dropped\r\n",
               (unsigned long)length, (unsigned)DAP_PACKET_SIZE);
        return false;
    }
    if (data == NULL && length != 0U) {
        return false;
    }

    return dap_push_response(data, (size_t)length);
}

bool dap_take_response(uint8_t *buf, uint32_t buf_size, uint32_t *length)
{
    void *item;
    size_t item_size = 0;

    if (length) {
        *length = 0;
    }
    if (dap_dataOUT_handle == NULL || buf == NULL || buf_size == 0) {
        return false;
    }

    /* NOSPLIT ring: the whole response is one contiguous item and its size is
     * the response length (no fixed-size staging item any more). */
    item = xRingbufferReceive(dap_dataOUT_handle, &item_size, 0);
    if (item == NULL) {
        return false;
    }

    /* Boundary checks: never truncate silently and never copy more than the
     * caller's buffer can hold. */
    if (item_size > (size_t)DAP_PACKET_SIZE || item_size > buf_size) {
        printf("DAP: response item %u B does not fit (cap %u/%u), dropped\r\n",
               (unsigned)item_size, (unsigned)DAP_PACKET_SIZE, (unsigned)buf_size);
        vRingbufferReturnItem(dap_dataOUT_handle, item);
        return false;
    }

    if (item_size > 0) {
        memcpy(buf, item, item_size);
    }
    if (length) {
        *length = (uint32_t)item_size;
    }
    vRingbufferReturnItem(dap_dataOUT_handle, item);

    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
        --dap_respond;
        xSemaphoreGive(data_response_mux);
    }
    return true;
}

void DAP_Thread(void *argument)
{
    size_t packetSize;
    s_task_started = true;
    int resLength;
    uint8_t *item;

    /* P0-2: rings/mux may already exist from the early app_main()
     * reservation (dap_handle_prealloc_ringbuffers); reuse them instead of
     * overwriting the handles and leaking the reservation. */
    if (data_response_mux == NULL) {
        data_response_mux = xSemaphoreCreateMutex();
    }
    if (dap_reset_done == NULL) {
        dap_reset_done = xSemaphoreCreateBinary();
    }
#if BIRDIEBOT_DAP_RING_PREALLOC
    /* Second chance for the ring storage reservation (only when the optional
     * P0-2 reservation is enabled; default off, see DAP_handle.h). */
    if (!s_prealloc_ready) {
        (void)dap_handle_prealloc_ringbuffers();
    }
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    printf("DAP: task running (internal free=%u largest=%u)\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT));
#endif
    /* The unified NCM+WiFi image can be short of contiguous heap in the first
     * moments of boot (IDF 5.5.6 NCM allocates earlier than the old baseline;
     * C5 additionally competes with Wi-Fi/Web). Do not kill the DAP task on
     * the first failed allocation: retry for up to ~2 s. */
    for (int attempt = 0; attempt < 20 && (dap_dataIN_handle == NULL ||
                                           dap_dataOUT_handle == NULL); attempt++) {
        malloc_dap_ringbuf();
        if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
            break;
        }
        free_dap_ringbuf();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    kDAPTaskHandle = xTaskGetCurrentTaskHandle();

    /* A 2025: initialize the CMSIS-DAP status LEDs (board pins, no-op when the
     * selected board has no status LEDs). */
    dap_led_init();

    if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL ||
        data_response_mux == NULL)
    {
        snprintf(s_ring_error, sizeof(s_ring_error),
                 "ring/mux fail req=%u resp=%u free=%u largest=%u",
                 (unsigned)DAP_REQUEST_RING_SIZE,
                 (unsigned)DAP_RESPONSE_RING_SIZE,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT |
                                                   MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                            MALLOC_CAP_INTERNAL));
        printf("Can not create DAP ringbuf/mux! %s\r\n", s_ring_error);
#if defined(CONFIG_IDF_TARGET_ESP32C5)
        printf("DAP C5 internal free=%u largest=%u\r\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                          MALLOC_CAP_8BIT));
#endif
        vTaskDelete(NULL);
    }
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#if DAP_RING_USE_PSRAM
    printf("DAP: rings ready req=%u B resp=%u B (psram=%d/%d, internal free=%u largest=%u)\r\n",
           (unsigned)DAP_REQUEST_RING_SIZE, (unsigned)DAP_RESPONSE_RING_SIZE,
           s_req_ring_psram ? 1 : 0, s_resp_ring_psram ? 1 : 0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT));
#else
    printf("DAP: rings ready req=%u B resp=%u B (internal free=%u largest=%u)\r\n",
           (unsigned)DAP_REQUEST_RING_SIZE, (unsigned)DAP_RESPONSE_RING_SIZE,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT));
#endif
#endif
    s_ring_error[0] = '\0';
    for (;;)
    {

        while (1)
        {
            if (kRestartDAPHandle != NO_SIGNAL)
            {
                /* The DAP task owns the ring buffer lifecycle: drain/recreate
                 * only here, never in the TCP/WebSocket tasks. */
                enum reset_handle_t mode = (enum reset_handle_t)kRestartDAPHandle;

                if (mode == RESET_HANDLE)
                {
                    /* Restore the USBIP/WebSocket data path (session end or
                     * end of a DELETE_HANDLE hand-off).  The rings are created
                     * once when this task starts and are only drained here: a
                     * free + re-create would need a 20,672-byte contiguous
                     * block after Wi-Fi+NCM fragmented the heap (measured
                     * largest free block 20,480 B), which failed and left the
                     * next USBIP session without a data path (C1/task-66
                     * "CMSIS-DAP command CMD_INFO failed").  Re-create only
                     * when the rings were never created (very early reset or a
                     * boot-time allocation failure) and were not explicitly
                     * released for OTA (task-66b: the memory must stay free
                     * until the OTA upload reboots or restores). */
                    if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
                        dap_reset_pending_frames();
                    } else if (!s_rings_released) {
                        (void)dap_create_rings_retry();
                    }
                    s_data_path_detached = false;
                }
                else if (mode == RELEASE_HANDLE)
                {
                    /* OTA memory-pressure hand-off (task-66b): block the USBIP
                     * request path, drop pending payloads and FREE both ring
                     * buffers so POST /api/ota/upload can allocate its 5120-byte
                     * task stack (with the rings resident heap_largest was
                     * ~4.6 KiB -> 503 "ota service unavailable").  RESET_HANDLE
                     * does not re-create them until RESTORE_HANDLE/reboot. */
                    s_data_path_detached = true;
                    s_rings_released = true;
                    if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
                        dap_reset_pending_frames();
                        free_dap_ringbuf();
                    }
                    printf("DAP: ring buffers released for OTA/memory pressure "
                           "(request %u B + response %u B freed)\r\n",
                           (unsigned)DAP_REQUEST_RING_SIZE,
                           (unsigned)DAP_RESPONSE_RING_SIZE);
                }
                else if (mode == RESTORE_HANDLE)
                {
                    /* OTA aborted/failed without a reboot: get the DAP data
                     * path back (best effort; the re-create can fail on the
                     * fragmented heap exactly like the old RESET_HANDLE path). */
                    s_rings_released = false;
                    if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
                        dap_reset_pending_frames();
                    } else {
                        (void)dap_create_rings_retry();
                    }
                    s_data_path_detached = false;
                }
                else /* DELETE_HANDLE */
                {
                    /* Detach the DAP data path for a transport that runs
                     * DAP_ExecuteCommand() inline (WebSocket-DAP, elaphureLink,
                     * target-flash probe/flash, curve_sampler).  Set the flag
                     * BEFORE draining so neither the TCP request path nor this
                     * task can touch the rings afterwards.  The buffers stay
                     * allocated for the whole boot; freeing them here was the
                     * task-66 bug because the following RESET_HANDLE re-create
                     * did not fit into the fragmented heap. */
                    s_data_path_detached = true;
                    if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL) {
                        dap_reset_pending_frames();
                    }
                    printf("DAP: data path detached (rings kept: request %u B, "
                           "response %u B)\r\n",
                           (unsigned)DAP_REQUEST_RING_SIZE,
                           (unsigned)DAP_RESPONSE_RING_SIZE);
                }

                kRestartDAPHandle = NO_SIGNAL;

                if (dap_reset_done)
                    xSemaphoreGive(dap_reset_done);
            }

            ulTaskNotifyTake(pdFALSE, portMAX_DELAY); // wait event


            if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL) {
                continue; // may be use elaphureLink, wait...
            }
            /* Never consume ring items while another transport owns the DAP
             * engine inline (DELETE_HANDLE): a racing request that reached the
             * ring after the drain must not execute next to DAP_ExecuteCommand()
             * in the WebSocket/elaphureLink/target-flash/curve caller. */
            if (s_data_path_detached) {
                continue;
            }


            packetSize = 0;
            item = (uint8_t *)xRingbufferReceiveUpTo(dap_dataIN_handle, &packetSize,
                                                     pdMS_TO_TICKS(1), DAP_PACKET_SIZE);
            if (packetSize == 0)
            {
                break;
            }

            else if (packetSize != (size_t)DAP_PACKET_SIZE)
            {
                /* Cannot happen with the fixed-size request items and a ring
                 * size that is an exact multiple of the item size; keep the
                 * defensive drop instead of parsing a truncated request. */
                printf("Wrong data in packet size:%u , data in remain: %u\r\n",
                       (unsigned)packetSize,
                       (unsigned)xRingbufferGetMaxItemSize(dap_dataIN_handle));
                vRingbufferReturnItem(dap_dataIN_handle, (void *)item);
                break;
            }

            if (item[0] == ID_DAP_QueueCommands)
            {
                item[0] = ID_DAP_ExecuteCommands;
            }

            resLength = DAP_ProcessCommand(item, DAPDataProcessed);
            resLength &= 0xFFFF; /* res length in lower 16 bits */

            vRingbufferReturnItem(dap_dataIN_handle, (void *)item); // process done.

            /* Host-side clip for an over-long DAP response (P0 in
             * analysis/REPORT.md).  The DAP command layer is responsible for
             * refusing over-long counts; this guard only prevents an over-long
             * response from being handed to the USBIP/WS transports. */
            if (resLength > (int)DAP_PACKET_SIZE) {
                printf("DAP: response length %d exceeds DAP_PACKET_SIZE %u, clipped\r\n",
                       resLength, (unsigned)DAP_PACKET_SIZE);
                resLength = (int)DAP_PACKET_SIZE;
            }

            /* Now prepare to reply (actual length for USBIP/WinUSB).  The
             * response is pushed straight from DAPDataProcessed: no staging
             * packet, no fixed-size ring item (docs/dap-copy-budget.md). */
            if (usbip_pipeline_is_active()) {
                /* B1: pair the response with a parked IN URB and send it from
                 * this task, or store it in the OUT ring until the IN arrives. */
                usbip_pipeline_complete_response(DAPDataProcessed, (uint32_t)resLength);
            } else {
                (void)dap_push_response_data(DAPDataProcessed, (uint32_t)resLength);
            }
        }
    }
}

int fast_reply(uint8_t *buf, uint32_t length, int dap_req_num)
{
    usbip_stage2_header *buf_header = (usbip_stage2_header *)buf;

    if (dap_req_num > 0) {
        void *item;
        size_t reply_len = 0;

        /* The data path is not available while a WebSocket/elaphureLink/
         * target-flash/curve transport owns the DAP engine (DELETE_HANDLE) or
         * when the rings have never been created.  Answer with an empty
         * RET_SUBMIT so the host URB is not left hanging forever. */
        if (dap_dataOUT_handle == NULL || s_data_path_detached) {
            printf("fast_reply: DAP OUT ring buffer not available\r\n");
            buf_header->base.command = PP_HTONL(USBIP_STAGE2_RSP_SUBMIT);
            buf_header->base.direction = PP_HTONL(USBIP_DIR_OUT);
            buf_header->u.ret_submit.status = 0;
            buf_header->u.ret_submit.data_length = 0;
            buf_header->u.ret_submit.error_count = 0;
            return 0;
        }

        /* NOSPLIT response ring: item size is the DAP response length. */
        item = xRingbufferReceive(dap_dataOUT_handle, &reply_len, portMAX_DELAY);
        if (item != NULL) {
            if (reply_len > (size_t)DAP_PACKET_SIZE) {
                printf("Wrong data out packet size:%u!\r\n", (unsigned)reply_len);
                vRingbufferReturnItem(dap_dataOUT_handle, item);
                return 0;
            }
            send_stage2_submit_data_fast((usbip_stage2_header *)buf, item,
                                         (int32_t)reply_len);
            dap_stats_mark_in(); /* A3: response reached the host IN URB */

            if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
                --dap_respond;
                xSemaphoreGive(data_response_mux);
            }

            vRingbufferReturnItem(dap_dataOUT_handle, item);
            return 1;
        }
        ////TODO: fast reply
    } else {
        buf_header->base.command = PP_HTONL(USBIP_STAGE2_RSP_SUBMIT);
        buf_header->base.direction = PP_HTONL(USBIP_DIR_OUT);
        buf_header->u.ret_submit.status = 0;
        buf_header->u.ret_submit.data_length = 0;
        buf_header->u.ret_submit.error_count = 0;
        usbip_network_send(kSock, buf, 48, 0);
        return 1;
    }

    return 0;
}

void handle_dap_unlink()
{
    // `USBIP_CMD_UNLINK` means calling `usb_unlink_urb()` or `usb_kill_urb()`.
    // Note that execution of an URB is inherently an asynchronous operation, and there may be
    // synchronization problems in the following solutions.

    // One of the reasons this happens is that the host wants to abort the URB transfer operation
    // as soon as possible. USBIP network fluctuations will also cause this error, but I don't know
    // whether this is the main reason.

    // Unlink may be applied to zero length URB of "DIR_IN", or a URB containing data.
    // In the case of unlink, for the new "DIR_IN" request, it may always return an older response,
    // which will lead to panic. This code is a compromise for eliminating the lagging response
    // caused by UNLINK. It will clean up the buffers that may have data for return to the host.
    // In general, we assume that there is at most one piece of data that has not yet been returned.
    if (dap_respond > 0 && dap_dataOUT_handle != NULL && !s_data_path_detached)
    {
        void *item;
        size_t item_size = 0;
        item = xRingbufferReceive(dap_dataOUT_handle, &item_size, pdMS_TO_TICKS(10));
        if (item != NULL)
        {
            /* Any stored response item is discarded (NOSPLIT: whole item). */
            if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
            {
                --dap_respond;
                xSemaphoreGive(data_response_mux);
            }

            vRingbufferReturnItem(dap_dataOUT_handle, item);
        }
    }
}

bool dap_handle_task_started(void)
{
    return s_task_started;
}

void dap_handle_note_task_create(bool ok)
{
    if (!ok) {
        snprintf(s_ring_error, sizeof(s_ring_error),
                 "xTaskCreate(DAP_Task) failed free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT |
                                                   MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                            MALLOC_CAP_INTERNAL));
        printf("DAP: %s\r\n", s_ring_error);
    }
}

const char *dap_handle_ring_error(void)
{
    return s_ring_error;
}
