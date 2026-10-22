/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - POST /api/curve/start JSON parser.
 *
 * Body schema (docs/curve-local-sampler.md §1.1):
 *
 *   {"interval_us":20000,"halt":true,
 *    "series":[{"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}}]}
 *
 * Limits: series 1..16, size 1..8, sum(size) <= 256, interval_us 5000..200000,
 * expression depth <= 8.  The expression schema is identical to the Web UI
 * AddressExpr (addr/reg/cfa/add/deref); parsing into the compact node pool is
 * the only allocation-free representation the sampling worker reads later.
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <esp_err.h>

#include "cJSON.h"
#include "curve_priv.h"

#define CURVE_EXPR_POOL_SIZE ((uint16_t)CURVE_EXPR_POOL_MAX)

static esp_err_t curve_parse_fail(char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL && err_len > 0U) {
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return ESP_ERR_INVALID_ARG;
}

/** Accept an integral JSON number in [min_v, max_v] and return it as u32. */
static bool json_u32(const cJSON *item, double min_v, double max_v, uint32_t *out)
{
    double v;
    uint32_t u;

    if (!cJSON_IsNumber(item)) {
        return false;
    }
    v = item->valuedouble;
    if (!(v >= min_v && v <= max_v)) {
        return false;
    }
    u = (uint32_t)v;
    if ((double)u != v) {
        return false;
    }
    *out = u;
    return true;
}

/** Accept an integral JSON number in [min_v, max_v] and return it as i32. */
static bool json_i32(const cJSON *item, int32_t min_v, int32_t max_v, int32_t *out)
{
    double v;
    int32_t s;

    if (!cJSON_IsNumber(item)) {
        return false;
    }
    v = item->valuedouble;
    if (!(v >= (double)min_v && v <= (double)max_v)) {
        return false;
    }
    s = (int32_t)v;
    if ((double)s != v) {
        return false;
    }
    *out = s;
    return true;
}

static esp_err_t parse_expr(const cJSON *item, curve_config_t *cfg,
                            uint16_t *out_index, uint32_t depth,
                            char *err, size_t err_len)
{
    const cJSON *kind;
    curve_expr_t *node;
    uint16_t index;

    if (depth > CURVE_MAX_EXPR_DEPTH) {
        return curve_parse_fail(err, err_len, "expr depth > %u",
                                (unsigned)CURVE_MAX_EXPR_DEPTH);
    }
    if (!cJSON_IsObject(item)) {
        return curve_parse_fail(err, err_len, "expr must be an object");
    }
    if (cfg->pool_used >= CURVE_EXPR_POOL_SIZE) {
        return curve_parse_fail(err, err_len, "too many expr nodes");
    }
    kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
    if (!cJSON_IsString(kind) || kind->valuestring == NULL) {
        return curve_parse_fail(err, err_len, "expr.kind missing");
    }

    index = cfg->pool_used;
    node = &cfg->pool[cfg->pool_used++];
    memset(node, 0, sizeof(*node));
    node->inner = CURVE_EXPR_NONE;

    if (strcmp(kind->valuestring, "addr") == 0) {
        node->kind = CURVE_EXPR_ADDR;
        if (!json_u32(cJSON_GetObjectItemCaseSensitive(item, "address"), 0.0,
                      4294967295.0, &node->address)) {
            return curve_parse_fail(err, err_len, "addr.address invalid");
        }
    } else if (strcmp(kind->valuestring, "reg") == 0) {
        uint32_t reg = 0U;
        int32_t offset = 0;

        node->kind = CURVE_EXPR_REG;
        if (!json_u32(cJSON_GetObjectItemCaseSensitive(item, "reg"), 0.0,
                      (double)(CURVE_MAX_CORE_REGS - 1U), &reg)) {
            return curve_parse_fail(err, err_len, "reg.reg invalid (0..15)");
        }
        (void)json_i32(cJSON_GetObjectItemCaseSensitive(item, "offset"),
                       INT32_MIN, INT32_MAX, &offset);
        node->reg = (uint8_t)reg;
        node->offset = offset;
    } else if (strcmp(kind->valuestring, "cfa") == 0) {
        int32_t offset = 0;

        node->kind = CURVE_EXPR_CFA;
        (void)json_i32(cJSON_GetObjectItemCaseSensitive(item, "offset"),
                       INT32_MIN, INT32_MAX, &offset);
        node->offset = offset;
    } else if (strcmp(kind->valuestring, "add") == 0 ||
               strcmp(kind->valuestring, "deref") == 0) {
        const cJSON *inner = cJSON_GetObjectItemCaseSensitive(item, "inner");
        int32_t offset = 0;
        esp_err_t e;

        node->kind = (kind->valuestring[0] == 'a') ? CURVE_EXPR_ADD : CURVE_EXPR_DEREF;
        (void)json_i32(cJSON_GetObjectItemCaseSensitive(item, "offset"),
                       INT32_MIN, INT32_MAX, &offset);
        node->offset = offset;
        e = parse_expr(inner, cfg, &node->inner, depth + 1U, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
    } else {
        return curve_parse_fail(err, err_len, "unsupported expr kind %s",
                                kind->valuestring);
    }

    *out_index = index;
    return ESP_OK;
}

/** Add the unique core registers an expression chain needs to a group list. */
static void collect_expr_regs(const curve_config_t *cfg, uint16_t index,
                              curve_group_t *group)
{
    const curve_expr_t *expr;
    uint8_t reg;

    if (index >= cfg->pool_used) {
        return;
    }
    expr = &cfg->pool[index];
    switch (expr->kind) {
    case CURVE_EXPR_REG:
        reg = expr->reg;
        break;
    case CURVE_EXPR_CFA:
        reg = CURVE_CFA_REG;
        break;
    case CURVE_EXPR_ADD:
    case CURVE_EXPR_DEREF:
        collect_expr_regs(cfg, expr->inner, group);
        return;
    default:
        return;
    }
    for (uint8_t i = 0U; i < group->reg_count; i++) {
        if (group->regs[i] == reg) {
            return;
        }
    }
    if (group->reg_count < CURVE_MAX_CORE_REGS) {
        group->regs[group->reg_count++] = reg;
    }
}

/** Normalize a flash [low, high) PC range (Thumb bit0 cleared). */
static bool normalize_pc_range(uint32_t pc_low, uint32_t pc_high,
                               uint32_t *low_norm, uint32_t *high_norm)
{
    uint32_t lo;
    uint32_t hi;

    if (pc_low == 0U || pc_high == 0U || pc_low >= pc_high) {
        return false;
    }
    if (pc_low < CURVE_BP_FLASH_START || pc_low > CURVE_BP_FLASH_END ||
        pc_high < CURVE_BP_FLASH_START || pc_high > CURVE_BP_FLASH_END) {
        return false;
    }
    lo = pc_low & ~0x01U;
    hi = pc_high & ~0x01U;
    if (hi <= lo) {
        /* Degenerate range after clearing the Thumb bits: still scope to the
         * single instruction starting at lo. */
        hi = lo + 2U;
    }
    *low_norm = lo;
    *high_norm = hi;
    return true;
}

/**
 * Parse the optional per-series PC scope [pc_low, pc_high).
 *
 * The gate is enabled when both values are present, both are target-flash
 * addresses and low < high.  Missing/zero/invalid pairs silently leave the
 * series ungated (the contract makes them optional; DWARF-derived ranges are
 * the intended source).
 */
static void parse_pc_scope(const cJSON *series_item, curve_series_t *dst)
{
    uint32_t pc_low = 0U;
    uint32_t pc_high = 0U;
    uint32_t low_norm;
    uint32_t high_norm;

    if (!json_u32(cJSON_GetObjectItemCaseSensitive(series_item, "pc_low"), 0.0,
                  4294967295.0, &pc_low) ||
        !json_u32(cJSON_GetObjectItemCaseSensitive(series_item, "pc_high"), 0.0,
                  4294967295.0, &pc_high)) {
        return;
    }
    if (!normalize_pc_range(pc_low, pc_high, &low_norm, &high_norm)) {
        return;
    }
    dst->flags |= CURVE_SERIES_FLAG_PC_GATE;
    dst->pc_low = low_norm;
    dst->pc_high = high_norm;
}

/**
 * Parse an optional loclist `variants` array: [{"pc_low","pc_high","expr"}].
 * A loclist variable carries one entry per address range; the sampler picks the
 * first variant whose PC range contains the halted core's PC.
 */
static esp_err_t parse_variants(const cJSON *series_item, curve_config_t *cfg,
                                uint16_t series_index, curve_series_t *dst,
                                char *err, size_t err_len)
{
    const cJSON *variants =
        cJSON_GetObjectItemCaseSensitive(series_item, "variants");
    const cJSON *variant;
    uint32_t count;

    if (variants == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(variants)) {
        return curve_parse_fail(err, err_len, "series[%u].variants must be an array",
                                (unsigned)series_index);
    }
    count = (uint32_t)cJSON_GetArraySize(variants);
    if (count < 1U || count > CURVE_MAX_VARIANTS_PER_SERIES) {
        return curve_parse_fail(err, err_len,
                                "series[%u].variants count must be 1..%u",
                                (unsigned)series_index,
                                (unsigned)CURVE_MAX_VARIANTS_PER_SERIES);
    }
    if (cfg->variant_used + count > CURVE_MAX_VARIANTS) {
        return curve_parse_fail(err, err_len,
                                "too many loclist variants (max %u)",
                                (unsigned)CURVE_MAX_VARIANTS);
    }
    dst->variant_first = cfg->variant_used;
    dst->variant_count = 0U;

    cJSON_ArrayForEach(variant, variants) {
        curve_variant_t *slot = &cfg->variants[cfg->variant_used];
        const cJSON *expr_item;
        uint32_t pc_low = 0U;
        uint32_t pc_high = 0U;
        uint32_t low_norm;
        uint32_t high_norm;
        uint16_t expr_index = CURVE_EXPR_NONE;
        esp_err_t e;

        if (!cJSON_IsObject(variant)) {
            return curve_parse_fail(err, err_len,
                                    "series[%u].variants[%u] must be an object",
                                    (unsigned)series_index,
                                    (unsigned)dst->variant_count);
        }
        if (!json_u32(cJSON_GetObjectItemCaseSensitive(variant, "pc_low"), 0.0,
                      4294967295.0, &pc_low) ||
            !json_u32(cJSON_GetObjectItemCaseSensitive(variant, "pc_high"), 0.0,
                      4294967295.0, &pc_high) ||
            !normalize_pc_range(pc_low, pc_high, &low_norm, &high_norm)) {
            return curve_parse_fail(err, err_len,
                                    "series[%u].variants[%u] has a bad PC range "
                                    "(flash 0x08000000..0x08FFFFFF, low<high)",
                                    (unsigned)series_index,
                                    (unsigned)dst->variant_count);
        }
        expr_item = cJSON_GetObjectItemCaseSensitive(variant, "expr");
        if (expr_item == NULL || !cJSON_IsObject(expr_item)) {
            return curve_parse_fail(err, err_len,
                                    "series[%u].variants[%u].expr missing",
                                    (unsigned)series_index,
                                    (unsigned)dst->variant_count);
        }
        e = parse_expr(expr_item, cfg, &expr_index, 1U, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        memset(slot, 0, sizeof(*slot));
        slot->pc_low = low_norm;
        slot->pc_high = high_norm;
        slot->expr = expr_index;
        cfg->variant_used++;
        dst->variant_count++;
    }
    dst->flags |= CURVE_SERIES_FLAG_VARIANTS;
    return ESP_OK;
}

/** Parse one series object into cfg->series[cfg->series_count]. */
static esp_err_t parse_series(const cJSON *item, curve_config_t *cfg,
                              uint16_t global_index, uint16_t *sample_bytes,
                              char *err, size_t err_len)
{
    curve_series_t *dst;
    const cJSON *expr_item;
    uint32_t id = global_index;
    uint32_t size = 0U;
    uint16_t expr_index = CURVE_EXPR_NONE;
    bool has_variants;
    esp_err_t e;

    if (!cJSON_IsObject(item)) {
        return curve_parse_fail(err, err_len, "series[%u] must be an object",
                                (unsigned)global_index);
    }
    if (cfg->series_count >= CURVE_MAX_SERIES) {
        return curve_parse_fail(err, err_len, "total series count > %u",
                                (unsigned)CURVE_MAX_SERIES);
    }
    dst = &cfg->series[cfg->series_count];
    memset(dst, 0, sizeof(*dst));
    dst->expr = CURVE_EXPR_NONE;

    (void)json_u32(cJSON_GetObjectItemCaseSensitive(item, "id"), 0.0,
                   4294967295.0, &id);
    if (!json_u32(cJSON_GetObjectItemCaseSensitive(item, "size"),
                  (double)CURVE_SERIES_SIZE_MIN,
                  (double)CURVE_SERIES_SIZE_MAX, &size)) {
        return curve_parse_fail(err, err_len, "series[%u].size must be 1..8",
                                (unsigned)global_index);
    }

    expr_item = cJSON_GetObjectItemCaseSensitive(item, "expr");
    if (expr_item != NULL && cJSON_IsObject(expr_item)) {
        e = parse_expr(expr_item, cfg, &expr_index, 1U, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        dst->expr = expr_index;
    }
    e = parse_variants(item, cfg, global_index, dst, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    has_variants = (dst->flags & CURVE_SERIES_FLAG_VARIANTS) != 0U;
    if (!has_variants && dst->expr == CURVE_EXPR_NONE) {
        return curve_parse_fail(err, err_len,
                                "series[%u] needs expr or variants",
                                (unsigned)global_index);
    }
    parse_pc_scope(item, dst);

    dst->id = id;
    dst->size = (uint8_t)size;
    *sample_bytes += (uint16_t)size;
    if (*sample_bytes > CURVE_MAX_SAMPLE_BYTES) {
        return curve_parse_fail(err, err_len, "group sum(size) > %u",
                                (unsigned)CURVE_MAX_SAMPLE_BYTES);
    }
    cfg->series_count++;
    return ESP_OK;
}

/** Parse one group's series array (1..32) into cfg->series[]. */
static esp_err_t parse_series_array(const cJSON *series_array,
                                    curve_config_t *cfg, curve_group_t *group,
                                    uint16_t *global_sample_bytes,
                                    char *err, size_t err_len)
{
    const cJSON *item;
    uint32_t count;
    uint16_t group_sample_bytes = 0U;
    esp_err_t e;

    if (!cJSON_IsArray(series_array)) {
        return curve_parse_fail(err, err_len, "group series must be an array");
    }
    count = (uint32_t)cJSON_GetArraySize(series_array);
    if (count < 1U || count > CURVE_MAX_SERIES_PER_GROUP) {
        return curve_parse_fail(err, err_len, "group series count must be 1..%u",
                                (unsigned)CURVE_MAX_SERIES_PER_GROUP);
    }
    group->series_first = cfg->series_count;
    group->series_count = 0U;

    cJSON_ArrayForEach(item, series_array) {
        e = parse_series(item, cfg, (uint16_t)cfg->series_count,
                         &group_sample_bytes, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        group->series_count++;
    }
    group->sample_bytes = group_sample_bytes;
    if (group_sample_bytes > *global_sample_bytes) {
        *global_sample_bytes = group_sample_bytes;
    }
    return ESP_OK;
}

esp_err_t curve_config_parse(const char *json, size_t len, curve_config_t *cfg,
                             char *err, size_t err_len)
{
    cJSON *root;
    const cJSON *groups_item;
    const cJSON *item;
    const cJSON *group;
    uint32_t interval_us = 0U;
    uint32_t group_count = 0U;
    uint16_t max_sample_bytes = 0U;
    esp_err_t result = ESP_OK;

    if (cfg == NULL) {
        return curve_parse_fail(err, err_len, "no config");
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->halt = true;

    if (json == NULL || len == 0U) {
        return curve_parse_fail(err, err_len, "empty body");
    }
    root = cJSON_Parse(json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return curve_parse_fail(err, err_len, "invalid JSON object");
    }

    /* Phase 2 trigger selector: "periodic" (default) or "breakpoint". */
    item = cJSON_GetObjectItemCaseSensitive(root, "trigger");
    if (item != NULL) {
        if (!cJSON_IsString(item) || item->valuestring == NULL) {
            result = curve_parse_fail(err, err_len,
                                      "trigger must be \"periodic\" or \"breakpoint\"");
            goto out;
        }
        if (strcmp(item->valuestring, "breakpoint") == 0) {
            cfg->trigger = (uint8_t)CURVE_TRIGGER_BREAKPOINT;
        } else if (strcmp(item->valuestring, "periodic") == 0) {
            cfg->trigger = (uint8_t)CURVE_TRIGGER_PERIODIC;
        } else {
            result = curve_parse_fail(err, err_len,
                                      "trigger must be \"periodic\" or \"breakpoint\"");
            goto out;
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "interval_us");
    if (cfg->trigger == (uint8_t)CURVE_TRIGGER_BREAKPOINT) {
        const cJSON *bp_item =
            cJSON_GetObjectItemCaseSensitive(root, "breakpoint_address");
        uint32_t bp_address = 0U;

        if (!json_u32(bp_item, 0.0, 4294967295.0, &bp_address) ||
            !curve_breakpoint_addr_ok(bp_address)) {
            result = curve_parse_fail(err, err_len, "bad breakpoint address");
            goto out;
        }
        cfg->breakpoint_address = bp_address;
        /* interval_us is the throttle floor; optional, 5 ms..1 s. */
        if (item == NULL) {
            cfg->interval_us = CURVE_BP_INTERVAL_DEFAULT_US;
        } else if (!json_u32(item, (double)CURVE_BP_INTERVAL_MIN_US,
                             (double)CURVE_BP_INTERVAL_MAX_US, &interval_us)) {
            result = curve_parse_fail(err, err_len,
                                      "interval_us must be %u..%u for breakpoint",
                                      (unsigned)CURVE_BP_INTERVAL_MIN_US,
                                      (unsigned)CURVE_BP_INTERVAL_MAX_US);
            goto out;
        } else {
            cfg->interval_us = interval_us;
        }
    } else {
        if (!json_u32(item, (double)CURVE_MIN_INTERVAL_US,
                      (double)CURVE_MAX_INTERVAL_US, &interval_us)) {
            result = curve_parse_fail(err, err_len, "interval_us must be %u..%u",
                                      (unsigned)CURVE_MIN_INTERVAL_US,
                                      (unsigned)CURVE_MAX_INTERVAL_US);
            goto out;
        }
        cfg->interval_us = interval_us;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "halt");
    if (item != NULL) {
        if (!cJSON_IsBool(item)) {
            result = curve_parse_fail(err, err_len, "halt must be a boolean");
            goto out;
        }
        cfg->halt = cJSON_IsTrue(item) ? true : false;
    }

    /* Periodic + halt: a debug tick costs ~1-3 ms; below 5 ms the target would
     * be halted nearly continuously. Only the no-halt fast path accepts 1 ms. */
    if (cfg->trigger == (uint8_t)CURVE_TRIGGER_PERIODIC && cfg->halt &&
        cfg->interval_us < 5000U) {
        result = curve_parse_fail(err, err_len,
                                  "interval_us must be >=5000 when halt=true");
        goto out;
    }

    /* Protocol v2: `groups` arrays, or a legacy root `series` single group. */
    groups_item = cJSON_GetObjectItemCaseSensitive(root, "groups");
    if (cJSON_IsArray(groups_item)) {
        uint32_t parsed_groups = 0U;

        group_count = (uint32_t)cJSON_GetArraySize(groups_item);
        if (group_count < 1U || group_count > CURVE_MAX_GROUPS) {
            result = curve_parse_fail(err, err_len, "groups count must be 1..%u",
                                      (unsigned)CURVE_MAX_GROUPS);
            goto out;
        }
        cJSON_ArrayForEach(group, groups_item) {
            const cJSON *series_array;

            if (!cJSON_IsObject(group)) {
                result = curve_parse_fail(err, err_len,
                                          "groups[%u] must be an object",
                                          (unsigned)parsed_groups);
                goto out;
            }
            series_array = cJSON_GetObjectItemCaseSensitive(group, "series");
            result = parse_series_array(series_array, cfg,
                                        &cfg->groups[parsed_groups],
                                        &max_sample_bytes, err, err_len);
            if (result != ESP_OK) {
                goto out;
            }
            parsed_groups++;
        }
        cfg->group_count = (uint8_t)parsed_groups;
    } else {
        const cJSON *series_array =
            cJSON_GetObjectItemCaseSensitive(root, "series");

        if (!cJSON_IsArray(series_array)) {
            result = curve_parse_fail(err, err_len,
                                      "groups or series must be an array");
            goto out;
        }
        if ((uint32_t)cJSON_GetArraySize(series_array) >
            CURVE_MAX_SERIES_PER_GROUP) {
            result = curve_parse_fail(err, err_len, "series count must be 1..%u",
                                      (unsigned)CURVE_MAX_SERIES_PER_GROUP);
            goto out;
        }
        result = parse_series_array(series_array, cfg, &cfg->groups[0],
                                    &max_sample_bytes, err, err_len);
        if (result != ESP_OK) {
            goto out;
        }
        cfg->group_count = 1U;
    }

    cfg->sample_bytes = max_sample_bytes;
    cfg->needs_regs = false;
    cfg->pc_gate_count = 0U;
    for (uint8_t g = 0U; g < cfg->group_count; g++) {
        curve_group_t *group_cfg = &cfg->groups[g];
        uint16_t first = group_cfg->series_first;
        uint16_t last = (uint16_t)(first + group_cfg->series_count);

        group_cfg->pc_needed = false;
        group_cfg->reg_count = 0U;
        for (uint16_t si = first; si < last; si++) {
            const curve_series_t *series = &cfg->series[si];

            if (curve_series_needs_pc(series)) {
                group_cfg->pc_needed = true;
                cfg->pc_gate_count++;
            }
            if ((series->flags & CURVE_SERIES_FLAG_VARIANTS) != 0U) {
                for (uint8_t vi = 0U; vi < series->variant_count; vi++) {
                    collect_expr_regs(cfg,
                                      cfg->variants[series->variant_first + vi].expr,
                                      group_cfg);
                }
            } else {
                collect_expr_regs(cfg, series->expr, group_cfg);
            }
        }
        if (group_cfg->reg_count > 0U) {
            cfg->needs_regs = true;
        }
    }

    /* reg/cfa expressions need the core halted: periodic mode must halt and
     * loclist variants are only meaningful when the PC is readable.  breakpoint
     * mode always samples on a halted core. */
    if (cfg->trigger == (uint8_t)CURVE_TRIGGER_PERIODIC && !cfg->halt &&
        cfg->needs_regs) {
        result = curve_parse_fail(err, err_len,
                                  "halt=false cannot sample reg/cfa expressions");
        goto out;
    }
    if (cfg->variant_used > 0U &&
        cfg->trigger == (uint8_t)CURVE_TRIGGER_PERIODIC && !cfg->halt) {
        result = curve_parse_fail(err, err_len, "variants require halt=true");
        goto out;
    }

out:
    cJSON_Delete(root);
    return result;
}
