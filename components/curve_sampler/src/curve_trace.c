/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - target-UART trace capture.
 *
 * The ESP32 already has a target-UART link owned by components/uart_service
 * (Web UART page + TCP 1346 bridge + WS binary router are its existing
 * clients).  This component subscribes as one more RX consumer and keeps:
 *
 *   - a raw byte ring (CURVE_TRACE_RING_BYTES) served by
 *     GET /api/curve/trace?since=  (legacy raw endpoint, unchanged); and
 *   - P1-13: a line ring (CURVE_TRACE_LINE_MAX lines, each with the ESP32
 *     esp_timer_get_time() timestamp of line completion) served by
 *     GET /api/curve/trace/lines?since=&max= as JSON.  Lines are split on
 *     '\n', '\r' is stripped, non-printable bytes are replaced with '.', and
 *     lines longer than CURVE_TRACE_LINE_TEXT_MAX are split into segments
 *     flagged CURVE_TRACE_LINE_FLAG_TRUNCATED (the target's line boundaries are
 *     preserved as far as the fixed buffer allows).
 *
 * Optional trigger: POST /api/curve/trace/start accepts
 *   {"trigger":"PATTERN","stop_after":true|false}
 * The first completed line containing PATTERN latches triggered/trigger_seq;
 * stop_after ends the capture at that line (the Web can then read the raw ring
 * and the line ring up to the trigger, i.e. pre-trigger context is preserved —
 * the rings always keep history).
 *
 * Deliberately NOT implemented here (prototype boundaries, see
 * docs/curve-local-sampler.md):
 *   - SWO/ITM: SWO_FUNCTION_ENABLE=0 in DAP_config.h and the SWO pin is not
 *     wired to the ESP32, so there is no local SWO path;
 *   - target-side timestamps: t_us is the ESP32 receive/completion time;
 *   - UART line settings: use the existing Web UART module (uart_service) -
 *     this component never reconfigures the UART.
 *
 * Thread model: append runs in the uart_service RX task, start/stop/read run in
 * the httpd task (or the sampler status path).  A mutex serializes all state
 * (never taken in an ISR).  RX callbacks never unsubscribe directly (a
 * stop_after trigger only marks the capture inactive; the next trace stop or
 * trace start does the subscription cleanup in task context).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "uart_service.h"

#include "curve_priv.h"

static struct {
    SemaphoreHandle_t lock;
    bool inited;
    bool active;
    int sub_slot;
    uint8_t storage[CURVE_TRACE_RING_BYTES];
    curve_trace_ring_t ring;
    /* P1-13 line ring + line assembler state (guarded by lock) */
    curve_trace_line_t line_storage[CURVE_TRACE_LINE_MAX];
    curve_trace_line_ring_t lines;
    char line_buf[CURVE_TRACE_LINE_TEXT_MAX + 1U];
    uint32_t line_len;
    bool line_truncated;
    char trigger[CURVE_TRACE_TRIGGER_MAX];
    bool trigger_enabled;
    bool stop_after_trigger;
    bool triggered;
    uint32_t trigger_seq;
} s_trace = {
    .sub_slot = -1,
};

static bool trace_lock_init(void)
{
    if (s_trace.inited) {
        return true;
    }
    s_trace.lock = xSemaphoreCreateMutex();
    if (s_trace.lock == NULL) {
        return false;
    }
    curve_trace_ring_reset(&s_trace.ring, s_trace.storage,
                           CURVE_TRACE_RING_BYTES);
    curve_trace_line_ring_reset(&s_trace.lines, s_trace.line_storage,
                                CURVE_TRACE_LINE_MAX);
    s_trace.line_len = 0U;
    s_trace.line_truncated = false;
    s_trace.trigger[0] = '\0';
    s_trace.inited = true;
    return true;
}

static void trace_lock(void)
{
    (void)xSemaphoreTake(s_trace.lock, portMAX_DELAY);
}

static void trace_unlock(void)
{
    (void)xSemaphoreGive(s_trace.lock);
}

/** Complete the current line (called under the lock): timestamp + trigger. */
static void trace_flush_line_locked(void)
{
    uint64_t t_us = (uint64_t)esp_timer_get_time();
    uint32_t seq;

    s_trace.line_buf[s_trace.line_len] = '\0';
    curve_trace_line_ring_append(&s_trace.lines, t_us, s_trace.line_buf,
                                 s_trace.line_len, s_trace.line_truncated);
    seq = s_trace.lines.total; /* seq of the line just appended */
    if (s_trace.trigger_enabled && !s_trace.triggered &&
        curve_trace_line_match(s_trace.line_buf, s_trace.trigger)) {
        s_trace.triggered = true;
        s_trace.trigger_seq = seq;
        ESP_LOGI(CURVE_TAG, "trace trigger matched at line %u: %s",
                 (unsigned)seq, s_trace.trigger);
        if (s_trace.stop_after_trigger) {
            /* Deferred stop: the RX task must not unsubscribe itself.  The
             * next GET .../lines or POST .../trace/stop observes `active=false`
             * and the Web can stop explicitly. */
            s_trace.active = false;
        }
    }
    s_trace.line_len = 0U;
    s_trace.line_truncated = false;
}

/** Feed one target byte into the line assembler (called under the lock). */
static void trace_feed_line_locked(uint8_t byte)
{
    char c;

    if (byte == (uint8_t)'\n') {
        trace_flush_line_locked();
        return;
    }
    if (byte == (uint8_t)'\r') {
        return; /* strip CR so CRLF targets produce one clean line */
    }
    c = (char)byte;
    if (byte >= 0x20U && byte <= 0x7EU) {
        /* printable ASCII kept as-is */
    } else if (byte == (uint8_t)'\t') {
        c = ' ';
    } else {
        c = '.';
    }
    if (s_trace.line_len >= CURVE_TRACE_LINE_TEXT_MAX) {
        /* Long line: close this segment (flagged truncated) and start the next
         * one with a clean segment timestamp instead of fabricating bytes. */
        s_trace.line_truncated = true;
        trace_flush_line_locked();
        s_trace.line_truncated = true;
    }
    s_trace.line_buf[s_trace.line_len++] = c;
}

/** uart_service RX fan-out callback: runs in the UART RX task, must not block
 *  for long.  Copy the bytes into both rings only. */
static void trace_rx_cb(uint8_t sub_mod, const uint8_t *data, size_t len,
                        void *arg)
{
    (void)sub_mod;
    (void)arg;
    if (data == NULL || len == 0U || !s_trace.inited) {
        return;
    }
    trace_lock();
    if (s_trace.active) {
        curve_trace_ring_append(&s_trace.ring, data, (uint32_t)len);
        for (size_t i = 0U; i < len; i++) {
            trace_feed_line_locked(data[i]);
        }
    }
    trace_unlock();
}

static esp_err_t trace_start_common(const char *trigger, bool stop_after)
{
    esp_err_t err;

    if (!trace_lock_init()) {
        return ESP_ERR_NO_MEM;
    }
    /* Clean up a previous session/subscription first (idempotent, task
     * context, also releases a deferred stop_after trigger). */
    (void)curve_trace_stop();

    trace_lock();
    curve_trace_ring_reset(&s_trace.ring, s_trace.storage,
                           CURVE_TRACE_RING_BYTES);
    curve_trace_line_ring_reset(&s_trace.lines, s_trace.line_storage,
                                CURVE_TRACE_LINE_MAX);
    s_trace.line_len = 0U;
    s_trace.line_truncated = false;
    s_trace.triggered = false;
    s_trace.trigger_seq = 0U;
    s_trace.trigger[0] = '\0';
    s_trace.trigger_enabled = false;
    s_trace.stop_after_trigger = false;
    if (trigger != NULL && trigger[0] != '\0') {
        size_t n = strlen(trigger);

        if (n >= sizeof(s_trace.trigger)) {
            n = sizeof(s_trace.trigger) - 1U;
        }
        memcpy(s_trace.trigger, trigger, n);
        s_trace.trigger[n] = '\0';
        s_trace.trigger_enabled = true;
        s_trace.stop_after_trigger = stop_after;
    }
    trace_unlock();

    /* The UART service is lazily initialized by its first client; subscribing
     * before it exists is supported (the callback records nothing until the
     * service runs).  Init here so TX/RX actually start for a pure trace
     * session. */
    err = uart_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(CURVE_TAG, "trace: uart_service_init failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    trace_lock();
    s_trace.sub_slot = uart_service_subscribe_rx(trace_rx_cb, NULL);
    if (s_trace.sub_slot < 0) {
        trace_unlock();
        ESP_LOGE(CURVE_TAG, "trace: no UART RX subscriber slot (max %d)",
                 (int)UART_SERVICE_MAX_SUBSCRIBERS);
        return ESP_ERR_NO_MEM;
    }
    s_trace.active = true;
    trace_unlock();
    ESP_LOGI(CURVE_TAG,
             "UART trace capture started (%u B raw ring, %u line ring, "
             "trigger=\"%s\"%s)",
             (unsigned)CURVE_TRACE_RING_BYTES, (unsigned)CURVE_TRACE_LINE_MAX,
             s_trace.trigger, s_trace.stop_after_trigger ? ", stop_after" : "");
    return ESP_OK;
}

esp_err_t curve_trace_start(void)
{
    return trace_start_common(NULL, false);
}

esp_err_t curve_trace_start_with_config(const char *trigger, bool stop_after)
{
    return trace_start_common(trigger, stop_after);
}

esp_err_t curve_trace_stop(void)
{
    if (!s_trace.inited) {
        return ESP_OK;
    }
    trace_lock();
    s_trace.active = false;
    s_trace.trigger_enabled = false;
    if (s_trace.sub_slot >= 0) {
        uart_service_unsubscribe_rx(s_trace.sub_slot);
        s_trace.sub_slot = -1;
    }
    s_trace.line_len = 0U;
    s_trace.line_truncated = false;
    trace_unlock();
    ESP_LOGI(CURVE_TAG, "UART trace capture stopped");
    return ESP_OK;
}

void curve_trace_get_status(bool *active, uint32_t *total_bytes,
                            uint32_t *dropped_bytes)
{
    bool is_active = false;
    uint32_t total = 0U;
    uint32_t dropped = 0U;

    if (s_trace.inited) {
        trace_lock();
        is_active = s_trace.active;
        total = s_trace.ring.total;
        dropped = s_trace.ring.total - s_trace.ring.used;
        trace_unlock();
    }
    if (active != NULL) {
        *active = is_active;
    }
    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (dropped_bytes != NULL) {
        *dropped_bytes = dropped;
    }
}

uint32_t curve_trace_read(uint32_t since, uint8_t *out, uint32_t max,
                          uint32_t *first_seq, bool *gap)
{
    curve_trace_seg_t seg[2];
    uint32_t count;
    uint32_t copied = 0U;

    if (first_seq != NULL) {
        *first_seq = 0U;
    }
    if (gap != NULL) {
        *gap = false;
    }
    if (!s_trace.inited || out == NULL || max == 0U) {
        return 0U;
    }
    trace_lock();
    count = curve_trace_ring_plan(&s_trace.ring, since, first_seq, gap, seg);
    if (count > max) {
        count = max;
    }
    for (uint32_t i = 0U; i < 2U && copied < count; i++) {
        uint32_t take = seg[i].len;

        if (take > count - copied) {
            take = count - copied;
        }
        if (take > 0U) {
            memcpy(out + copied, s_trace.storage + seg[i].offset, take);
            copied += take;
        }
    }
    trace_unlock();
    return copied;
}

void curve_trace_lines_get_status(bool *active, uint32_t *total_lines,
                                  uint32_t *dropped_lines, bool *triggered,
                                  uint32_t *trigger_seq)
{
    bool is_active = false;
    uint32_t total = 0U;
    uint32_t dropped = 0U;
    bool trig = false;
    uint32_t trig_seq = 0U;

    if (s_trace.inited) {
        trace_lock();
        is_active = s_trace.active;
        total = s_trace.lines.total;
        dropped = s_trace.lines.total - s_trace.lines.count;
        trig = s_trace.triggered;
        trig_seq = s_trace.trigger_seq;
        trace_unlock();
    }
    if (active != NULL) {
        *active = is_active;
    }
    if (total_lines != NULL) {
        *total_lines = total;
    }
    if (dropped_lines != NULL) {
        *dropped_lines = dropped;
    }
    if (triggered != NULL) {
        *triggered = trig;
    }
    if (trigger_seq != NULL) {
        *trigger_seq = trig_seq;
    }
}

uint32_t curve_trace_lines_read(uint32_t since, uint32_t max,
                                curve_trace_line_t *out, uint32_t *first_seq,
                                bool *gap)
{
    uint32_t count;

    if (first_seq != NULL) {
        *first_seq = 0U;
    }
    if (gap != NULL) {
        *gap = false;
    }
    if (!s_trace.inited || out == NULL || max == 0U) {
        return 0U;
    }
    trace_lock();
    count = curve_trace_line_ring_plan(&s_trace.lines, since, max, out,
                                       first_seq, gap);
    trace_unlock();
    return count;
}
