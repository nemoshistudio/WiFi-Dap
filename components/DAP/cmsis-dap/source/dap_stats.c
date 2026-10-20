/*
 * SPDX-License-Identifier: MIT
 *
 * Optional DAP core profiling counters for the W5 OpenOCD throughput work.
 * Compiled to nothing unless CONFIG_BIRDIEBOT_DAP_STATS is enabled.
 */
#include "sdkconfig.h"

#if CONFIG_BIRDIEBOT_DAP_STATS

#include <string.h>

#include <esp_timer.h>

#include "cmsis-dap/include/DAP.h"
#include "cmsis-dap/include/dap_stats.h"

static dap_stats_snapshot_t s_stats;
static int64_t s_top_start_us;
static int64_t s_last_end_us;
static int64_t s_first_us;

static int64_t dap_stats_now(void)
{
    int64_t now = esp_timer_get_time();

    if (s_first_us == 0) {
        s_first_us = now;
    }
    return now;
}

void dap_stats_top_begin(void)
{
    int64_t now = dap_stats_now();
    int64_t gap;

    s_top_start_us = now;
    if (s_last_end_us != 0 && now > s_last_end_us) {
        gap = now - s_last_end_us;
        if (gap > (int64_t)UINT32_MAX) {
            gap = UINT32_MAX;
        }
        if (gap > 5000000) {
            gap = 0; /* long idle between sessions: do not skew the average */
        }
        s_stats.gap_us_sum += (uint32_t)gap;
        if ((uint32_t)gap > s_stats.gap_us_max) {
            s_stats.gap_us_max = (uint32_t)gap;
        }
        s_stats.last_gap_us = (uint32_t)gap;
    } else {
        s_stats.last_gap_us = 0U;
    }
}

void dap_stats_top_end(uint8_t cmd)
{
    int64_t now = dap_stats_now();
    int64_t dur = now - s_top_start_us;

    (void)cmd;
    if (dur < 0) {
        dur = 0;
    }
    if (dur > (int64_t)UINT32_MAX) {
        dur = UINT32_MAX;
    }
    s_stats.top_cmds++;
    s_stats.proc_us_sum += (uint32_t)dur;
    if ((uint32_t)dur > s_stats.proc_us_max) {
        s_stats.proc_us_max = (uint32_t)dur;
    }
    s_stats.last_proc_us = (uint32_t)dur;
    s_last_end_us = now;
}

void dap_stats_count_cmd(uint8_t cmd)
{
    s_stats.hist[cmd & 0x1FU]++;
}

void dap_stats_note_batch(uint32_t requested, uint32_t processed, uint8_t hit_cap)
{
    s_stats.batch_cmds++;
    s_stats.batch_subcmds += processed;
    if (hit_cap != 0U && processed < requested) {
        s_stats.batch_breaks++;
    }
}

void dap_stats_note_transfer(uint32_t items)
{
    s_stats.xfer_cmds++;
    s_stats.xfer_items += items;
}

void dap_stats_note_block(uint32_t words, uint8_t is_read)
{
    if (is_read != 0U) {
        s_stats.block_read_words += words;
    } else {
        s_stats.block_write_words += words;
    }
}

void dap_stats_note_swd_xfer(void)
{
    s_stats.swd_xfers++;
}

void dap_stats_note_swd_result(uint8_t ack)
{
    switch (ack) {
    case DAP_TRANSFER_OK:
        s_stats.swd_ok++;
        break;
    case DAP_TRANSFER_WAIT:
        s_stats.swd_wait++;
        break;
    case DAP_TRANSFER_FAULT:
        s_stats.swd_fault++;
        break;
    default:
        s_stats.swd_other++;
        break;
    }
}

uint32_t dap_stats_vendor_command(const uint8_t *request, uint8_t *response)
{
    uint8_t sub = request[0];

    if (sub == 0U) {
        /* Reset counters. */
        memset(&s_stats, 0, sizeof(s_stats));
        s_first_us = 0;
        s_last_end_us = 0;
        response[0] = 0x00U; /* DAP_OK */
        return ((2U << 16) | 2U);
    }

    if (sub == 1U) {
        int64_t now = dap_stats_now();

        s_stats.version = 2U;
        s_stats.uptime_ms = (uint32_t)((now - s_first_us) / 1000);
        memcpy(response, &s_stats, sizeof(s_stats));
        return ((2U << 16) | (uint32_t)(1U + sizeof(s_stats)));
    }

    response[0] = 0x01U; /* DAP_ERROR */
    return ((2U << 16) | 2U);
}

#endif /* CONFIG_BIRDIEBOT_DAP_STATS */
