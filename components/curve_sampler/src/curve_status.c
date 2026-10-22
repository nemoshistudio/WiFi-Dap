/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - status JSON formatting.
 *
 * Pure C (no RTOS/ESP dependencies) so the exact GET /api/curve/status payload
 * is covered by the host regression test.  Field set:
 *   Phase 1: ok/running/interval_us/frames/overwritten/invalid/error/
 *            sample_bytes/series_count/halt/link_hz/consecutive_errors/
 *            ring_frames/served_seq/last_tick_us/last_err/message
 *   Phase 2: trigger/breakpoint_address/hits/sampled/throttled
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "curve_priv.h"

int curve_status_format_json(const curve_status_t *status, char *buf, size_t buf_len)
{
    if (status == NULL || buf == NULL || buf_len == 0U) {
        return 0;
    }
    return snprintf(buf, buf_len,
                    "{\"ok\":true,\"running\":%s,\"trigger\":\"%s\","
                    "\"breakpoint_address\":%u,\"hits\":%u,\"sampled\":%u,"
                    "\"throttled\":%u,\"interval_us\":%u,\"frames\":%u,"
                    "\"overwritten\":%u,\"invalid\":%u,\"out_of_scope\":%u,"
                    "\"error\":%u,"
                    "\"sample_bytes\":%u,\"series_count\":%u,\"halt\":%s,"
                    "\"link_hz\":%u,\"consecutive_errors\":%u,"
                    "\"ring_frames\":%u,\"served_seq\":%u,\"last_tick_us\":%u,"
                    "\"last_err\":\"%s\",\"message\":\"%s\"}",
                    status->running ? "true" : "false",
                    curve_trigger_name(status->trigger),
                    (unsigned)status->breakpoint_address, (unsigned)status->hits,
                    (unsigned)status->sampled, (unsigned)status->throttled,
                    (unsigned)status->interval_us, (unsigned)status->frames,
                    (unsigned)status->overwritten, (unsigned)status->invalid,
                    (unsigned)status->out_of_scope, (unsigned)status->errors,
                    (unsigned)status->sample_bytes,
                    (unsigned)status->series_count,
                    status->halt ? "true" : "false", (unsigned)status->link_hz,
                    (unsigned)status->consecutive_errors,
                    (unsigned)status->ring_capacity, (unsigned)status->served_seq,
                    (unsigned)status->last_tick_us, status->last_err,
                    status->message);
}
