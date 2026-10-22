/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - protocol v2 (CVF2) wire framing.
 *
 * Pure C so the exact binary layout is covered by the host regression test.
 *
 * Batch header (16 B, little-endian):
 *   magic 4B "CVF2" | version u8 = 2 | flags u8 (bit0=gap, bit1=PC_PRESENT)
 *   count u16 | first_seq u32 | frame_bytes u16 (max payload across groups)
 *   | reserved u16
 *   flags bit2 = TRIGGERED: the batch contains at least one trigger-event
 *   frame (DWT watchpoint trigger); per-frame granularity is not in the wire
 *   format, status.trigger_seq identifies the exact sequence number.
 *   flags bit3 = CONDITION: the batch contains at least one frame whose
 *   trigger_condition matched; status.condition_trigger_seq is the exact seq.
 * Frame metadata (15 B, or 19 B when flags.bit1 says PC_PRESENT):
 *   seq u32 | timestamp_us u64 | group u8 | payload_len u16 [| pc u32]
 *   | payload bytes (payload_len)
 * The PC is the halted core's r15 with the Thumb bit0 cleared; no-halt
 * sessions cannot read DCRSR and therefore leave PC_PRESENT clear.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "curve_priv.h"

static size_t put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    return 2U;
}

static size_t put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
    return 4U;
}

static size_t put_u64(uint8_t *p, uint64_t v)
{
    (void)put_u32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    (void)put_u32(p + 4U, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
    return 8U;
}

size_t curve_cvf2_write_header(uint8_t *buf, uint16_t count, uint32_t first_seq,
                               uint16_t frame_bytes, bool gap, bool pc_present,
                               bool triggered, bool condition)
{
    size_t offset = 0U;

    if (buf == NULL) {
        return 0U;
    }
    buf[offset++] = 'C';
    buf[offset++] = 'V';
    buf[offset++] = 'F';
    buf[offset++] = '2';
    buf[offset++] = (uint8_t)CURVE_CVF2_VERSION;
    buf[offset++] = (gap ? (uint8_t)CURVE_CVF2_FLAG_GAP : 0U) |
                    (pc_present ? (uint8_t)CURVE_CVF2_FLAG_PC_PRESENT : 0U) |
                    (triggered ? (uint8_t)CURVE_CVF2_FLAG_TRIGGERED : 0U) |
                    (condition ? (uint8_t)CURVE_CVF2_FLAG_CONDITION : 0U);
    offset += put_u16(buf + offset, count);
    offset += put_u32(buf + offset, first_seq);
    offset += put_u16(buf + offset, frame_bytes);
    offset += put_u16(buf + offset, 0U); /* reserved */
    return offset; /* CURVE_FRAME_HEADER_BYTES */
}

size_t curve_cvf2_write_frame_meta(uint8_t *buf, uint32_t seq,
                                   uint64_t timestamp_us, uint8_t group,
                                   uint16_t payload_len, uint32_t pc,
                                   bool pc_present)
{
    size_t offset = 0U;

    if (buf == NULL) {
        return 0U;
    }
    offset += put_u32(buf + offset, seq);
    offset += put_u64(buf + offset, timestamp_us);
    buf[offset++] = group;
    offset += put_u16(buf + offset, payload_len);
    if (pc_present) {
        offset += put_u32(buf + offset, pc);
    }
    return offset; /* 15 B, or 19 B with PC */
}
