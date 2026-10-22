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

#define CURVE_EXPR_POOL_SIZE \
    ((uint16_t)(sizeof(((curve_config_t *)0)->pool) / sizeof(curve_expr_t)))

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

static void collect_expr_regs(const curve_config_t *cfg, uint16_t index,
                              curve_config_t *mutable_cfg)
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
        collect_expr_regs(cfg, expr->inner, mutable_cfg);
        return;
    default:
        return;
    }
    for (uint8_t i = 0U; i < mutable_cfg->reg_count; i++) {
        if (mutable_cfg->regs[i] == reg) {
            return;
        }
    }
    if (mutable_cfg->reg_count < CURVE_MAX_CORE_REGS) {
        mutable_cfg->regs[mutable_cfg->reg_count++] = reg;
    }
}

esp_err_t curve_config_parse(const char *json, size_t len, curve_config_t *cfg,
                             char *err, size_t err_len)
{
    cJSON *root;
    const cJSON *series;
    const cJSON *item;
    uint32_t interval_us = 0U;
    uint32_t series_count = 0U;
    uint32_t sample_bytes = 0U;
    uint32_t index = 0U;
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

    if (!json_u32(cJSON_GetObjectItemCaseSensitive(root, "interval_us"),
                  (double)CURVE_MIN_INTERVAL_US, (double)CURVE_MAX_INTERVAL_US,
                  &interval_us)) {
        result = curve_parse_fail(err, err_len, "interval_us must be %u..%u",
                                  (unsigned)CURVE_MIN_INTERVAL_US,
                                  (unsigned)CURVE_MAX_INTERVAL_US);
        goto out;
    }
    cfg->interval_us = interval_us;

    item = cJSON_GetObjectItemCaseSensitive(root, "halt");
    if (item != NULL) {
        if (!cJSON_IsBool(item)) {
            result = curve_parse_fail(err, err_len, "halt must be a boolean");
            goto out;
        }
        cfg->halt = cJSON_IsTrue(item) ? true : false;
    }

    series = cJSON_GetObjectItemCaseSensitive(root, "series");
    if (!cJSON_IsArray(series)) {
        result = curve_parse_fail(err, err_len, "series must be an array");
        goto out;
    }
    series_count = (uint32_t)cJSON_GetArraySize(series);
    if (series_count < 1U || series_count > CURVE_MAX_SERIES) {
        result = curve_parse_fail(err, err_len, "series count must be 1..%u",
                                  (unsigned)CURVE_MAX_SERIES);
        goto out;
    }
    cfg->series_count = (uint8_t)series_count;

    cJSON_ArrayForEach(item, series) {
        curve_series_t *dst = &cfg->series[index];
        const cJSON *size_item;
        const cJSON *expr_item;
        uint32_t id = index;
        uint32_t size = 0U;

        if (!cJSON_IsObject(item)) {
            result = curve_parse_fail(err, err_len, "series[%u] must be an object",
                                      (unsigned)index);
            goto out;
        }
        (void)json_u32(cJSON_GetObjectItemCaseSensitive(item, "id"), 0.0,
                       4294967295.0, &id);
        size_item = cJSON_GetObjectItemCaseSensitive(item, "size");
        if (!json_u32(size_item, (double)CURVE_SERIES_SIZE_MIN,
                      (double)CURVE_SERIES_SIZE_MAX, &size)) {
            result = curve_parse_fail(err, err_len, "series[%u].size must be 1..8",
                                      (unsigned)index);
            goto out;
        }
        expr_item = cJSON_GetObjectItemCaseSensitive(item, "expr");
        if (expr_item == NULL || !cJSON_IsObject(expr_item)) {
            result = curve_parse_fail(err, err_len, "series[%u].expr missing",
                                      (unsigned)index);
            goto out;
        }
        dst->id = id;
        dst->size = (uint8_t)size;
        result = parse_expr(expr_item, cfg, &dst->expr, 1U, err, err_len);
        if (result != ESP_OK) {
            goto out;
        }
        sample_bytes += size;
        if (sample_bytes > CURVE_MAX_SAMPLE_BYTES) {
            result = curve_parse_fail(err, err_len, "sum(size) > %u",
                                      (unsigned)CURVE_MAX_SAMPLE_BYTES);
            goto out;
        }
        index++;
    }

    cfg->sample_bytes = (uint16_t)sample_bytes;
    cfg->needs_regs = false;
    for (uint8_t i = 0U; i < cfg->series_count; i++) {
        collect_expr_regs(cfg, cfg->series[i].expr, cfg);
    }
    cfg->needs_regs = (cfg->reg_count != 0U);

    if (!cfg->halt && cfg->needs_regs) {
        result = curve_parse_fail(err, err_len,
                                  "halt=false cannot sample reg/cfa expressions");
        goto out;
    }

out:
    cJSON_Delete(root);
    return result;
}
