/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - sampling engine.
 *
 * A dedicated worker task (pinned to the DAP/SWD core, 8 KiB stack, below the
 * httpd priority) owns the target for one sampling session:
 *
 *   start:  tcp_server_kick_session() -> dap_handle_request_reset(DELETE_HANDLE)
 *           + dap_handle_wait_reset() -> tf_swd_link_init() (2/1/0.5 MHz retry)
 *           -> periodic esp_timer tick -> sample loop
 *   tick:   sample the current group: [halt] -> read needed core regs ->
 *           evaluate expressions -> read series bytes over the AHB-AP ->
 *           [resume] -> append one CVF2 frame to the ring -> round-robin the
 *           next group
 *   stop:   stop the tick timer -> resume the target -> release the DAP
 *           (RESET_HANDLE) -> idle
 *
 * The HTTP layer only parses configuration / copies frames; it never runs the
 * DAP command path on the httpd task.  A sample tick that hits an SWD/AP error
 * runs tf_swd_recover_bus(); CURVE_MAX_CONSECUTIVE_ERRORS failed ticks stop the
 * session, always after resuming the target and releasing the DAP.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include "DAP_handle.h"
#include "tcp_server.h"
#include "wt_target_flash.h"

#include "curve_priv.h"

/* ------------------------------------------------------------------------- */
/* engine state                                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    CURVE_CMD_NONE = 0,
    CURVE_CMD_START,
    CURVE_CMD_SELFTEST,
} curve_cmd_t;

typedef enum {
    CURVE_WORKER_IDLE = 0,
    CURVE_WORKER_BUSY,
} curve_worker_state_t;

/* The SWD bit-bang uses the core-1 dedicated GPIO signals (see tf_swd.c /
 * components/DAP), so the worker must run on the DAP core.  The target-flash
 * worker does the same. */
#if defined(portNUM_PROCESSORS) && (portNUM_PROCESSORS > 1)
#define CURVE_TASK_CORE 1
#else
#define CURVE_TASK_CORE 0
#endif
#define CURVE_TASK_STACK 8192U
#define CURVE_TASK_PRIO  4 /* below httpd (5) / DAP (16): preemption is safe */

static struct {
    SemaphoreHandle_t lock;      /* config / ring / stats / state snapshot */
    SemaphoreHandle_t done_sem;  /* command completed (bring-up/selftest)   */
    SemaphoreHandle_t idle_sem;  /* worker back to idle                     */
    TaskHandle_t task;
    esp_timer_handle_t tick_timer;

    volatile curve_cmd_t cmd;
    volatile bool stop_req;
    curve_worker_state_t worker_state;
    bool running;
    uint8_t current_group;       /* round-robin sampling cursor (v2) */
    bool dap_held;               /* worker task only */

    const curve_config_t *cfg;   /* owned by HTTP layer; immutable while running */
    uint32_t link_hz;
    uint32_t dpidr;
    esp_err_t cmd_result;
    char message[64];

    /* ring (heap, allocated/reused on start; dropped only when the frame size
     * changes or the component is never started again) */
    uint8_t *ring_block;
    uint16_t ring_frame_bytes;
    uint32_t ring_capacity;
    uint32_t *ring_seq;
    uint64_t *ring_ts;
    uint8_t *ring_payload;
    uint8_t *ring_group;         /* CVF2 per-frame group index */
    uint16_t *ring_len;          /* CVF2 per-frame payload_len */
    curve_ring_t ring;           /* seq/count/served/overwritten bookkeeping */

    /* Phase 2: FPB breakpoint state (worker task only) */
    bool fpb_active;
    uint32_t fpb_orig_ctrl;
    uint32_t fpb_orig_comp0;
    uint32_t fpb_compare;

    /* statistics reported by GET /api/curve/status */
    uint32_t stat_invalid;
    uint32_t stat_out_of_scope;
    uint32_t stat_errors;
    uint32_t stat_consecutive;
    uint32_t stat_last_tick_us;
    uint32_t stat_last_error;
    uint32_t stat_hits;        /* FPB breakpoint hits */
    uint32_t stat_sampled;     /* frames written for breakpoint hits */
    uint32_t stat_throttled;   /* hits dropped by throttle (reserved: always 0) */

    curve_selftest_t selftest;
} s_engine;

static bool s_inited;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void engine_lock(void)
{
    (void)xSemaphoreTake(s_engine.lock, portMAX_DELAY);
}

static void engine_unlock(void)
{
    (void)xSemaphoreGive(s_engine.lock);
}

/** Set the diagnostic message shown by status / returned by start. */
static void engine_set_message(const char *fmt, ...)
{
    va_list ap;

    engine_lock();
    va_start(ap, fmt);
    (void)vsnprintf(s_engine.message, sizeof(s_engine.message), fmt, ap);
    va_end(ap);
    engine_unlock();
}

/** Target flash sessions and local curve sampling share the same DAP engine:
 *  refuse to start while the flash engine owns it. (The reverse direction is
 *  covered by polling wt_tf_get_status() from the sample loop; the two
 *  components have no shared mutex - see README "known limitations".) */
static bool engine_target_flash_busy(void)
{
    wt_tf_status_t st;

    wt_tf_get_status(&st);
    switch (st.state) {
    case WT_TF_ERASING:
    case WT_TF_PROGRAMMING:
    case WT_TF_VERIFYING:
    case WT_TF_RESETTING:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* DAP ownership (copied from components/target_flash/src/wt_target_flash.c) */
/* ------------------------------------------------------------------------- */

static esp_err_t curve_dap_acquire(void)
{
    if (s_engine.dap_held) {
        return ESP_OK;
    }
    /* Drop a WebSocket/USBIP-DAP session first so it cannot race the hand-off;
     * then take the engine like target flash does. */
    tcp_server_kick_session();
    vTaskDelay(pdMS_TO_TICKS(CURVE_KICK_SETTLE_MS));
    dap_handle_request_reset(DELETE_HANDLE);
    if (dap_handle_wait_reset(CURVE_DAP_HANDOFF_TIMEOUT_MS) != 0) {
        ESP_LOGE(CURVE_TAG, "DAP hand-off timed out (%u ms)",
                 (unsigned)CURVE_DAP_HANDOFF_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    s_engine.dap_held = true;
    ESP_LOGI(CURVE_TAG, "DAP acquired for curve sampling");
    return ESP_OK;
}

static void curve_dap_release(void)
{
    if (!s_engine.dap_held) {
        return;
    }
    dap_handle_request_reset(RESET_HANDLE);
    (void)dap_handle_wait_reset(CURVE_DAP_RESTORE_TIMEOUT_MS);
    s_engine.dap_held = false;
    ESP_LOGI(CURVE_TAG, "DAP released (USBIP/WS data path restored)");
}

/** Link the DAP to SWD; H7 boards in the field only answer at <= 1 MHz. */
static esp_err_t curve_link_up(uint32_t *clock_hz, uint32_t *dpidr)
{
    static const uint32_t clock_try[] = {2000000U, 1000000U, 500000U};
    esp_err_t last_err = ESP_ERR_NOT_FOUND;

    for (size_t i = 0U; i < (sizeof(clock_try) / sizeof(clock_try[0])); i++) {
        uint32_t id = 0U;
        esp_err_t err = tf_swd_link_init(clock_try[i], &id);

        if (err == ESP_OK) {
            *clock_hz = clock_try[i];
            *dpidr = id;
            return ESP_OK;
        }
        last_err = err;
        ESP_LOGW(CURVE_TAG, "SWD link init @%u Hz failed (%s)",
                 (unsigned)clock_try[i], esp_err_to_name(err));
        (void)tf_swd_recover_bus();
    }
    return last_err;
}

/** Release the core without tf_swd_resume()'s 10 ms vTaskDelay settle: the
 *  per-tick path must stay well below the 5 ms minimum sampling interval. */
static esp_err_t curve_resume_core(void)
{
    return tf_swd_write_u32(CURVE_REG_DHCSR,
                            CURVE_DHCSR_DBGKEY | CURVE_DHCSR_C_DEBUGEN);
}

/** Best-effort release of a core we halted: one retry after clearing a
 *  possible AP/DP sticky error, so stop/error paths do not leave the target
 *  halted after the DAP has been handed back. */
static void curve_release_core(void)
{
    if (curve_resume_core() != ESP_OK) {
        (void)tf_swd_recover_bus();
        (void)curve_resume_core();
    }
}

static esp_err_t curve_bringup(void)
{
    esp_err_t err;
    uint32_t clock_hz = 0U;
    uint32_t dpidr = 0U;

    err = curve_dap_acquire();
    if (err != ESP_OK) {
        return err;
    }
    err = curve_link_up(&clock_hz, &dpidr);
    if (err != ESP_OK) {
        return err;
    }
    engine_lock();
    s_engine.link_hz = clock_hz;
    s_engine.dpidr = dpidr;
    s_engine.stat_last_error = 0;
    engine_unlock();
    ESP_LOGI(CURVE_TAG, "SWD link up @%u Hz: DPIDR=0x%08X",
             (unsigned)clock_hz, (unsigned)dpidr);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* ring buffer                                                               */
/* ------------------------------------------------------------------------- */

static esp_err_t engine_ring_prepare(uint16_t frame_bytes)
{
    size_t seq_bytes;
    size_t ts_bytes;
    size_t group_bytes;
    size_t len_bytes;
    size_t payload_bytes;
    size_t total_bytes;
    uint32_t capacity;
    uint8_t *block;
    size_t offset;
    uint32_t attempt_capacity;

    if (frame_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    capacity = curve_ring_capacity_for(frame_bytes);
    if (capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_engine.ring_block != NULL && s_engine.ring_frame_bytes == frame_bytes &&
        s_engine.ring_capacity == capacity) {
        ESP_LOGI(CURVE_TAG, "reusing ring (%u frames x %u B)",
                 (unsigned)capacity, (unsigned)frame_bytes);
    } else {
        /* Large payloads (v2: up to 8 groups x 256 B) can exceed the fragmented
         * WiFi heap even though the nominal 32 KiB budget looks fine.  Free the
         * old ring first and, on allocation failure, halve the frame count down
         * to 32 frames instead of failing the whole start request. */
        free(s_engine.ring_block);
        s_engine.ring_block = NULL;
        s_engine.ring_seq = NULL;
        s_engine.ring_ts = NULL;
        s_engine.ring_group = NULL;
        s_engine.ring_len = NULL;
        s_engine.ring_payload = NULL;
        s_engine.ring_frame_bytes = 0U;
        s_engine.ring_capacity = 0U;
        block = NULL;
        attempt_capacity = capacity;
        while (attempt_capacity >= 8U) {
            seq_bytes = sizeof(uint32_t) * (size_t)attempt_capacity;
            ts_bytes = sizeof(uint64_t) * (size_t)attempt_capacity;
            group_bytes = sizeof(uint8_t) * (size_t)attempt_capacity;
            len_bytes = sizeof(uint16_t) * (size_t)attempt_capacity;
            payload_bytes = (size_t)frame_bytes * (size_t)attempt_capacity;
            total_bytes = seq_bytes + ts_bytes + group_bytes + len_bytes +
                          payload_bytes;
            block = (uint8_t *)malloc(total_bytes);
            if (block != NULL) {
                capacity = attempt_capacity;
                break;
            }
            attempt_capacity /= 2U;
        }
        if (block == NULL) {
            ESP_LOGE(CURVE_TAG, "ring allocation failed (%u frames x %u B)",
                     (unsigned)capacity, (unsigned)frame_bytes);
            return ESP_ERR_NO_MEM;
        }
        offset = 0U;
        s_engine.ring_seq = (uint32_t *)block;
        offset += seq_bytes;
        s_engine.ring_ts = (uint64_t *)(block + offset);
        offset += ts_bytes;
        s_engine.ring_group = block + offset;
        offset += group_bytes;
        s_engine.ring_len = (uint16_t *)(block + offset);
        offset += len_bytes;
        s_engine.ring_payload = block + offset;
        s_engine.ring_block = block;
        s_engine.ring_frame_bytes = frame_bytes;
        s_engine.ring_capacity = capacity;
        ESP_LOGI(CURVE_TAG, "ring: %u frames x %u B CVF2 (%u bytes heap)",
                 (unsigned)capacity, (unsigned)frame_bytes, (unsigned)total_bytes);
    }

    curve_ring_reset(&s_engine.ring, s_engine.ring_capacity);
    s_engine.stat_invalid = 0U;
    s_engine.stat_out_of_scope = 0U;
    s_engine.stat_errors = 0U;
    s_engine.stat_consecutive = 0U;
    s_engine.stat_last_tick_us = 0U;
    s_engine.stat_last_error = 0U;
    s_engine.stat_hits = 0U;
    s_engine.stat_sampled = 0U;
    s_engine.stat_throttled = 0U;
    s_engine.current_group = 0U;
    return ESP_OK;
}

static void engine_record_frame(const uint8_t *payload, uint16_t payload_len,
                                uint8_t group, uint64_t timestamp_us,
                                uint32_t invalid, uint32_t out_of_scope,
                                bool tick_ok, uint32_t tick_us,
                                esp_err_t tick_err)
{
    uint32_t slot = 0U;
    uint32_t seq = 0U;

    engine_lock();
    if (s_engine.ring_block != NULL && s_engine.ring_payload != NULL &&
        payload_len <= s_engine.ring_frame_bytes) {
        /* curve_ring_begin_write() only counts a frame as overwritten when it
         * is evicted before a frames?since=... response delivered it. */
        curve_ring_begin_write(&s_engine.ring, &slot, &seq);
        if (seq != 0U) {
            s_engine.ring_seq[slot] = seq;
            s_engine.ring_ts[slot] = timestamp_us;
            s_engine.ring_group[slot] = group;
            s_engine.ring_len[slot] = payload_len;
            memcpy(s_engine.ring_payload +
                       (size_t)slot * s_engine.ring_frame_bytes,
                   payload, payload_len);
            curve_ring_commit_write(&s_engine.ring);
        }
        s_engine.stat_invalid += invalid;
        s_engine.stat_out_of_scope += out_of_scope;
        s_engine.stat_last_tick_us = tick_us;
        if (tick_ok) {
            s_engine.stat_consecutive = 0U;
        } else {
            s_engine.stat_errors++;
            s_engine.stat_consecutive++;
            s_engine.stat_last_error = (uint32_t)tick_err;
        }
    }
    engine_unlock();
}

/* ------------------------------------------------------------------------- */
/* shared sample collection (periodic / breakpoint trigger)                  */
/* ------------------------------------------------------------------------- */

/** Sleep up to `us`, waking early on a stop request (esp_timer one-shot plus a
 *  worker notification; the timeout is only a failsafe).  Returns false when a
 *  stop was requested. */
static bool curve_sleep_us(uint32_t us)
{
    TickType_t safety;

    if (us == 0U) {
        return !s_engine.stop_req;
    }
    safety = pdMS_TO_TICKS(us / 1000U) + pdMS_TO_TICKS(100U);
    if (esp_timer_start_once(s_engine.tick_timer, us) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS((us + 999U) / 1000U));
        return !s_engine.stop_req;
    }
    (void)ulTaskNotifyTake(pdTRUE, safety);
    (void)esp_timer_stop(s_engine.tick_timer);
    return !s_engine.stop_req;
}

/** DHCSR.S_HALT must clear a few cycles after a resume write. */
static esp_err_t curve_wait_core_running(void)
{
    for (int i = 0; i < 10; i++) {
        uint32_t dhcsr = 0U;
        esp_err_t err = tf_swd_read_u32(CURVE_REG_DHCSR, &dhcsr);

        if (err != ESP_OK) {
            return err;
        }
        if ((dhcsr & CURVE_DHCSR_S_HALT) == 0U) {
            return ESP_OK;
        }
        esp_rom_delay_us(200U);
    }
    return ESP_ERR_INVALID_STATE;
}

/** Make sure the core runs before installing the comparator (a previous probe
 *  or debug session may have left it halted). */
static esp_err_t curve_ensure_core_running(void)
{
    bool halted = false;
    esp_err_t err = tf_swd_is_halted(&halted);

    if (err != ESP_OK) {
        return err;
    }
    if (!halted) {
        return ESP_OK;
    }
    ESP_LOGW(CURVE_TAG, "breakpoint: target was halted, resuming it");
    err = curve_resume_core();
    if (err != ESP_OK) {
        return err;
    }
    return curve_wait_core_running();
}

/**
 * @brief Read one sample for one group: core registers (only when the group's
 *        expressions need them and the core is halted), select the expression
 *        (single / PC gate / loclist variant), evaluate it and read the target
 *        memory.
 *
 * `pc_state` mirrors the Phase 2.5 contract:
 *   0 = PC not available (no-halt periodic mode) -> series that need the PC are
 *       zeroed and counted out_of_scope;
 *   1 = PC valid -> loclist variant selection / single PC gate;
 *   2 = PC read failed -> series that need the PC are zeroed and counted
 *       invalid (a real read failure).
 *
 * `payload` must have room for cfg->sample_bytes (max group payload) and is
 * zero-filled first, so skipped/invalid series stay 0.  *payload_len_out is the
 * group's payload length (== group sample_bytes) and becomes the CVF2
 * payload_len.
 */
static void curve_collect_sample(const curve_config_t *cfg, uint8_t group_index,
                                 bool core_halted, uint8_t pc_state,
                                 uint32_t pc_value, uint8_t *payload,
                                 uint16_t *payload_len_out,
                                 uint32_t *invalid_out,
                                 uint32_t *out_of_scope_out,
                                 bool *ok_out, esp_err_t *err_out)
{
    const curve_group_t *group;
    uint32_t reg_values[CURVE_MAX_CORE_REGS];
    bool reg_valid[CURVE_MAX_CORE_REGS];
    uint32_t invalid = 0U;
    uint32_t out_of_scope = 0U;
    uint16_t offset = 0U;
    uint16_t first;
    uint16_t last;
    bool ok = true;
    esp_err_t tick_err = ESP_OK;

    if (cfg == NULL || group_index >= cfg->group_count) {
        *payload_len_out = 0U;
        *invalid_out = 0U;
        *out_of_scope_out = 0U;
        *ok_out = false;
        *err_out = ESP_ERR_INVALID_ARG;
        return;
    }
    group = &cfg->groups[group_index];
    memset(reg_valid, 0, sizeof(reg_valid));
    memset(payload, 0, group->sample_bytes);

    /* Reuse the PC read done for the scope gate/variants if an expression also
     * needs r15 (one DCRSR read per tick). */
    if (pc_state == 1U) {
        reg_values[15] = pc_value;
        reg_valid[15] = true;
    }

    if (core_halted && group->reg_count > 0U) {
        for (uint8_t i = 0U; i < group->reg_count; i++) {
            uint8_t reg = group->regs[i];
            uint32_t value = 0U;
            esp_err_t err;

            if (reg_valid[reg]) {
                continue;
            }
            err = tf_swd_read_reg(reg, &value);
            if (err == ESP_OK) {
                reg_values[reg] = value;
                reg_valid[reg] = true;
            } else {
                ok = false;
                tick_err = err;
            }
        }
    }

    first = group->series_first;
    last = (uint16_t)(first + group->series_count);
    for (uint16_t si = first; si < last; si++) {
        const curve_series_t *series = &cfg->series[si];
        uint32_t address = 0U;
        uint16_t expr_index = series->expr;
        curve_eval_result_t res;

        if (curve_series_needs_pc(series)) {
            if (pc_state == 1U) {
                expr_index = curve_series_expr_for_pc(cfg, series, pc_value);
                if (expr_index == CURVE_EXPR_NONE) {
                    out_of_scope++;
                    offset += series->size;
                    continue;
                }
            } else if (pc_state == 2U) {
                invalid++;
                offset += series->size;
                continue;
            } else {
                /* No PC while the target runs: a stack-frame variable cannot be
                 * safely attributed to this tick, so gate it out. */
                out_of_scope++;
                offset += series->size;
                continue;
            }
        }

        res = curve_expr_eval(cfg, expr_index, reg_values, reg_valid, &address);
        if (res == CURVE_EVAL_OK && curve_addr_range_ok(address, series->size)) {
            esp_err_t err = tf_swd_read_mem(address, payload + offset, series->size);

            if (err != ESP_OK) {
                invalid++;
                ok = false;
                tick_err = err;
                (void)tf_swd_recover_bus();
            }
        } else {
            /* Not readable / not evaluable: zero-fill this series and keep
             * sampling (contract: invalid++, never abort the session). */
            invalid++;
            if (res == CURVE_EVAL_BUS) {
                ok = false;
                tick_err = ESP_FAIL;
                (void)tf_swd_recover_bus();
            }
        }
        offset += series->size;
    }

    *payload_len_out = offset;
    *invalid_out = invalid;
    *out_of_scope_out = out_of_scope;
    *ok_out = ok;
    *err_out = tick_err;
}

/* ------------------------------------------------------------------------- */
/* periodic trigger: halt -> sample current group -> resume every interval   */
/* ------------------------------------------------------------------------- */

static void curve_periodic_loop(void)
{
    uint8_t payload[CURVE_MAX_SAMPLE_BYTES];
    const curve_config_t *cfg = s_engine.cfg;
    bool halted = false;
    uint32_t consecutive_errors = 0U;
    TickType_t tick_wait;

    if (cfg == NULL) {
        engine_set_message("stopped: no configuration");
        return;
    }
    tick_wait = pdMS_TO_TICKS(cfg->interval_us / 1000U) * 2U +
                pdMS_TO_TICKS(1000U);

    if (esp_timer_start_periodic(s_engine.tick_timer, cfg->interval_us) != ESP_OK) {
        engine_set_message("stopped: tick timer start failed");
        return;
    }

    while (!s_engine.stop_req) {
        /* Tick source: one esp_timer notification per sampling interval.  The
         * timeout is a failsafe only (never expected to fire). */
        (void)ulTaskNotifyTake(pdTRUE, tick_wait);
        if (s_engine.stop_req) {
            break;
        }
        if (engine_target_flash_busy()) {
            engine_set_message("stopped: target flash session is active");
            break;
        }

        int64_t t_start = esp_timer_get_time();
        bool tick_ok = true;
        esp_err_t tick_err = ESP_OK;
        uint32_t invalid = 0U;
        uint32_t out_of_scope = 0U;
        uint16_t payload_len = 0U;
        uint8_t pc_state = 0U;
        uint32_t pc_value = 0U;
        uint8_t group_index = s_engine.current_group;
        const curve_group_t *group;

        if (group_index >= cfg->group_count) {
            group_index = 0U;
        }
        group = &cfg->groups[group_index];

        if (cfg->halt) {
            esp_err_t err = tf_swd_halt();
            if (err != ESP_OK) {
                tick_ok = false;
                tick_err = err;
            } else {
                halted = true;
            }
        }

        /* One PC read per tick when the current group needs it.  DCRSR is only
         * readable while the core is halted (no-halt mode keeps pc_state=0). */
        if (group->pc_needed && halted) {
            esp_err_t err = tf_swd_read_reg(15U, &pc_value);
            if (err == ESP_OK) {
                pc_state = 1U;
            } else {
                pc_state = 2U;
                tick_ok = false;
                if (tick_err == ESP_OK) {
                    tick_err = err;
                }
            }
        }

        {
            bool collect_ok = true;
            esp_err_t collect_err = ESP_OK;

            curve_collect_sample(cfg, group_index, halted, pc_state, pc_value,
                                 payload, &payload_len, &invalid, &out_of_scope,
                                 &collect_ok, &collect_err);
            if (!collect_ok) {
                tick_ok = false;
                if (tick_err == ESP_OK) {
                    tick_err = collect_err;
                }
            }
        }

        if (halted) {
            esp_err_t err = curve_resume_core();
            if (err != ESP_OK) {
                tick_ok = false;
                tick_err = err;
            } else {
                halted = false;
            }
        }

        {
            int64_t t_end = esp_timer_get_time();

            engine_record_frame(payload, payload_len, group_index,
                                (uint64_t)t_start, invalid, out_of_scope,
                                tick_ok, (uint32_t)(t_end - t_start), tick_err);
        }
        engine_lock();
        s_engine.current_group = curve_next_group(group_index, cfg->group_count);
        engine_unlock();

        if (tick_ok) {
            consecutive_errors = 0U;
        } else {
            consecutive_errors++;
            (void)tf_swd_recover_bus();
            ESP_LOGW(CURVE_TAG, "sample tick failed (%s), consecutive=%u",
                     esp_err_to_name(tick_err), (unsigned)consecutive_errors);
            if (consecutive_errors >= CURVE_MAX_CONSECUTIVE_ERRORS) {
                engine_set_message("stopped: %u consecutive SWD errors",
                                   (unsigned)consecutive_errors);
                break;
            }
        }
    }

    (void)esp_timer_stop(s_engine.tick_timer);
    if (halted) {
        /* Never leave the target halted when stopping (normal stop or error). */
        curve_release_core();
        halted = false;
    }
    if (s_engine.stop_req) {
        engine_set_message("stopped");
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 2: FPB breakpoint trigger                                           */
/* ------------------------------------------------------------------------- */

static esp_err_t curve_fpb_enable(uint32_t compare_value)
{
    /* FPB writes require the KEY bit in FP_CTRL; comparator bit0 = enable. */
    esp_err_t err = tf_swd_write_u32(CURVE_FPB_CTRL,
                                     CURVE_FPB_CTRL_KEY | CURVE_FPB_CTRL_ENABLE);

    if (err != ESP_OK) {
        return err;
    }
    return tf_swd_write_u32(CURVE_FPB_COMP0, compare_value);
}

static esp_err_t curve_fpb_disable(void)
{
    esp_err_t err = tf_swd_write_u32(CURVE_FPB_CTRL,
                                     CURVE_FPB_CTRL_KEY | CURVE_FPB_CTRL_ENABLE);

    if (err != ESP_OK) {
        return err;
    }
    err = tf_swd_write_u32(CURVE_FPB_COMP0, 0U);
    if (err != ESP_OK) {
        return err;
    }
    /* Gate the whole FPB unit off while throttling: the core runs unrestricted
     * and cannot retrigger on the same comparator. */
    return tf_swd_write_u32(CURVE_FPB_CTRL, CURVE_FPB_CTRL_KEY);
}

/** Save the current FPB_CTRL/FP_COMP0 and arm comparator 0. */
static esp_err_t curve_fpb_begin(uint32_t compare_value)
{
    esp_err_t err;
    uint32_t ctrl = 0U;
    uint32_t comp0 = 0U;

    err = tf_swd_read_u32(CURVE_FPB_CTRL, &ctrl);
    if (err != ESP_OK) {
        return err;
    }
    if (ctrl == 0xFFFFFFFFU) {
        ESP_LOGE(CURVE_TAG, "breakpoint: FPB not available (FP_CTRL=0x%08X)",
                 (unsigned)ctrl);
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = tf_swd_read_u32(CURVE_FPB_COMP0, &comp0);
    if (err != ESP_OK) {
        return err;
    }
    s_engine.fpb_orig_ctrl = ctrl;
    s_engine.fpb_orig_comp0 = comp0;
    s_engine.fpb_compare = compare_value;
    /* Mark active before arming: a partially failed arm must still be restored
     * on the session error path. */
    s_engine.fpb_active = true;

    err = curve_fpb_enable(compare_value);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(CURVE_TAG, "FPB armed: FP_CTRL=0x%08X FP_COMP0=0x%08X -> 0x%08X",
             (unsigned)ctrl, (unsigned)comp0, (unsigned)compare_value);
    return ESP_OK;
}

/** Restore the FPB_CTRL/FP_COMP0 values captured at session start. */
static void curve_fpb_restore(void)
{
    if (!s_engine.fpb_active) {
        return;
    }
    (void)tf_swd_write_u32(CURVE_FPB_CTRL,
                           CURVE_FPB_CTRL_KEY | CURVE_FPB_CTRL_ENABLE);
    (void)tf_swd_write_u32(CURVE_FPB_COMP0, s_engine.fpb_orig_comp0);
    (void)tf_swd_write_u32(CURVE_FPB_CTRL,
                           CURVE_FPB_CTRL_KEY |
                               (s_engine.fpb_orig_ctrl & CURVE_FPB_CTRL_ENABLE));
    s_engine.fpb_active = false;
    ESP_LOGI(CURVE_TAG, "FPB restored (FP_CTRL=0x%08X FP_COMP0=0x%08X)",
             (unsigned)s_engine.fpb_orig_ctrl, (unsigned)s_engine.fpb_orig_comp0);
}

/**
 * Breakpoint trigger loop: FPB comparator stops the core itself, the sampler
 * reads the halted frame, then disables the comparator, resumes the core,
 * waits the throttle interval and re-arms.  All FPB state is restored on exit.
 */
static void curve_breakpoint_loop(void)
{
    uint8_t payload[CURVE_MAX_SAMPLE_BYTES];
    const curve_config_t *cfg = s_engine.cfg;
    uint32_t compare_value;
    uint32_t consecutive_errors = 0U;
    bool core_halted = false;
    bool fpb_active = false;
    bool armed = false;
    esp_err_t err;

    if (cfg == NULL) {
        engine_set_message("breakpoint: no configuration");
        return;
    }
    compare_value = curve_breakpoint_compare_value(cfg->breakpoint_address);
    (void)esp_timer_stop(s_engine.tick_timer);

    err = curve_ensure_core_running();
    if (err != ESP_OK) {
        engine_set_message("breakpoint: target does not run: %s",
                           esp_err_to_name(err));
        goto out;
    }
    err = curve_fpb_begin(compare_value);
    if (err != ESP_OK) {
        engine_set_message("breakpoint: FPB setup failed: %s",
                           esp_err_to_name(err));
        goto out;
    }
    fpb_active = true;
    armed = true;

    while (!s_engine.stop_req) {
        uint32_t dhcsr = 0U;

        if (engine_target_flash_busy()) {
            engine_set_message("stopped: target flash session is active");
            break;
        }
        if (!armed) {
            /* Re-arm after the throttle (or retry a failed arm). */
            err = curve_fpb_enable(compare_value);
            if (err != ESP_OK) {
                consecutive_errors++;
                (void)tf_swd_recover_bus();
                ESP_LOGW(CURVE_TAG, "breakpoint re-arm failed (%s), consecutive=%u",
                         esp_err_to_name(err), (unsigned)consecutive_errors);
                if (consecutive_errors >= CURVE_MAX_CONSECUTIVE_ERRORS) {
                    engine_set_message("stopped: %u consecutive SWD errors",
                                       (unsigned)consecutive_errors);
                    break;
                }
                if (!curve_sleep_us(CURVE_BP_POLL_INTERVAL_US)) {
                    break;
                }
                continue;
            }
            armed = true;
        }
        err = tf_swd_read_u32(CURVE_REG_DHCSR, &dhcsr);
        if (err != ESP_OK) {
            consecutive_errors++;
            (void)tf_swd_recover_bus();
            ESP_LOGW(CURVE_TAG, "breakpoint poll failed (%s), consecutive=%u",
                     esp_err_to_name(err), (unsigned)consecutive_errors);
            if (consecutive_errors >= CURVE_MAX_CONSECUTIVE_ERRORS) {
                engine_set_message("stopped: %u consecutive SWD errors",
                                   (unsigned)consecutive_errors);
                break;
            }
            if (!curve_sleep_us(CURVE_BP_POLL_INTERVAL_US)) {
                break;
            }
            continue;
        }
        if ((dhcsr & CURVE_DHCSR_S_HALT) == 0U) {
            /* No hit yet: poll DHCSR every ~1 ms without blocking httpd. */
            if (!curve_sleep_us(CURVE_BP_POLL_INTERVAL_US)) {
                break;
            }
            continue;
        }

        /* Breakpoint hit: the core is halted.  Timestamp close to the hit. */
        core_halted = true;
        {
            int64_t t_hit = esp_timer_get_time();
            bool collect_ok = true;
            esp_err_t collect_err = ESP_OK;
            uint32_t invalid = 0U;
            uint32_t out_of_scope = 0U;
            uint16_t payload_len = 0U;
            uint32_t sample_us;
            uint8_t pc_state = 0U;
            uint32_t pc_value = 0U;
            uint8_t group_index = s_engine.current_group;
            const curve_group_t *group;

            if (group_index >= cfg->group_count) {
                group_index = 0U;
            }
            group = &cfg->groups[group_index];

            engine_lock();
            s_engine.stat_hits++;
            engine_unlock();

            /* One PC read per hit when the current group needs it (the core is
             * halted by the FPB hit, so DCRSR works like periodic halt). */
            if (group->pc_needed) {
                esp_err_t perr = tf_swd_read_reg(15U, &pc_value);

                if (perr == ESP_OK) {
                    pc_state = 1U;
                } else {
                    pc_state = 2U;
                    collect_ok = false;
                    collect_err = perr;
                }
            }

            curve_collect_sample(cfg, group_index, true, pc_state, pc_value,
                                 payload, &payload_len, &invalid, &out_of_scope,
                                 &collect_ok, &collect_err);
            sample_us = (uint32_t)(esp_timer_get_time() - t_hit);
            engine_record_frame(payload, payload_len, group_index,
                                (uint64_t)t_hit, invalid, out_of_scope,
                                collect_ok, sample_us, collect_err);
            engine_lock();
            s_engine.stat_sampled++;
            s_engine.current_group = curve_next_group(group_index, cfg->group_count);
            engine_unlock();
            if (!collect_ok) {
                ESP_LOGW(CURVE_TAG, "breakpoint sample failed (%s)",
                         esp_err_to_name(collect_err));
            }
        }

        /* Disable the comparator before resuming, then throttle.  The loop
         * top re-arms it after the throttle wait. */
        armed = false;
        err = curve_fpb_disable();
        if (err != ESP_OK) {
            consecutive_errors++;
            (void)tf_swd_recover_bus();
            ESP_LOGW(CURVE_TAG, "breakpoint disarm failed (%s), consecutive=%u",
                     esp_err_to_name(err), (unsigned)consecutive_errors);
        }
        err = curve_resume_core();
        if (err == ESP_OK) {
            core_halted = false;
            err = curve_wait_core_running();
        }
        if (err != ESP_OK) {
            consecutive_errors++;
            (void)tf_swd_recover_bus();
            ESP_LOGW(CURVE_TAG, "breakpoint resume failed (%s), consecutive=%u",
                     esp_err_to_name(err), (unsigned)consecutive_errors);
        } else {
            consecutive_errors = 0U;
        }
        if (consecutive_errors >= CURVE_MAX_CONSECUTIVE_ERRORS) {
            engine_set_message("stopped: %u consecutive SWD errors",
                               (unsigned)consecutive_errors);
            break;
        }

        /* Throttle: minimum time between two sampled hits.  The comparator is
         * re-armed at the top of the next loop iteration. */
        if (!curve_sleep_us(cfg->interval_us)) {
            break;
        }
    }

out:
    /* stop/error path: restore FPB first, then make sure the target runs. */
    if (fpb_active) {
        curve_fpb_restore();
        fpb_active = false;
    }
    if (core_halted) {
        curve_release_core();
        core_halted = false;
    }
    (void)esp_timer_stop(s_engine.tick_timer);
    if (s_engine.stop_req) {
        engine_set_message("stopped");
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 0 selftest (worker task)                                            */
/* ------------------------------------------------------------------------- */

static void sort_u32(uint32_t *values, uint32_t count)
{
    for (uint32_t i = 1U; i < count; i++) {
        uint32_t key = values[i];
        uint32_t j = i;

        while (j > 0U && values[j - 1U] > key) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = key;
    }
}

static uint32_t percentile_u32(const uint32_t *sorted, uint32_t count, uint32_t pct)
{
    uint32_t index;

    if (count == 0U) {
        return 0U;
    }
    index = (count * pct) / 100U;
    if (index >= count) {
        index = count - 1U;
    }
    return sorted[index];
}

static void curve_run_selftest(curve_selftest_t *st)
{
    uint32_t samples[CURVE_SELFTEST_ITERATIONS];
    uint64_t stage_sums[4] = {0U, 0U, 0U, 0U}; /* halt, regs, mem, resume */
    uint32_t completed = 0U;
    uint32_t failed = 0U;
    uint32_t consecutive_fail = 0U;
    bool halted = false;
    esp_err_t err;

    memset(st, 0, sizeof(*st));
    st->iterations = CURVE_SELFTEST_ITERATIONS;

    err = curve_bringup();
    if (err != ESP_OK) {
        curve_dap_release();
        st->failed = CURVE_SELFTEST_ITERATIONS;
        engine_lock();
        s_engine.selftest = *st;
        s_engine.cmd_result = err;
        (void)snprintf(s_engine.message, sizeof(s_engine.message),
                       "selftest: SWD bring-up failed: %s", esp_err_to_name(err));
        engine_unlock();
        ESP_LOGE(CURVE_TAG, "selftest bring-up failed: %s", esp_err_to_name(err));
        return;
    }
    st->link_hz = s_engine.link_hz;
    st->dpidr = s_engine.dpidr;

    for (uint32_t i = 0U; i < CURVE_SELFTEST_ITERATIONS; i++) {
        bool ok = true;
        uint32_t r13 = 0U;
        uint32_t r7 = 0U;
        uint32_t r15 = 0U;
        uint32_t mem_addr;
        uint32_t word0 = 0U;
        uint32_t word1 = 0U;
        int64_t t0 = esp_timer_get_time();
        int64_t t1;
        int64_t t2;
        int64_t t3;
        int64_t t4;

        if (tf_swd_halt() != ESP_OK) {
            ok = false;
        } else {
            halted = true;
        }
        t1 = esp_timer_get_time();

        if (ok && (tf_swd_read_reg(13U, &r13) != ESP_OK ||
                   tf_swd_read_reg(7U, &r7) != ESP_OK ||
                   tf_swd_read_reg(15U, &r15) != ESP_OK)) {
            ok = false;
        }
        t2 = esp_timer_get_time();

        if (ok) {
            /* Read two real target words from the halted stack (a realistic
             * sampling read); fall back to the DTCM/SRAM base if SP is not in
             * an allowed window (e.g. target reset and not yet running). */
            mem_addr = curve_addr_range_ok(r13, 8U) ? r13 : 0x20000000U;
            if (tf_swd_read_u32(mem_addr, &word0) != ESP_OK ||
                tf_swd_read_u32(mem_addr + 4U, &word1) != ESP_OK) {
                ok = false;
            }
        }
        t3 = esp_timer_get_time();

        if (halted) {
            if (curve_resume_core() != ESP_OK) {
                ok = false;
            } else {
                halted = false;
            }
        }
        t4 = esp_timer_get_time();

        if (ok) {
            samples[completed] = (uint32_t)(t4 - t0);
            stage_sums[0] += (uint64_t)(t1 - t0);
            stage_sums[1] += (uint64_t)(t2 - t1);
            stage_sums[2] += (uint64_t)(t3 - t2);
            stage_sums[3] += (uint64_t)(t4 - t3);
            completed++;
            consecutive_fail = 0U;
        } else {
            failed++;
            consecutive_fail++;
            (void)tf_swd_recover_bus();
            if (consecutive_fail >= CURVE_MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGW(CURVE_TAG, "selftest: %u consecutive failures, aborting",
                         (unsigned)consecutive_fail);
                break;
            }
        }
        /* Let the target execute between iterations so the next halt is a real
         * running->halt transition (the interval is not part of the measured
         * per-iteration cost). */
        if (i + 1U < CURVE_SELFTEST_ITERATIONS) {
            esp_rom_delay_us(2000U);
        }
    }

    if (halted) {
        curve_release_core();
        halted = false;
    }
    curve_dap_release();

    st->completed = completed;
    st->failed = failed;
    if (completed > 0U) {
        sort_u32(samples, completed);
        st->min_us = samples[0];
        st->max_us = samples[completed - 1U];
        st->p90_us = percentile_u32(samples, completed, 90U);
        if ((completed % 2U) == 0U) {
            st->median_us = (samples[completed / 2U - 1U] + samples[completed / 2U]) / 2U;
        } else {
            st->median_us = samples[completed / 2U];
        }
        st->halt_us = (uint32_t)(stage_sums[0] / completed);
        st->regs_us = (uint32_t)(stage_sums[1] / completed);
        st->mem_us = (uint32_t)(stage_sums[2] / completed);
        st->resume_us = (uint32_t)(stage_sums[3] / completed);
        err = ESP_OK;
        ESP_LOGI(CURVE_TAG,
                 "selftest: n=%u min=%u med=%u p90=%u max=%u us (halt=%u regs=%u "
                 "mem=%u resume=%u us) link=%u Hz",
                 (unsigned)completed, (unsigned)st->min_us, (unsigned)st->median_us,
                 (unsigned)st->p90_us, (unsigned)st->max_us, (unsigned)st->halt_us,
                 (unsigned)st->regs_us, (unsigned)st->mem_us, (unsigned)st->resume_us,
                 (unsigned)st->link_hz);
    } else {
        err = ESP_ERR_NOT_FOUND;
        ESP_LOGE(CURVE_TAG, "selftest: no successful iteration");
    }

    engine_lock();
    s_engine.selftest = *st;
    s_engine.cmd_result = err;
    if (completed > 0U) {
        (void)snprintf(s_engine.message, sizeof(s_engine.message),
                       "selftest ok: median %u us", (unsigned)st->median_us);
    } else {
        (void)snprintf(s_engine.message, sizeof(s_engine.message),
                       "selftest failed: no SWD iteration succeeded");
    }
    engine_unlock();
}

/* ------------------------------------------------------------------------- */
/* worker task / commands                                                    */
/* ------------------------------------------------------------------------- */

static void curve_tick_cb(void *arg)
{
    (void)arg;
    if (s_engine.task != NULL) {
        (void)xTaskNotifyGive(s_engine.task);
    }
}

static void curve_worker_start_cmd(void)
{
    esp_err_t err = curve_bringup();
    bool aborted;

    if (err != ESP_OK) {
        curve_dap_release();
        engine_lock();
        s_engine.cmd_result = err;
        (void)snprintf(s_engine.message, sizeof(s_engine.message),
                       "start failed: SWD bring-up failed: %s", esp_err_to_name(err));
        engine_unlock();
        ESP_LOGE(CURVE_TAG, "start failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_engine.done_sem);
        return;
    }

    engine_lock();
    aborted = s_engine.stop_req;
    if (!aborted) {
        const curve_config_t *cfg = s_engine.cfg;

        s_engine.running = true;
        s_engine.cmd_result = ESP_OK;
        if (cfg != NULL &&
            cfg->trigger == (uint8_t)CURVE_TRIGGER_BREAKPOINT) {
            (void)snprintf(s_engine.message, sizeof(s_engine.message),
                           "breakpoint sampling @0x%08X (%u group(s), throttle %u us)",
                           (unsigned)cfg->breakpoint_address,
                           (unsigned)cfg->group_count,
                           (unsigned)cfg->interval_us);
        } else if (cfg != NULL) {
            (void)snprintf(s_engine.message, sizeof(s_engine.message),
                           "sampling %u group(s) @%u us",
                           (unsigned)cfg->group_count,
                           (unsigned)cfg->interval_us);
        }
    }
    engine_unlock();

    xSemaphoreGive(s_engine.done_sem);

    if (aborted) {
        curve_dap_release();
        return;
    }

    if (s_engine.cfg != NULL &&
        s_engine.cfg->trigger == (uint8_t)CURVE_TRIGGER_BREAKPOINT) {
        curve_breakpoint_loop();
    } else {
        curve_periodic_loop();
    }

    curve_dap_release();
    engine_lock();
    s_engine.running = false;
    engine_unlock();
}

static void curve_worker_selftest_cmd(void)
{
    curve_run_selftest(&s_engine.selftest);
    xSemaphoreGive(s_engine.done_sem);
}

static void curve_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        curve_cmd_t cmd;

        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        engine_lock();
        cmd = s_engine.cmd;
        s_engine.cmd = CURVE_CMD_NONE;
        engine_unlock();

        /* Stale esp_timer notification (e.g. timer stopped right after firing):
         * nothing to do, do not touch the worker state. */
        if (cmd == CURVE_CMD_NONE) {
            continue;
        }

        if (cmd == CURVE_CMD_START) {
            curve_worker_start_cmd();
        } else if (cmd == CURVE_CMD_SELFTEST) {
            curve_worker_selftest_cmd();
        }

        engine_lock();
        s_engine.worker_state = CURVE_WORKER_IDLE;
        engine_unlock();
        (void)xSemaphoreGive(s_engine.idle_sem);
    }
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t curve_engine_init(void)
{
    static const esp_timer_create_args_t timer_args = {
        .callback = curve_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "curve_tick",
        .skip_unhandled_events = true,
    };
    BaseType_t created;

    if (s_inited) {
        return ESP_OK;
    }
    s_engine.lock = xSemaphoreCreateMutex();
    s_engine.done_sem = xSemaphoreCreateBinary();
    s_engine.idle_sem = xSemaphoreCreateBinary();
    if (s_engine.lock == NULL || s_engine.done_sem == NULL ||
        s_engine.idle_sem == NULL) {
        ESP_LOGE(CURVE_TAG, "engine init: semaphore creation failed");
        return ESP_FAIL;
    }
    if (esp_timer_create(&timer_args, &s_engine.tick_timer) != ESP_OK) {
        ESP_LOGE(CURVE_TAG, "engine init: esp_timer_create failed");
        return ESP_FAIL;
    }
    created = xTaskCreatePinnedToCore(curve_worker_task, "curve_sampler",
                                      CURVE_TASK_STACK, NULL, CURVE_TASK_PRIO,
                                      &s_engine.task, CURVE_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(CURVE_TAG, "engine init: worker task creation failed");
        s_engine.task = NULL;
        return ESP_FAIL;
    }
    s_inited = true;
    ESP_LOGI(CURVE_TAG, "sampler engine initialized (worker core %d, %u B stack)",
             CURVE_TASK_CORE, (unsigned)CURVE_TASK_STACK);
    return ESP_OK;
}

bool curve_engine_is_idle(void)
{
    bool idle;

    if (curve_engine_init() != ESP_OK) {
        return true; /* init failed: start will surface the error */
    }
    engine_lock();
    idle = !s_engine.running && s_engine.worker_state == CURVE_WORKER_IDLE;
    engine_unlock();
    return idle;
}

esp_err_t curve_engine_start(const curve_config_t *cfg, char *err, size_t err_len)
{
    esp_err_t rc;

    if (err != NULL && err_len > 0U) {
        err[0] = '\0';
    }
    rc = curve_engine_init();
    if (rc != ESP_OK) {
        if (err != NULL) {
            (void)snprintf(err, err_len, "sampler init failed");
        }
        return rc;
    }
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (engine_target_flash_busy()) {
        if (err != NULL) {
            (void)snprintf(err, err_len, "busy (target flash session active)");
        }
        return ESP_ERR_INVALID_STATE;
    }

    engine_lock();
    if (s_engine.running || s_engine.worker_state != CURVE_WORKER_IDLE) {
        engine_unlock();
        if (err != NULL) {
            (void)snprintf(err, err_len, "busy");
        }
        return ESP_ERR_INVALID_STATE;
    }
    rc = engine_ring_prepare(cfg->sample_bytes);
    if (rc != ESP_OK) {
        engine_unlock();
        if (err != NULL) {
            (void)snprintf(err, err_len, "no memory for sample ring");
        }
        return rc;
    }
    s_engine.cfg = cfg; /* HTTP-owned static config; valid while running */
    s_engine.current_group = 0U;
    s_engine.message[0] = '\0';
    s_engine.cmd_result = ESP_FAIL;
    s_engine.stop_req = false;
    s_engine.worker_state = CURVE_WORKER_BUSY;
    s_engine.cmd = CURVE_CMD_START;
    /* Reset the session counters before the ring is visible to the frames API
     * (ring slots are only written by the worker after the bring-up). */
    engine_unlock();

    /* Drop tokens left by a previous completed/cancelled command. */
    (void)xSemaphoreTake(s_engine.idle_sem, 0);
    (void)xSemaphoreTake(s_engine.done_sem, 0);
    (void)xTaskNotifyGive(s_engine.task);

    if (xSemaphoreTake(s_engine.done_sem,
                       pdMS_TO_TICKS(CURVE_BRINGUP_TIMEOUT_MS)) != pdTRUE) {
        engine_lock();
        s_engine.stop_req = true;
        engine_unlock();
        (void)xTaskNotifyGive(s_engine.task);
        if (err != NULL) {
            (void)snprintf(err, err_len, "SWD bring-up timeout");
        }
        return ESP_ERR_TIMEOUT;
    }

    engine_lock();
    rc = s_engine.cmd_result;
    bool running = s_engine.running;
    if (err != NULL && err_len > 0U) {
        (void)snprintf(err, err_len, "%s", s_engine.message);
    }
    engine_unlock();

    if (rc == ESP_OK && !running) {
        rc = ESP_FAIL;
    }
    return rc;
}

void curve_engine_release_ring(void)
{
    if (curve_engine_init() != ESP_OK) {
        return;
    }
    engine_lock();
    free(s_engine.ring_block);
    s_engine.ring_block = NULL;
    s_engine.ring_seq = NULL;
    s_engine.ring_ts = NULL;
    s_engine.ring_payload = NULL;
    s_engine.ring_group = NULL;
    s_engine.ring_len = NULL;
    s_engine.ring_frame_bytes = 0U;
    s_engine.ring_capacity = 0U;
    memset(&s_engine.ring, 0, sizeof(s_engine.ring));
    engine_unlock();
}

esp_err_t curve_engine_stop(uint32_t timeout_ms, char *err, size_t err_len)
{
    bool running;
    bool busy;

    if (err != NULL && err_len > 0U) {
        err[0] = '\0';
    }
    if (curve_engine_init() != ESP_OK) {
        if (err != NULL) {
            (void)snprintf(err, err_len, "sampler init failed");
        }
        return ESP_FAIL;
    }
    if (timeout_ms == 0U) {
        timeout_ms = CURVE_STOP_TIMEOUT_MS;
    }

    /* Drop a token left by an earlier self-stop before re-checking the state,
     * so a stale token cannot make us wait for a second (never coming) one. */
    (void)xSemaphoreTake(s_engine.idle_sem, 0);
    engine_lock();
    running = s_engine.running;
    busy = (s_engine.worker_state != CURVE_WORKER_IDLE);
    if (running || busy) {
        s_engine.stop_req = true;
    }
    engine_unlock();

    if (!running && !busy) {
        return ESP_OK;
    }

    (void)xTaskNotifyGive(s_engine.task); /* wake the tick wait immediately */
    if (xSemaphoreTake(s_engine.idle_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        if (err != NULL) {
            (void)snprintf(err, err_len, "stop timeout (worker busy)");
        }
        ESP_LOGE(CURVE_TAG, "stop timed out after %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    engine_lock();
    running = s_engine.running;
    engine_unlock();
    return running ? ESP_FAIL : ESP_OK;
}

esp_err_t curve_engine_selftest(curve_selftest_t *out, char *err, size_t err_len)
{
    esp_err_t rc;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (err != NULL && err_len > 0U) {
        err[0] = '\0';
    }
    rc = curve_engine_init();
    if (rc != ESP_OK) {
        return rc;
    }
    if (engine_target_flash_busy()) {
        if (err != NULL) {
            (void)snprintf(err, err_len, "busy (target flash session active)");
        }
        return ESP_ERR_INVALID_STATE;
    }

    engine_lock();
    if (s_engine.running || s_engine.worker_state != CURVE_WORKER_IDLE) {
        engine_unlock();
        if (err != NULL) {
            (void)snprintf(err, err_len, "busy");
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_engine.cmd_result = ESP_FAIL;
    s_engine.message[0] = '\0';
    s_engine.worker_state = CURVE_WORKER_BUSY;
    s_engine.cmd = CURVE_CMD_SELFTEST;
    engine_unlock();

    (void)xSemaphoreTake(s_engine.idle_sem, 0);
    (void)xSemaphoreTake(s_engine.done_sem, 0);
    (void)xTaskNotifyGive(s_engine.task);

    if (xSemaphoreTake(s_engine.done_sem,
                       pdMS_TO_TICKS(CURVE_BRINGUP_TIMEOUT_MS + 5000U)) != pdTRUE) {
        if (err != NULL) {
            (void)snprintf(err, err_len, "selftest timeout");
        }
        return ESP_ERR_TIMEOUT;
    }

    engine_lock();
    rc = s_engine.cmd_result;
    *out = s_engine.selftest;
    if (err != NULL && err_len > 0U) {
        (void)snprintf(err, err_len, "%s", s_engine.message);
    }
    engine_unlock();
    return rc;
}

void curve_engine_status(curve_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_inited) {
        (void)snprintf(out->message, sizeof(out->message), "not initialized");
        return;
    }
    engine_lock();
    out->running = s_engine.running;
    if (s_engine.cfg != NULL) {
        out->halt = s_engine.cfg->halt;
        out->interval_us = s_engine.cfg->interval_us;
        out->sample_bytes = s_engine.cfg->sample_bytes;
        out->series_count = s_engine.cfg->series_count;
        out->group_count = s_engine.cfg->group_count;
        out->trigger = s_engine.cfg->trigger;
        out->breakpoint_address = s_engine.cfg->breakpoint_address;
    }
    out->current_group = s_engine.current_group;
    out->frames = s_engine.ring.frames_total;
    out->overwritten = s_engine.ring.overwritten;
    out->ring_capacity = s_engine.ring.capacity;
    out->served_seq = s_engine.ring.served_seq;
    out->invalid = s_engine.stat_invalid;
    out->out_of_scope = s_engine.stat_out_of_scope;
    out->errors = s_engine.stat_errors;
    out->consecutive_errors = s_engine.stat_consecutive;
    out->link_hz = s_engine.link_hz;
    out->dpidr = s_engine.dpidr;
    out->last_tick_us = s_engine.stat_last_tick_us;
    out->last_error = s_engine.stat_last_error;
    out->hits = s_engine.stat_hits;
    out->sampled = s_engine.stat_sampled;
    out->throttled = s_engine.stat_throttled;
    if (s_engine.stat_last_error != 0U) {
        (void)snprintf(out->last_err, sizeof(out->last_err), "%s",
                       esp_err_to_name((esp_err_t)s_engine.stat_last_error));
    }
    (void)snprintf(out->message, sizeof(out->message), "%s", s_engine.message);
    engine_unlock();
}

esp_err_t curve_engine_alloc_frames(uint32_t since, uint32_t max,
                                    uint8_t **out_buf, size_t *out_len,
                                    bool *no_data)
{
    uint8_t *buf;
    uint32_t first_seq = 0U;
    uint32_t count = 0U;
    size_t frame_bytes;
    size_t len;
    size_t offset;
    bool gap = false;

    if (out_buf == NULL || out_len == NULL || no_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0U;
    *no_data = true;

    if (curve_engine_init() != ESP_OK) {
        return ESP_FAIL;
    }
    if (max == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max > CURVE_MAX_FRAMES_PER_REQ) {
        max = CURVE_MAX_FRAMES_PER_REQ;
    }

    engine_lock();
    if (s_engine.ring_block == NULL || s_engine.ring_payload == NULL) {
        engine_unlock();
        return ESP_OK;
    }
    count = curve_ring_plan_read(&s_engine.ring, since, max, &first_seq, &gap);
    if (count == 0U) {
        engine_unlock();
        return ESP_OK; /* 204: no frame newer than `since`, served cursor unchanged */
    }
    frame_bytes = s_engine.ring_frame_bytes;

    /* CVF2 frames are variable length (payload_len per group): compute the exact
     * response size first, then pack header + frames in one buffer. */
    len = CURVE_FRAME_HEADER_BYTES;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t seq = first_seq + i;
        uint32_t slot = (seq - 1U) % s_engine.ring.capacity;

        len += CURVE_FRAME_META_BYTES + s_engine.ring_len[slot];
    }
    buf = (uint8_t *)malloc(len);
    if (buf == NULL) {
        engine_unlock();
        return ESP_ERR_NO_MEM;
    }

    (void)curve_cvf2_write_header(buf, (uint16_t)count, first_seq,
                                  (uint16_t)frame_bytes, gap);
    offset = CURVE_FRAME_HEADER_BYTES;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t seq = first_seq + i;
        uint32_t slot = (seq - 1U) % s_engine.ring.capacity;
        uint16_t payload_len = s_engine.ring_len[slot];

        offset += curve_cvf2_write_frame_meta(buf + offset,
                                              s_engine.ring_seq[slot],
                                              s_engine.ring_ts[slot],
                                              s_engine.ring_group[slot],
                                              payload_len);
        memcpy(buf + offset,
               s_engine.ring_payload + (size_t)slot * frame_bytes,
               payload_len);
        offset += payload_len;
    }
    /* The batch was handed to the client: everything up to the last seq of
     * this response counts as served, so only frames evicted before a client
     * received them are counted in `overwritten` from now on. */
    curve_ring_mark_served(&s_engine.ring, first_seq, count);
    engine_unlock();

    *out_buf = buf;
    *out_len = offset;
    *no_data = false;
    return ESP_OK;
}
