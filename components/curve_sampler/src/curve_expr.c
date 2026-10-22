/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - address expression evaluation.
 *
 * The expression schema is identical to the Web UI AddressExpr
 * (web/src/utils/elfTypes.ts) and evaluated with the same semantics as
 * web/src/utils/dwarfLocation.ts: evaluateAddress():
 *
 *   addr    -> literal 32-bit address
 *   reg     -> core register value + offset
 *   cfa     -> SP (r13) + offset
 *   add     -> inner address + offset
 *   deref   -> u32 loaded from inner address, plus offset (pointer chain)
 *
 * Register values and pointer words come from the local SWD link; a pointer
 * load that leaves the allowed STM32H7 memory windows is reported INVALID
 * (no bus access at all), a failed AHB-AP read is reported BUS so the caller
 * can run tf_swd_recover_bus().
 */
#include <stdbool.h>
#include <stdint.h>

#include "curve_priv.h"

typedef struct {
    uint32_t start;
    uint32_t end_exclusive;
} curve_region_t;

static const curve_region_t s_regions[CURVE_REGION_COUNT] = {
    {0x20000000U, 0x20020000U}, /* DTCM (STM32H7) / SRAM (STM32F4) */
    {0x24000000U, 0x24080000U}, /* AXI SRAM                        */
    {0x08000000U, 0x09000000U}, /* main flash                      */
};

bool curve_addr_range_ok(uint32_t addr, uint32_t size)
{
    uint64_t last;
    uint32_t first_word;
    uint32_t last_word;
    size_t i;

    if (size == 0U || size > CURVE_MAX_SAMPLE_BYTES) {
        return false;
    }
    last = (uint64_t)addr + (uint64_t)size - 1U;
    if (last > 0xFFFFFFFFULL) {
        return false;
    }
    /* tf_swd_read_mem() aligns sub-word accesses down to a 32-bit word, so the
     * whole aligned span must be mapped or the AP would FAULT on a hole. */
    first_word = addr & ~0x03U;
    last_word = ((uint32_t)last) & ~0x03U;
    for (i = 0U; i < CURVE_REGION_COUNT; i++) {
        if (first_word >= s_regions[i].start &&
            (uint64_t)last_word + 4U <= (uint64_t)s_regions[i].end_exclusive) {
            return true;
        }
    }
    return false;
}

static curve_eval_result_t eval_index(const curve_config_t *cfg, uint16_t index,
                                      const uint32_t *reg_values,
                                      const bool *reg_valid, uint32_t *address)
{
    const curve_expr_t *expr;
    curve_eval_result_t res;
    uint32_t inner;
    uint32_t pointer = 0U;

    if (cfg == NULL || index >= cfg->pool_used) {
        return CURVE_EVAL_INVALID;
    }
    expr = &cfg->pool[index];

    switch (expr->kind) {
    case CURVE_EXPR_ADDR:
        *address = expr->address;
        return CURVE_EVAL_OK;

    case CURVE_EXPR_REG:
        if (expr->reg >= CURVE_MAX_CORE_REGS || !reg_valid[expr->reg]) {
            return CURVE_EVAL_INVALID;
        }
        *address = reg_values[expr->reg] + (uint32_t)expr->offset;
        return CURVE_EVAL_OK;

    case CURVE_EXPR_CFA:
        if (!reg_valid[CURVE_CFA_REG]) {
            return CURVE_EVAL_INVALID;
        }
        *address = reg_values[CURVE_CFA_REG] + (uint32_t)expr->offset;
        return CURVE_EVAL_OK;

    case CURVE_EXPR_ADD:
        res = eval_index(cfg, expr->inner, reg_values, reg_valid, &inner);
        if (res != CURVE_EVAL_OK) {
            return res;
        }
        *address = inner + (uint32_t)expr->offset;
        return CURVE_EVAL_OK;

    case CURVE_EXPR_DEREF:
        res = eval_index(cfg, expr->inner, reg_values, reg_valid, &inner);
        if (res != CURVE_EVAL_OK) {
            return res;
        }
        if (!curve_addr_range_ok(inner, 4U)) {
            return CURVE_EVAL_INVALID;
        }
        if (tf_swd_read_mem(inner, &pointer, sizeof(pointer)) != ESP_OK) {
            return CURVE_EVAL_BUS;
        }
        *address = pointer + (uint32_t)expr->offset;
        return CURVE_EVAL_OK;

    default:
        return CURVE_EVAL_INVALID;
    }
}

curve_eval_result_t curve_expr_eval(const curve_config_t *cfg, uint16_t expr_index,
                                    const uint32_t *reg_values,
                                    const bool *reg_valid,
                                    uint32_t *address)
{
    if (cfg == NULL || reg_values == NULL || reg_valid == NULL || address == NULL) {
        return CURVE_EVAL_INVALID;
    }
    return eval_index(cfg, expr_index, reg_values, reg_valid, address);
}
