/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - frame-pointer backtrace prototype.
 *
 * Prototype only, NOT a DWARF-CFI unwinder.  At a breakpoint/watchpoint hit the
 * core is halted; the engine reads r7 (AAPCS frame pointer) and the chain is
 * followed through the AHB-AP:
 *
 *   [fp + 0] = previous frame pointer (higher address, stack grows down)
 *   [fp + 4] = return address (Thumb bit0 set)
 *
 * A chain entry is only accepted when the return address is inside target flash
 * and the previous frame pointer is 4-byte aligned and strictly ascending
 * (stack grows downward, so a caller frame is at a higher address) and inside
 * an allowed target memory window (checked by the engine's read callback).
 *
 * Boundaries (documented in docs/curve-local-sampler.md):
 *   - requires frame pointers: -O0 or -fno-omit-frame-pointer; optimized code
 *     that reuses r7 (e.g. -O2 without frame pointers) yields a short/invalid
 *     chain, not a wrong-but-plausible one;
 *   - no .debug_frame/.eh_frame CFI, no inline-frame expansion, no task/RTOS
 *     awareness (only the currently interrupted context is unwound);
 *   - only the last trigger hit is kept (8 PCs) and the capture costs a few
 *     SWD word reads while the core is halted.
 *
 * Pure C, no IDF/FreeRTOS dependency: the same code is compiled by the host
 * regression test with a mock target-memory reader.
 */
#include <stdbool.h>
#include <stdint.h>

#include "curve_priv.h"

#define CURVE_BACKTRACE_PC_MIN      0x08000000U
#define CURVE_BACKTRACE_PC_MAX      0x08FFFFFFU

static uint32_t backtrace_normalize_pc(uint32_t pc)
{
    return pc & ~0x01U; /* Thumb bit0 is not part of the address */
}

static bool backtrace_pc_in_flash(uint32_t pc)
{
    uint32_t normalized = backtrace_normalize_pc(pc);

    return normalized >= CURVE_BACKTRACE_PC_MIN &&
           normalized <= CURVE_BACKTRACE_PC_MAX;
}

void curve_backtrace_reset(curve_backtrace_t *bt)
{
    if (bt == NULL) {
        return;
    }
    bt->count = 0U;
    bt->truncated = false;
    for (uint8_t i = 0U; i < CURVE_BACKTRACE_MAX_FRAMES; i++) {
        bt->pcs[i] = 0U;
    }
}

bool curve_backtrace_capture(uint32_t pc, uint32_t fp,
                             curve_backtrace_read_fn read, void *ctx,
                             curve_backtrace_t *out)
{
    if (out == NULL) {
        return false;
    }
    curve_backtrace_reset(out);
    if (read == NULL || !backtrace_pc_in_flash(pc)) {
        return false;
    }
    out->pcs[out->count++] = backtrace_normalize_pc(pc);

    while (out->count < CURVE_BACKTRACE_MAX_FRAMES) {
        uint32_t prev_fp = 0U;
        uint32_t ret = 0U;

        if ((fp & 0x03U) != 0U) {
            break;
        }
        if (!read(ctx, fp, &prev_fp) || !read(ctx, fp + 4U, &ret)) {
            break;
        }
        /* Return addresses on Cortex-M are Thumb (bit0 set) and live in flash.
         * Anything else means the chain is not a frame-pointer chain (e.g. the
         * function was built without frame pointers): stop instead of
         * reporting a bogus frame. */
        if ((ret & 0x01U) == 0U || !backtrace_pc_in_flash(ret)) {
            break;
        }
        /* Stack grows down: a caller frame is at a strictly higher address.
         * Reject self/backwards links (recursion without frame pointers would
         * loop forever) and implausible jumps. */
        if (prev_fp <= fp || (prev_fp & 0x03U) != 0U) {
            break;
        }
        if ((prev_fp - fp) > 0x100000U) {
            break;
        }
        out->pcs[out->count++] = backtrace_normalize_pc(ret);
        fp = prev_fp;
    }

    if (out->count == CURVE_BACKTRACE_MAX_FRAMES) {
        /* The chain may continue beyond the captured window.  Probe one more
         * link so callers can distinguish "exactly 8 frames" from "truncated";
         * a failed probe is not an error. */
        uint32_t prev_fp = 0U;
        uint32_t ret = 0U;

        out->truncated = (fp & 0x03U) == 0U && read(ctx, fp, &prev_fp) &&
                         read(ctx, fp + 4U, &ret) && (ret & 0x01U) != 0U &&
                         backtrace_pc_in_flash(ret) && prev_fp > fp &&
                         (prev_fp & 0x03U) == 0U;
    }
    return out->count > 0U;
}
