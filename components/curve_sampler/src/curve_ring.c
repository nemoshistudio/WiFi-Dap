/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - frame ring bookkeeping.
 *
 * The engine keeps the frame payload arrays (seq/timestamp/payload) in a single
 * heap block; this module owns the sequence/count/served bookkeeping so it can
 * be unit-tested on the host without FreeRTOS/ESP dependencies.
 *
 * overwritten semantics (fixed after the first hardware Phase 1 test): the ring
 * always retains the newest `capacity` frames, so once the sampling has run for
 * longer than capacity ticks every new frame evicts an old one.  A frame only
 * counts as `overwritten` when it was evicted *before any client received it*
 * (evicted_seq > served_seq).  A client that polls `frames?since=<last seq>`
 * fast enough never increments `overwritten`, even though frames are physically
 * recycled every tick.  `overwritten` therefore means "frames the client may
 * have missed", which is what the Web UI displays.
 *
 * `gap` (CVF1 flags bit0) keeps its contract meaning: the client's `since` is
 * older than the oldest retained frame, so the returned batch starts at the
 * oldest retained frame instead.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "curve_priv.h"

uint32_t curve_ring_capacity_for(uint32_t frame_bytes)
{
    uint32_t stride;
    uint32_t capacity;

    if (frame_bytes == 0U || frame_bytes > CURVE_MAX_SAMPLE_BYTES) {
        return 0U;
    }
    stride = CURVE_FRAME_META_BYTES + frame_bytes;
    /* Budget first: large payloads get fewer slots instead of a larger heap
     * allocation.  The 16-frame floor is only a sanity guard (at 256 B/frame
     * the 8 KiB budget still allows 30 frames). */
    capacity = CURVE_RING_BYTES_BUDGET / stride;
    if (capacity < CURVE_RING_FRAMES_MIN) {
        capacity = CURVE_RING_FRAMES_MIN;
    }
    if (capacity > CURVE_RING_FRAMES_MAX) {
        capacity = CURVE_RING_FRAMES_MAX;
    }
    return capacity;
}

void curve_ring_reset(curve_ring_t *ring, uint32_t capacity)
{
    if (ring == NULL) {
        return;
    }
    ring->capacity = capacity;
    ring->next_seq = 1U;
    ring->count = 0U;
    ring->served_seq = 0U;
    ring->frames_total = 0U;
    ring->overwritten = 0U;
}

void curve_ring_begin_write(curve_ring_t *ring, uint32_t *slot, uint32_t *seq)
{
    if (ring == NULL || slot == NULL || seq == NULL || ring->capacity == 0U) {
        if (slot != NULL) {
            *slot = 0U;
        }
        if (seq != NULL) {
            *seq = 0U;
        }
        return;
    }
    if (ring->count >= ring->capacity) {
        /* The new frame reuses the slot of the oldest retained frame. */
        uint32_t evicted_seq = ring->next_seq - ring->capacity;

        if (evicted_seq > ring->served_seq) {
            /* The client never received that frame: a real drop. */
            ring->overwritten++;
        }
    }
    *slot = (ring->next_seq - 1U) % ring->capacity;
    *seq = ring->next_seq;
}

void curve_ring_commit_write(curve_ring_t *ring)
{
    if (ring == NULL || ring->capacity == 0U) {
        return;
    }
    if (ring->count < ring->capacity) {
        ring->count++;
    }
    ring->next_seq++;
    ring->frames_total++;
}

uint32_t curve_ring_plan_read(const curve_ring_t *ring, uint32_t since,
                              uint32_t max, uint32_t *first_seq, bool *gap)
{
    uint32_t latest;
    uint32_t oldest;
    uint32_t first;
    uint32_t count;

    if (ring == NULL || first_seq == NULL || gap == NULL) {
        return 0U;
    }
    *gap = false;
    if (ring->capacity == 0U || ring->count == 0U || max == 0U) {
        return 0U;
    }
    latest = ring->next_seq - 1U;
    oldest = ring->next_seq - ring->count;
    if (since != 0U) {
        if (since >= latest) {
            return 0U; /* nothing newer than the client's cursor */
        }
        first = since + 1U;
    } else {
        /* since=0: start at the oldest retained frame. */
        first = oldest;
    }
    if (first < oldest) {
        first = oldest;
        *gap = true; /* the requested frames have already been overwritten */
    }
    if (first > latest) {
        return 0U;
    }
    count = latest - first + 1U;
    if (count > max) {
        count = max;
    }
    *first_seq = first;
    return count;
}

void curve_ring_mark_served(curve_ring_t *ring, uint32_t first_seq, uint32_t count)
{
    uint32_t last_seq;

    if (ring == NULL || count == 0U) {
        return;
    }
    last_seq = first_seq + count - 1U;
    if (last_seq > ring->served_seq) {
        ring->served_seq = last_seq;
    }
}
