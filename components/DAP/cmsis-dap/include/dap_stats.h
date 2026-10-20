/*
 * SPDX-License-Identifier: MIT
 *
 * Optional DAP core profiling counters (CONFIG_BIRDIEBOT_DAP_STATS).
 *
 * Used during the OpenOCD/elaphureLink throughput work (W5) to separate the
 * DAP command processing time from the transport round-trip gap and to see
 * which CMSIS-DAP commands OpenOCD actually sends (histogram, batch packing).
 *
 * When disabled, every macro below is a no-op and dap_stats.c is empty.
 */
#ifndef DAP_STATS_H
#define DAP_STATS_H

#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BIRDIEBOT_DAP_STATS

#define DAP_STATS_ENABLED 1

/** Counter snapshot returned by Vendor9 sub-command 1 (little endian). */
typedef struct __attribute__((packed)) {
    uint32_t version;              /* 1 */
    uint32_t uptime_ms;
    uint32_t top_cmds;             /* host commands (one per transport packet) */
    uint32_t proc_us_sum;          /* total time inside DAP processing */
    uint32_t gap_us_sum;           /* total time between command completions */
    uint32_t proc_us_max;
    uint32_t gap_us_max;
    uint32_t last_proc_us;
    uint32_t last_gap_us;
    uint32_t batch_cmds;           /* 0x7E/0x7F batch packets */
    uint32_t batch_subcmds;        /* sub-commands executed from batches */
    uint32_t batch_breaks;         /* batches stopped early by the response cap */
    uint32_t swd_xfers;            /* SWD_Transfer() calls (all modes) */
    uint32_t swd_ok;               /* ACK OK */
    uint32_t swd_wait;             /* ACK WAIT (retried by DAP handlers) */
    uint32_t swd_fault;            /* ACK FAULT */
    uint32_t swd_other;            /* junk/protocol error */
    uint32_t block_read_words;     /* DAP_TransferBlock read words */
    uint32_t block_write_words;    /* DAP_TransferBlock write words */
    uint32_t xfer_cmds;            /* DAP_Transfer commands */
    uint32_t xfer_items;           /* DAP_Transfer items executed */
    uint32_t hist[32];             /* command-id histogram: 0x00..0x1F */
} dap_stats_snapshot_t;

void dap_stats_top_begin(void);
void dap_stats_top_end(uint8_t cmd);
void dap_stats_count_cmd(uint8_t cmd);
void dap_stats_note_batch(uint32_t requested, uint32_t processed, uint8_t hit_cap);
void dap_stats_note_transfer(uint32_t items);
void dap_stats_note_block(uint32_t words, uint8_t is_read);
void dap_stats_note_swd_xfer(void);
void dap_stats_note_swd_result(uint8_t ack);

/** CMSIS-DAP Vendor9 handler: request points after the command id, response
 *  points after the echo byte.  Returns (request bytes << 16) | response bytes
 *  including the echo byte (same convention as DAP_ProcessVendorCommand). */
uint32_t dap_stats_vendor_command(const uint8_t *request, uint8_t *response);

#define DAP_STATS_TOP_BEGIN()          dap_stats_top_begin()
#define DAP_STATS_TOP_END(cmd)         dap_stats_top_end(cmd)
#define DAP_STATS_COUNT_CMD(cmd)       dap_stats_count_cmd(cmd)
#define DAP_STATS_BATCH(req, proc, cap) dap_stats_note_batch((req), (proc), (cap))
#define DAP_STATS_TRANSFER(items)      dap_stats_note_transfer(items)
#define DAP_STATS_BLOCK(words, rd)     dap_stats_note_block((words), (rd))
#define DAP_STATS_SWD_XFER()           dap_stats_note_swd_xfer()
#define DAP_STATS_SWD_RESULT(ack)      dap_stats_note_swd_result(ack)

#else /* CONFIG_BIRDIEBOT_DAP_STATS */

#define DAP_STATS_ENABLED 0
#define DAP_STATS_TOP_BEGIN()          do { } while (0)
#define DAP_STATS_TOP_END(cmd)         ((void)(cmd))
#define DAP_STATS_COUNT_CMD(cmd)       ((void)(cmd))
#define DAP_STATS_BATCH(req, proc, cap) ((void)(req), (void)(proc), (void)(cap))
#define DAP_STATS_TRANSFER(items)      ((void)(items))
#define DAP_STATS_BLOCK(words, rd)     ((void)(words), (void)(rd))
#define DAP_STATS_SWD_XFER()           do { } while (0)
#define DAP_STATS_SWD_RESULT(ack)      ((void)(ack))

#endif /* CONFIG_BIRDIEBOT_DAP_STATS */

#ifdef __cplusplus
}
#endif

#endif /* DAP_STATS_H */
