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

bool curve_breakpoint_addr_ok(uint32_t address)
{
    uint32_t normalized = address & ~0x01U; /* Thumb bit0 is ignored */

    return normalized >= CURVE_BP_FLASH_START && normalized <= CURVE_BP_FLASH_END;
}

uint32_t curve_breakpoint_compare_value(uint32_t address)
{
    /* FPB code comparator: address bits [31:1] plus the enable bit (bit0). */
    return (address & ~0x01U) | 0x01U;
}

const char *curve_trigger_name(uint8_t trigger)
{
    if (trigger == (uint8_t)CURVE_TRIGGER_BREAKPOINT) return "breakpoint";
    if (trigger == (uint8_t)CURVE_TRIGGER_WATCHPOINT) return "watchpoint";
    return "periodic";
}

bool curve_pc_in_scope(uint32_t pc_low, uint32_t pc_high, uint32_t pc)
{
    uint32_t pc_norm = pc & ~0x01U;

    return pc_norm >= pc_low && pc_norm < pc_high;
}

bool curve_condition_match(uint32_t raw_value, uint8_t size, bool is_signed,
                           uint8_t op, int32_t value)
{
    uint32_t bits;
    uint32_t mask;
    uint32_t u;
    int32_t s;

    if (size != 1U && size != 2U && size != 4U) {
        return false;
    }
    bits = (uint32_t)size * 8U;
    mask = (size == 4U) ? 0xFFFFFFFFU : ((1UL << bits) - 1U);
    u = raw_value & mask;
    /* Sign-extend when requested (1/2/4 byte little-endian values). */
    s = (int32_t)(u << (32U - bits));
    s >>= (32U - bits);

    switch (op) {
    case CURVE_COND_OP_EQ:
        return is_signed ? (s == value) : (u == (uint32_t)value);
    case CURVE_COND_OP_NE:
        return is_signed ? (s != value) : (u != (uint32_t)value);
    case CURVE_COND_OP_LT:
        return is_signed ? (s < value) : (u < (uint32_t)value);
    case CURVE_COND_OP_LE:
        return is_signed ? (s <= value) : (u <= (uint32_t)value);
    case CURVE_COND_OP_GT:
        return is_signed ? (s > value) : (u > (uint32_t)value);
    case CURVE_COND_OP_GE:
        return is_signed ? (s >= value) : (u >= (uint32_t)value);
    default:
        return false;
    }
}

const char *curve_cond_op_name(uint8_t op)
{
    static const char *names[] = {"eq", "ne", "lt", "le", "gt", "ge"};

    return (op < (uint8_t)(sizeof(names) / sizeof(names[0]))) ? names[op] : "?";
}

bool curve_watchpoint_params_ok(uint32_t address, uint8_t size)
{
    if (size != CURVE_WATCH_SIZE_1 && size != CURVE_WATCH_SIZE_2 &&
        size != CURVE_WATCH_SIZE_4) {
        return false;
    }
    if ((address & ((uint32_t)size - 1U)) != 0U) {
        return false; /* natural alignment required by DWT_MASK matching */
    }
    return curve_addr_range_ok(address, size);
}

const char *curve_watch_access_name(uint8_t access)
{
    return (access == (uint8_t)CURVE_WATCH_ACCESS_RW) ? "readwrite" : "write";
}

uint16_t curve_series_expr_for_pc(const curve_config_t *cfg,
                                  const curve_series_t *series, uint32_t pc)
{
    if (cfg == NULL || series == NULL) {
        return CURVE_EXPR_NONE;
    }
    if ((series->flags & CURVE_SERIES_FLAG_VARIANTS) != 0U) {
        for (uint8_t i = 0U; i < series->variant_count; i++) {
            const curve_variant_t *variant = &cfg->variants[series->variant_first + i];

            if (curve_pc_in_scope(variant->pc_low, variant->pc_high, pc)) {
                return variant->expr;
            }
        }
        return CURVE_EXPR_NONE;
    }
    if ((series->flags & CURVE_SERIES_FLAG_PC_GATE) != 0U &&
        !curve_pc_in_scope(series->pc_low, series->pc_high, pc)) {
        return CURVE_EXPR_NONE;
    }
    return series->expr;
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
