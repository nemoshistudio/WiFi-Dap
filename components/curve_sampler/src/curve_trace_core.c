/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - target-UART trace ring (pure C).
 *
 * The board's existing target-UART link (components/uart_service) is tapped by
 * curve_trace.c; this file only owns the byte ring and the read planning so the
 * wrap/gap/eviction behavior is host-testable without an ESP32 or a target.
 *
 * Usage: the UART RX callback appends bytes; the HTTP layer asks for the bytes
 * newer than the Web client's last seen byte sequence (`since`).  The ring
 * keeps at most `capacity` bytes; older bytes are reported as a gap.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "curve_priv.h"

void curve_trace_ring_reset(curve_trace_ring_t *ring, uint8_t *storage,
                            uint32_t capacity)
{
    if (ring == NULL) {
        return;
    }
    ring->storage = storage;
    ring->capacity = (storage != NULL) ? capacity : 0U;
    ring->head = 0U;
    ring->used = 0U;
    ring->total = 0U;
}

void curve_trace_ring_append(curve_trace_ring_t *ring, const uint8_t *data,
                             uint32_t len)
{
    if (ring == NULL || data == NULL || len == 0U) {
        return;
    }
    if (ring->capacity == 0U || ring->storage == NULL) {
        ring->total += len; /* no storage: report everything as evicted */
        return;
    }
    if (len >= ring->capacity) {
        /* Only the newest `capacity` bytes fit: remember them, reset the ring
         * to a linear state at offset 0 so the plan stays simple. */
        memcpy(ring->storage, data + (len - ring->capacity), ring->capacity);
        ring->head = 0U;
        ring->used = ring->capacity;
        ring->total += len;
        return;
    }
    {
        uint32_t first = ring->capacity - ring->head;

        if (first > len) {
            first = len;
        }
        memcpy(ring->storage + ring->head, data, first);
        if (first < len) {
            memcpy(ring->storage, data + first, len - first);
        }
        ring->head = (ring->head + len) % ring->capacity;
        ring->used += len;
        if (ring->used > ring->capacity) {
            ring->used = ring->capacity;
        }
        ring->total += len;
    }
}

uint32_t curve_trace_ring_plan(const curve_trace_ring_t *ring, uint32_t since,
                               uint32_t *first_seq, bool *gap,
                               curve_trace_seg_t seg[2])
{
    uint32_t oldest;
    uint32_t from;
    uint32_t count;
    uint32_t pos;

    if (first_seq == NULL || gap == NULL || seg == NULL) {
        return 0U;
    }
    *first_seq = 0U;
    *gap = false;
    seg[0].offset = 0U;
    seg[0].len = 0U;
    seg[1].offset = 0U;
    seg[1].len = 0U;
    if (ring == NULL || ring->capacity == 0U || ring->storage == NULL ||
        ring->used == 0U) {
        return 0U;
    }
    oldest = ring->total - ring->used;
    if (since >= ring->total) {
        /* `since` is the next expected byte seq already (or in the future):
         * nothing to send. */
        return 0U;
    }
    if (since == 0U || since < oldest) {
        *gap = (since != 0U);
        from = oldest;
    } else {
        from = since;
    }
    count = ring->total - from;
    if (count == 0U) {
        return 0U;
    }
    if (count > ring->used) {
        count = ring->used;
    }
    *first_seq = from;
    /* byte sequence `oldest` lives at (head - used) mod capacity. */
    pos = (ring->head + ring->capacity - ring->used) % ring->capacity;
    pos = (pos + (from - oldest)) % ring->capacity;
    seg[0].offset = pos;
    seg[0].len = count;
    if (pos + count > ring->capacity) {
        seg[0].len = ring->capacity - pos;
        seg[1].offset = 0U;
        seg[1].len = count - seg[0].len;
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* P1-13: line-buffered trace (timestamps + trigger)                         */
/* ------------------------------------------------------------------------- */

void curve_trace_line_ring_reset(curve_trace_line_ring_t *ring,
                                 curve_trace_line_t *storage, uint32_t capacity)
{
    if (ring == NULL) {
        return;
    }
    ring->storage = storage;
    ring->capacity = (storage != NULL) ? capacity : 0U;
    ring->head = 0U;
    ring->count = 0U;
    ring->total = 0U;
}

void curve_trace_line_ring_append(curve_trace_line_ring_t *ring, uint64_t t_us,
                                  const char *text, uint32_t len,
                                  bool truncated)
{
    curve_trace_line_t *slot;

    if (ring == NULL || ring->storage == NULL || ring->capacity == 0U) {
        return;
    }
    slot = &ring->storage[ring->head];
    memset(slot, 0, sizeof(*slot));
    slot->seq = ring->total + 1U;
    slot->t_us = t_us;
    if (len > CURVE_TRACE_LINE_TEXT_MAX) {
        len = CURVE_TRACE_LINE_TEXT_MAX;
        truncated = true;
    }
    if (text != NULL && len > 0U) {
        memcpy(slot->text, text, len);
    }
    slot->len = (uint8_t)len;
    slot->flags = truncated ? CURVE_TRACE_LINE_FLAG_TRUNCATED : 0U;
    ring->head = (ring->head + 1U) % ring->capacity;
    if (ring->count < ring->capacity) {
        ring->count++;
    }
    ring->total++;
}

uint32_t curve_trace_line_ring_plan(const curve_trace_line_ring_t *ring,
                                    uint32_t since, uint32_t max,
                                    curve_trace_line_t *out,
                                    uint32_t *first_seq, bool *gap)
{
    uint32_t oldest;
    uint32_t from;
    uint32_t count;
    uint32_t start;

    if (first_seq == NULL || gap == NULL) {
        return 0U;
    }
    *first_seq = 0U;
    *gap = false;
    if (ring == NULL || ring->storage == NULL || ring->capacity == 0U ||
        ring->count == 0U || out == NULL || max == 0U) {
        return 0U;
    }
    /* Line sequences are 1-based: seq 1 is the first completed line, so the
     * next expected seq after `total` lines is total+1. */
    oldest = ring->total + 1U - ring->count;
    if (since >= ring->total + 1U) {
        return 0U; /* caller already has every completed line */
    }
    if (since == 0U || since < oldest) {
        *gap = (since != 0U);
        from = oldest;
    } else {
        from = since;
    }
    count = ring->total + 1U - from;
    if (count > ring->count) {
        count = ring->count;
    }
    if (count > max) {
        count = max;
    }
    if (count == 0U) {
        return 0U;
    }
    *first_seq = from;
    /* line seq `oldest` lives at (head - count) mod capacity */
    start = (ring->head + ring->capacity - ring->count) % ring->capacity;
    start = (start + (from - oldest)) % ring->capacity;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t slot = (start + i) % ring->capacity;

        out[i] = ring->storage[slot];
        /* Keep the payload string NUL-terminated for JSON/formatting users. */
        out[i].text[out[i].len] = '\0';
    }
    return count;
}

bool curve_trace_line_match(const char *text, const char *pattern)
{
    if (text == NULL || pattern == NULL || pattern[0] == '\0') {
        return false;
    }
    return strstr(text, pattern) != NULL;
}
