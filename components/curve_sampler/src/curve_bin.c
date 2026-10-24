/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - compact binary start request (CBS1).
 *
 * POST /api/curve/start-bin accepts the same configuration as the JSON
 * /api/curve/start request, but in a compact binary encoding that the HTTP
 * layer can parse STREAMING: the parser pulls from a caller-provided
 * curve_bin_read_fn in small fixed-size pieces (the HTTP handler feeds 256 B
 * chunks straight from the socket).  Consequently this path never needs a
 * 16 KiB contiguous heap block for the request body, which the unified
 * USB-NCM + Wi-Fi image cannot reliably provide once lwIP/TinyUSB have
 * fragmented the internal heap.  JSON v3 remains supported for old clients.
 *
 * Wire format (all little-endian) is documented in curve_priv.h next to
 * CURVE_BIN_MAGIC.  The parser reuses the JSON parser's compact expression
 * grammar (curve_parse_expr_compact) and the shared cross-field validation
 * (curve_config_finalize), so both entry points enforce exactly the same
 * contract limits.
 *
 * The host regression test exercises curve_bin_parse_mem() on a 256-series
 * request and on malformed/truncated bodies.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <esp_err.h>

#include "curve_priv.h"

#define CURVE_BIN_HDR_RESERVED 0U

static esp_err_t bin_fail(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len > 0U) {
        (void)snprintf(err, err_len, "%s", msg);
    }
    return ESP_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------------- */
/* primitive readers                                                         */
/* ------------------------------------------------------------------------- */

static esp_err_t bin_u8(curve_bin_read_fn read_fn, void *ctx, uint8_t *out,
                        char *err, size_t err_len)
{
    return read_fn(ctx, out, 1U) == ESP_OK ? ESP_OK
                                           : bin_fail(err, err_len,
                                                      "truncated CBS1 body");
}

static esp_err_t bin_u16(curve_bin_read_fn read_fn, void *ctx, uint16_t *out,
                         char *err, size_t err_len)
{
    uint8_t b[2];

    if (read_fn(ctx, b, sizeof(b)) != ESP_OK) {
        return bin_fail(err, err_len, "truncated CBS1 body");
    }
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8U));
    return ESP_OK;
}

static esp_err_t bin_u32(curve_bin_read_fn read_fn, void *ctx, uint32_t *out,
                         char *err, size_t err_len)
{
    uint8_t b[4];

    if (read_fn(ctx, b, sizeof(b)) != ESP_OK) {
        return bin_fail(err, err_len, "truncated CBS1 body");
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8U) | ((uint32_t)b[2] << 16U) |
           ((uint32_t)b[3] << 24U);
    return ESP_OK;
}

/** Read a length-prefixed compact expression string and parse it into the pool.
 *  expr_len == 0 is allowed (out_index = CURVE_EXPR_NONE) when the caller
 *  explicitly permits it (loclist variant series or variant entries). */
static esp_err_t bin_expr(curve_bin_read_fn read_fn, void *ctx,
                          curve_config_t *cfg, bool allow_empty,
                          uint16_t *out_index, char *err, size_t err_len)
{
    char text[CURVE_BIN_EXPR_MAX + 1U];
    const char *cursor = text;
    uint16_t len = 0U;
    esp_err_t e;

    *out_index = CURVE_EXPR_NONE;
    e = bin_u16(read_fn, ctx, &len, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    if (len > CURVE_BIN_EXPR_MAX) {
        return bin_fail(err, err_len, "CBS1 expr too long");
    }
    if (len == 0U) {
        if (!allow_empty) {
            return bin_fail(err, err_len, "CBS1 expr missing");
        }
        return ESP_OK;
    }
    if (read_fn(ctx, text, len) != ESP_OK) {
        return bin_fail(err, err_len, "truncated CBS1 body");
    }
    text[len] = '\0';
    if (memchr(text, '\0', len) != NULL) {
        return bin_fail(err, err_len, "CBS1 expr contains NUL");
    }
    e = curve_parse_expr_compact(&cursor, cfg, out_index, 1U, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    if (*cursor != '\0') {
        return bin_fail(err, err_len, "CBS1 expr trailing bytes");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* request sections                                                          */
/* ------------------------------------------------------------------------- */

static esp_err_t bin_parse_watchpoints(curve_bin_read_fn read_fn, void *ctx,
                                       curve_config_t *cfg, uint8_t watch_count,
                                       char *err, size_t err_len)
{
    if (watch_count > CURVE_MAX_WATCHPOINTS) {
        return bin_fail(err, err_len, "CBS1 watch_count > max");
    }
    if (cfg->trigger == (uint8_t)CURVE_TRIGGER_WATCHPOINT && watch_count == 0U) {
        return bin_fail(err, err_len,
                        "trigger=watchpoint requires at least one watchpoint");
    }
    if (cfg->trigger != (uint8_t)CURVE_TRIGGER_WATCHPOINT && watch_count != 0U) {
        return bin_fail(err, err_len, "watchpoints require trigger=watchpoint");
    }
    for (uint8_t slot = 0U; slot < watch_count; slot++) {
        uint32_t address = 0U;
        uint8_t size = 0U;
        uint8_t access = 0U;
        esp_err_t e;

        e = bin_u32(read_fn, ctx, &address, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        e = bin_u8(read_fn, ctx, &size, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        e = bin_u8(read_fn, ctx, &access, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        if (size != CURVE_WATCH_SIZE_1 && size != CURVE_WATCH_SIZE_2 &&
            size != CURVE_WATCH_SIZE_4) {
            return bin_fail(err, err_len, "CBS1 watchpoint size must be 1/2/4");
        }
        if (access > (uint8_t)CURVE_WATCH_ACCESS_READ) {
            return bin_fail(err, err_len, "CBS1 watchpoint access invalid");
        }
        if (!curve_watchpoint_params_ok(address, size)) {
            return bin_fail(err, err_len, "CBS1 watchpoint address invalid");
        }
        for (uint8_t prev = 0U; prev < slot; prev++) {
            if (cfg->watch_addr[prev] == address) {
                return bin_fail(err, err_len,
                                "CBS1 watchpoints duplicate an address");
            }
        }
        cfg->watch_addr[slot] = address;
        cfg->watch_sz[slot] = size;
        cfg->watch_acc[slot] = access;
        if (slot == 0U) {
            cfg->watch_address = address;
            cfg->watch_size = size;
            cfg->watch_access = access;
        } else if (slot == 1U) {
            cfg->watch2_enabled = true;
            cfg->watch2_address = address;
            cfg->watch2_size = size;
            cfg->watch2_access = access;
        }
    }
    cfg->watch_count = watch_count;
    return ESP_OK;
}

static esp_err_t bin_parse_series(curve_bin_read_fn read_fn, void *ctx,
                                  curve_config_t *cfg, curve_group_t *group,
                                  uint16_t *group_sample_bytes, char *err,
                                  size_t err_len)
{
    curve_series_t *dst;

    (void)group; /* series are appended to cfg->series[]; group bounds are set
                  * by bin_parse_groups() */
    uint8_t size = 0U;
    uint8_t flags = 0U;
    uint8_t sample_every = 1U;
    uint8_t variant_count = 0U;
    uint32_t pc_low = 0U;
    uint32_t pc_high = 0U;
    uint16_t expr_index = CURVE_EXPR_NONE;
    esp_err_t e;

    if (cfg->series_count >= CURVE_MAX_SERIES) {
        return bin_fail(err, err_len, "CBS1 total series > max");
    }
    dst = &cfg->series[cfg->series_count];
    memset(dst, 0, sizeof(*dst));
    dst->expr = CURVE_EXPR_NONE;

    e = bin_u8(read_fn, ctx, &size, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &flags, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &sample_every, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &variant_count, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u32(read_fn, ctx, &pc_low, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u32(read_fn, ctx, &pc_high, err, err_len);
    if (e != ESP_OK) {
        return e;
    }

    if (size < CURVE_SERIES_SIZE_MIN || size > CURVE_SERIES_SIZE_MAX) {
        return bin_fail(err, err_len, "CBS1 series size must be 1..8");
    }
    if ((flags & (uint8_t)~CURVE_BIN_SERIES_FLAG_PC_GATE) != 0U) {
        return bin_fail(err, err_len, "CBS1 series flags reserved bits set");
    }
    if (sample_every < CURVE_SAMPLE_EVERY_MIN ||
        sample_every > CURVE_SAMPLE_EVERY_MAX) {
        return bin_fail(err, err_len,
                        "CBS1 series sample_every must be 1..64");
    }
    if (variant_count > CURVE_MAX_VARIANTS_PER_SERIES) {
        return bin_fail(err, err_len, "CBS1 series variants count > max");
    }
    if ((flags & CURVE_BIN_SERIES_FLAG_PC_GATE) != 0U) {
        uint32_t low_norm = 0U;
        uint32_t high_norm = 0U;

        if (!curve_normalize_pc_range(pc_low, pc_high, &low_norm, &high_norm)) {
            return bin_fail(err, err_len, "CBS1 series bad PC gate range");
        }
        dst->flags |= CURVE_SERIES_FLAG_PC_GATE;
        dst->pc_low = low_norm;
        dst->pc_high = high_norm;
    }
    dst->size = size;
    dst->sample_every = sample_every;

    e = bin_expr(read_fn, ctx, cfg, variant_count > 0U, &expr_index, err,
                 err_len);
    if (e != ESP_OK) {
        return e;
    }
    dst->expr = expr_index;
    if (variant_count == 0U && expr_index == CURVE_EXPR_NONE) {
        return bin_fail(err, err_len, "CBS1 series needs expr or variants");
    }

    if (variant_count > 0U) {
        if (cfg->variant_used + variant_count > CURVE_MAX_VARIANTS) {
            return bin_fail(err, err_len, "CBS1 too many loclist variants");
        }
        dst->variant_first = cfg->variant_used;
        dst->variant_count = 0U;
        for (uint8_t vi = 0U; vi < variant_count; vi++) {
            curve_variant_t *slot = &cfg->variants[cfg->variant_used];
            uint32_t v_low = 0U;
            uint32_t v_high = 0U;
            uint32_t low_norm = 0U;
            uint32_t high_norm = 0U;
            uint16_t v_expr = CURVE_EXPR_NONE;

            e = bin_u32(read_fn, ctx, &v_low, err, err_len);
            if (e != ESP_OK) {
                return e;
            }
            e = bin_u32(read_fn, ctx, &v_high, err, err_len);
            if (e != ESP_OK) {
                return e;
            }
            if (!curve_normalize_pc_range(v_low, v_high, &low_norm, &high_norm)) {
                return bin_fail(err, err_len, "CBS1 variant bad PC range");
            }
            e = bin_expr(read_fn, ctx, cfg, false, &v_expr, err, err_len);
            if (e != ESP_OK) {
                return e;
            }
            memset(slot, 0, sizeof(*slot));
            slot->pc_low = low_norm;
            slot->pc_high = high_norm;
            slot->expr = v_expr;
            cfg->variant_used++;
            dst->variant_count++;
        }
        dst->flags |= CURVE_SERIES_FLAG_VARIANTS;
    }

    *group_sample_bytes += size;
    if (*group_sample_bytes > CURVE_MAX_SAMPLE_BYTES) {
        return bin_fail(err, err_len, "CBS1 group sum(size) > 256");
    }
    cfg->series_count++;
    return ESP_OK;
}

static esp_err_t bin_parse_groups(curve_bin_read_fn read_fn, void *ctx,
                                  curve_config_t *cfg, uint8_t group_count,
                                  char *err, size_t err_len)
{
    uint16_t max_sample_bytes = 0U;

    if (group_count < 1U || group_count > CURVE_MAX_GROUPS) {
        return bin_fail(err, err_len, "CBS1 group_count must be 1..8");
    }
    for (uint8_t gi = 0U; gi < group_count; gi++) {
        curve_group_t *group = &cfg->groups[gi];
        uint8_t weight = 1U;
        uint8_t series_count = 0U;
        uint16_t reserved = 0U;
        uint16_t group_sample_bytes = 0U;
        esp_err_t e;

        e = bin_u8(read_fn, ctx, &weight, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        e = bin_u8(read_fn, ctx, &series_count, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        e = bin_u16(read_fn, ctx, &reserved, err, err_len);
        if (e != ESP_OK) {
            return e;
        }
        if (weight < 1U || weight > CURVE_MAX_GROUP_WEIGHT) {
            return bin_fail(err, err_len, "CBS1 group weight must be 1..8");
        }
        if (series_count < 1U || series_count > CURVE_MAX_SERIES_PER_GROUP) {
            return bin_fail(err, err_len,
                            "CBS1 group series count must be 1..64");
        }
        group->series_first = cfg->series_count;
        group->series_count = 0U;
        group->weight = weight;
        for (uint8_t si = 0U; si < series_count; si++) {
            e = bin_parse_series(read_fn, ctx, cfg, group, &group_sample_bytes,
                                 err, err_len);
            if (e != ESP_OK) {
                return e;
            }
            group->series_count++;
        }
        group->sample_bytes = group_sample_bytes;
        if (group_sample_bytes > max_sample_bytes) {
            max_sample_bytes = group_sample_bytes;
        }
    }
    cfg->group_count = group_count;
    cfg->sample_bytes = max_sample_bytes;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t curve_bin_parse(curve_bin_read_fn read_fn, void *ctx,
                          curve_config_t *cfg, char *err, size_t err_len)
{
    uint8_t magic[4];
    uint8_t version = 0U;
    uint8_t flags = 0U;
    uint8_t trigger = 0U;
    uint8_t watch_count = 0U;
    uint32_t interval_us = 0U;
    uint32_t breakpoint_address = 0U;
    uint32_t cond_address = 0U;
    uint32_t cond_value_u = 0U;
    uint8_t cond_size = 0U;
    uint8_t cond_op = 0U;
    uint8_t cond_mode = 0U;
    uint8_t cond_flags = 0U;
    uint8_t snapshot_after = 0U;
    uint8_t group_count = 0U;
    uint16_t reserved = 0U;
    esp_err_t e;

    if (err != NULL && err_len > 0U) {
        err[0] = '\0';
    }
    if (read_fn == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->halt = true;
    cfg->want_pc = true;

    if (read_fn(ctx, magic, sizeof(magic)) != ESP_OK) {
        return bin_fail(err, err_len, "CBS1 truncated header");
    }
    {
        uint32_t magic_le = (uint32_t)magic[0] | ((uint32_t)magic[1] << 8U) |
                            ((uint32_t)magic[2] << 16U) |
                            ((uint32_t)magic[3] << 24U);

        if (magic_le != CURVE_BIN_MAGIC) {
            return bin_fail(err, err_len, "CBS1 bad magic");
        }
    }
    e = bin_u8(read_fn, ctx, &version, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    if (version != CURVE_BIN_VERSION) {
        return bin_fail(err, err_len, "CBS1 unsupported version");
    }
    e = bin_u8(read_fn, ctx, &flags, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    if ((flags & (uint8_t)~0x1FU) != 0U) {
        return bin_fail(err, err_len, "CBS1 header flags reserved bits set");
    }
    e = bin_u8(read_fn, ctx, &trigger, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    if (trigger > (uint8_t)CURVE_TRIGGER_WATCHPOINT) {
        return bin_fail(err, err_len, "CBS1 trigger invalid");
    }
    cfg->trigger = trigger;
    cfg->halt = (flags & CURVE_BIN_FLAG_HALT) != 0U;
    cfg->want_pc = (flags & CURVE_BIN_FLAG_WANT_PC) != 0U;
    cfg->backtrace = (flags & CURVE_BIN_FLAG_BACKTRACE) != 0U;

    e = bin_u8(read_fn, ctx, &watch_count, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u32(read_fn, ctx, &interval_us, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u32(read_fn, ctx, &breakpoint_address, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u32(read_fn, ctx, &cond_address, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u32(read_fn, ctx, &cond_value_u, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &cond_size, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &cond_op, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &cond_mode, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &cond_flags, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &snapshot_after, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u8(read_fn, ctx, &group_count, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_u16(read_fn, ctx, &reserved, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    if (reserved != CURVE_BIN_HDR_RESERVED) {
        return bin_fail(err, err_len, "CBS1 header reserved field != 0");
    }
    if ((cond_flags & (uint8_t)~(CURVE_BIN_COND_FLAG_SIGNED |
                                 CURVE_BIN_COND_FLAG_STOP)) != 0U) {
        return bin_fail(err, err_len, "CBS1 cond_flags reserved bits set");
    }

    /* Trigger-specific header fields (same rules as the JSON parser). */
    if (trigger == (uint8_t)CURVE_TRIGGER_PERIODIC) {
        if (interval_us < CURVE_MIN_INTERVAL_US ||
            interval_us > CURVE_MAX_INTERVAL_US) {
            return bin_fail(err, err_len, "CBS1 interval_us out of range");
        }
        cfg->interval_us = interval_us;
        if (cfg->halt && interval_us < 5000U) {
            return bin_fail(err, err_len,
                            "CBS1 interval_us must be >=5000 when halt=true");
        }
    } else if (trigger == (uint8_t)CURVE_TRIGGER_BREAKPOINT) {
        if (!curve_breakpoint_addr_ok(breakpoint_address)) {
            return bin_fail(err, err_len, "CBS1 bad breakpoint address");
        }
        cfg->breakpoint_address = breakpoint_address;
        if (interval_us < CURVE_BP_INTERVAL_MIN_US ||
            interval_us > CURVE_BP_INTERVAL_MAX_US) {
            return bin_fail(err, err_len,
                            "CBS1 interval_us out of range for breakpoint");
        }
        cfg->interval_us = interval_us;
    } else {
        if (interval_us < CURVE_TRIGGER_WATCH_MIN_US ||
            interval_us > CURVE_TRIGGER_WATCH_MAX_US) {
            return bin_fail(err, err_len,
                            "CBS1 interval_us out of range for watchpoint");
        }
        cfg->interval_us = interval_us;
    }

    if (cfg->backtrace && trigger == (uint8_t)CURVE_TRIGGER_PERIODIC) {
        return bin_fail(err, err_len,
                        "CBS1 backtrace requires breakpoint/watchpoint");
    }
    if ((flags & CURVE_BIN_FLAG_SNAPSHOT) != 0U) {
        if (trigger != (uint8_t)CURVE_TRIGGER_BREAKPOINT) {
            return bin_fail(err, err_len,
                            "CBS1 snapshot_after requires breakpoint");
        }
        if (snapshot_after > CURVE_SNAPSHOT_MAX_AFTER) {
            return bin_fail(err, err_len, "CBS1 snapshot_after > max");
        }
        cfg->snapshot_enabled = true;
        cfg->snapshot_after = snapshot_after;
    }

    if ((flags & CURVE_BIN_FLAG_CONDITION) != 0U) {
        if (cond_size != CURVE_COND_SIZE_MIN && cond_size != 2U &&
            cond_size != CURVE_COND_SIZE_MAX) {
            return bin_fail(err, err_len,
                            "CBS1 trigger_condition.size must be 1/2/4");
        }
        if (cond_op > (uint8_t)CURVE_COND_OP_GE) {
            return bin_fail(err, err_len, "CBS1 trigger_condition.op invalid");
        }
        if (cond_mode > (uint8_t)CURVE_COND_MODE_CONTINUOUS) {
            return bin_fail(err, err_len,
                            "CBS1 trigger_condition.mode invalid");
        }
        if (!curve_addr_range_ok(cond_address, cond_size)) {
            return bin_fail(err, err_len,
                            "CBS1 bad trigger_condition address");
        }
        cfg->cond_enabled = true;
        cfg->cond_address = cond_address;
        cfg->cond_size = cond_size;
        cfg->cond_op = cond_op;
        cfg->cond_mode = cond_mode;
        cfg->cond_value = (int32_t)cond_value_u;
        cfg->cond_signed = (cond_flags & CURVE_BIN_COND_FLAG_SIGNED) != 0U;
        cfg->cond_stop_after = (cond_flags & CURVE_BIN_COND_FLAG_STOP) != 0U;
    }

    e = bin_parse_watchpoints(read_fn, ctx, cfg, watch_count, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    e = bin_parse_groups(read_fn, ctx, cfg, group_count, err, err_len);
    if (e != ESP_OK) {
        return e;
    }
    /* Same cross-field validation/derivation as the JSON path (schedule,
     * needed registers, halt/interval constraints, group PC flags). */
    return curve_config_finalize(cfg, err, err_len);
}

/* ------------------------------------------------------------------------- */
/* in-memory helper (host test / unit use)                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} curve_bin_mem_t;

static esp_err_t bin_mem_read(void *ctx, void *dst, size_t len)
{
    curve_bin_mem_t *mem = (curve_bin_mem_t *)ctx;

    if (mem == NULL || len > mem->len - mem->pos) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, mem->data + mem->pos, len);
    mem->pos += len;
    return ESP_OK;
}

esp_err_t curve_bin_parse_mem(const uint8_t *data, size_t len,
                              curve_config_t *cfg, char *err, size_t err_len)
{
    curve_bin_mem_t mem = {.data = data, .len = len, .pos = 0U};

    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return curve_bin_parse(bin_mem_read, &mem, cfg, err, err_len);
}
