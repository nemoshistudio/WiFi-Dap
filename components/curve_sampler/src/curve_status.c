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
                    "\"group_count\":%u,\"current_group\":%u,"
                    "\"breakpoint_address\":%u,\"hits\":%u,\"sampled\":%u,"
                    "\"throttled\":%u,\"interval_us\":%u,\"frames\":%u,"
                    "\"overwritten\":%u,\"invalid\":%u,\"out_of_scope\":%u,"
                    "\"pc_available\":%s,\"last_pc\":%u,"
                    "\"watchpoint_armed\":%s,\"watchpoint_address\":%u,"
                    "\"watchpoint_hits\":%u,\"trigger_seq\":%u,"
                    "\"condition_armed\":%s,\"condition_met\":%s,"
                    "\"condition_hits\":%u,\"condition_trigger_seq\":%u,"
                    "\"max_groups\":%u,\"max_series\":%u,"
                    "\"max_series_per_group\":%u,\"expr_pool_max\":%u,"
                    "\"expr_pool_used\":%u,\"error\":%u,"
                    "\"sample_bytes\":%u,\"series_count\":%u,\"halt\":%s,"
                    "\"link_hz\":%u,\"consecutive_errors\":%u,"
                    "\"ring_frames\":%u,\"served_seq\":%u,\"last_tick_us\":%u,"
                    "\"last_err\":\"%s\",\"message\":\"%s\"}",
                    status->running ? "true" : "false",
                    curve_trigger_name(status->trigger),
                    (unsigned)status->group_count,
                    (unsigned)status->current_group,
                    (unsigned)status->breakpoint_address, (unsigned)status->hits,
                    (unsigned)status->sampled, (unsigned)status->throttled,
                    (unsigned)status->interval_us, (unsigned)status->frames,
                    (unsigned)status->overwritten, (unsigned)status->invalid,
                    (unsigned)status->out_of_scope,
                    status->pc_available ? "true" : "false",
                    (unsigned)status->last_pc,
                    status->watchpoint_armed ? "true" : "false",
                    (unsigned)status->watchpoint_address,
                    (unsigned)status->watchpoint_hits,
                    (unsigned)status->trigger_seq,
                    status->condition_armed ? "true" : "false",
                    status->condition_met ? "true" : "false",
                    (unsigned)status->condition_hits,
                    (unsigned)status->condition_trigger_seq,
                    (unsigned)status->max_groups,
                    (unsigned)status->max_series,
                    (unsigned)status->max_series_per_group,
                    (unsigned)status->expr_pool_max,
                    (unsigned)status->expr_pool_used,
                    (unsigned)status->errors,
                    (unsigned)status->sample_bytes,
                    (unsigned)status->series_count,
                    status->halt ? "true" : "false", (unsigned)status->link_hz,
                    (unsigned)status->consecutive_errors,
                    (unsigned)status->ring_capacity, (unsigned)status->served_seq,
                    (unsigned)status->last_tick_us, status->last_err,
                    status->message);
}
