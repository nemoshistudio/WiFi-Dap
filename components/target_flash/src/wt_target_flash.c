/*
 * SPDX-License-Identifier: MIT
 *
 * WiFi Dap - target flash engine (Web-side direct flashing).
 *
 * Public API: components/target_flash/include/wt_target_flash.h
 *
 * One flash session is a strict sequence:
 *   wt_tf_flash_begin(size)     -- probe target, halt core, erase covered
 *                                  sectors; takes exclusive ownership of the
 *                                  DAP engine and keeps it.
 *   wt_tf_flash_feed(data, n)*  -- stream image bytes; aligned blocks are
 *                                  programmed as soon as they are complete.
 *                                  Incremental SHA-256 of the source is kept.
 *   wt_tf_flash_end()           -- flush tail (0xFF padded), lock flash, read
 *                                  the whole image back, compare SHA-256, reset
 *                                  and release the DAP engine.
 *   wt_tf_abort()               -- flag an abort from another task; the running
 *                                  call stops at the next block boundary.
 *
 * Every exit path (success/error/abort) unlocks the flash (best effort),
 * releases the target (reset+run), restores the DAP data path with
 * dap_handle_request_reset(RESET_HANDLE) and resumes the UART bridge.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <mbedtls/sha256.h>

#include "DAP_handle.h"

#include "tf_dap.h"
#include "tf_internal.h"
#include "tf_stm32_id.h"
#include "tf_swd.h"
#include "wt_target_flash.h"

/* UART bridge pause/resume (transport-dev, optional at link time). */
extern esp_err_t wt_uart_bridge_pause(void) __attribute__((weak));
extern esp_err_t wt_uart_bridge_resume(void) __attribute__((weak));

#define TF_PROG_BUF_SIZE     1024U
#define TF_READBACK_CHUNK    480U

static SemaphoreHandle_t s_status_lock;
static bool s_inited;

static wt_tf_status_t s_status;
static wt_tf_info_t s_info;
static tf_target_t s_target;

static bool s_session_active;
static bool s_core_halted;
static volatile bool s_abort_requested;
static bool s_dap_held;
static bool s_uart_paused;

static uint32_t s_image_size;
static uint32_t s_received;
static uint32_t s_programmed;
static uint8_t s_buf[TF_PROG_BUF_SIZE];
static size_t s_buf_len;
static mbedtls_sha256_context s_sha;
static bool s_sha_active;
static uint8_t s_sha_expected[32];
static uint8_t s_sha_actual[32];
static uint64_t s_op_start_ms;
static tf_debug_result_t s_debug_result;
/* P0-8: actual SWD clock of the last successful link bring-up (0 = none yet).
 * Written by the target worker task, read by GET /api/target/status. */
static volatile uint32_t s_actual_swd_hz;

/* ------------------------------------------------------------------------- */
/* init / locks                                                              */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* engine worker task                                                        */
/* ------------------------------------------------------------------------- */
/*
 * All CMSIS-DAP/SWD work runs in a dedicated worker task pinned to core 0
 * (the DAP task's core) with an 8 KiB stack.  Callers (httpd, request_runner)
 * only receive/queue data and block on a semaphore, so their 4 KiB stacks are
 * never used for the deep DAP command path (hardware bring-up found that
 * running it in the httpd task overflowed the stack).
 *
 * The job mutex is the "one operation at a time" gate: a concurrent API call
 * fails with ESP_ERR_INVALID_STATE instead of blocking.
 */
typedef struct {
    esp_err_t (*fn)(void *arg);
    void *arg;
    esp_err_t result;
} tf_job_t;

static SemaphoreHandle_t s_job_mutex;
static SemaphoreHandle_t s_job_done;
static TaskHandle_t s_worker;
static tf_job_t s_job;

static void tf_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_job.fn != NULL) {
            s_job.result = s_job.fn(s_job.arg);
        }
        xSemaphoreGive(s_job_done);
    }
}

#if defined(portNUM_PROCESSORS) && (portNUM_PROCESSORS > 1)
/* Core-1 dedicated-GPIO signals (S3) / CPU GPIO CSRs (C3) are per-core. */
#define TF_WORKER_CORE 1
#else
/* Single-core targets (ESP32-C5, ESP32, ESP32-C3): core 1 does not exist and
 * xTaskCreatePinnedToCore(..., 1) would trip the FreeRTOS core-id assert. */
#define TF_WORKER_CORE 0
#endif

static esp_err_t tf_worker_start(void)
{
    BaseType_t created;

    s_job_mutex = xSemaphoreCreateMutex();
    s_job_done = xSemaphoreCreateBinary();
    if (s_job_mutex == NULL || s_job_done == NULL) {
        return ESP_FAIL;
    }
    /* The SWD bit-bang uses per-core dedicated-GPIO signals, so the DAP
     * command path must run on the DAP core - the same reason the DAP task
     * and tcp_server are pinned there (core 1 on S3, core 0 on unicore). */
    created = xTaskCreatePinnedToCore(tf_worker_task, "tf_worker", 8192, NULL,
                                      6 /* above httpd (5), below tcp_server/DAP */,
                                      &s_worker, TF_WORKER_CORE);
    if (created != pdPASS) {
        s_worker = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wt_tf_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_status_lock = xSemaphoreCreateMutex();
    if (s_status_lock == NULL) {
        return ESP_FAIL;
    }
    if (tf_worker_start() != ESP_OK) {
        return ESP_FAIL;
    }
    s_inited = true;
    ESP_LOGI(TF_TAG, "target flash engine initialized (worker task, core %d, 8 KiB)",
             TF_WORKER_CORE);
    return ESP_OK;
}

static esp_err_t tf_ensure_init(void)
{
    return s_inited ? ESP_OK : wt_tf_init();
}

/** Serialize one engine operation and run it on the worker task. */
static esp_err_t tf_submit(esp_err_t (*fn)(void *arg), void *arg)
{
    esp_err_t err;

    if (s_worker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_job_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_job.fn = fn;
    s_job.arg = arg;
    s_job.result = ESP_FAIL;
    xTaskNotifyGive(s_worker);
    if (xSemaphoreTake(s_job_done, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(s_job_mutex);
        return ESP_FAIL;
    }
    err = s_job.result;
    s_job.fn = NULL;
    s_job.arg = NULL;
    xSemaphoreGive(s_job_mutex);
    return err;
}

static void tf_status_lock(void)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
}

static void tf_status_unlock(void)
{
    xSemaphoreGive(s_status_lock);
}

static void tf_set_message(const char *fmt, ...)
{
    va_list ap;

    if (fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    tf_status_lock();
    vsnprintf(s_status.message, sizeof(s_status.message), fmt, ap);
    tf_status_unlock();
    va_end(ap);
}

static void tf_set_state(wt_tf_state_t state)
{
    tf_status_lock();
    s_status.state = state;
    tf_status_unlock();
}

static void tf_set_error(esp_err_t err, const char *msg)
{
    tf_status_lock();
    s_status.state = WT_TF_ERROR;
    s_status.err = (int)err;
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             (msg != NULL) ? msg : esp_err_to_name(err));
    tf_status_unlock();
}

static void tf_update_progress(uint32_t written, uint32_t verified)
{
    uint32_t total;

    tf_status_lock();
    total = (s_status.total != 0U) ? s_status.total : 1U;
    s_status.written = written;
    s_status.verified = verified;
    if (s_status.state == WT_TF_ERASING) {
        s_status.percent = 5;
    } else if (s_status.state == WT_TF_PROGRAMMING) {
        uint32_t pct = (written <= total) ? (written * 60U) / total : 60U;
        s_status.percent = (int)(5U + pct);
    } else if (s_status.state == WT_TF_VERIFYING) {
        uint32_t pct = (verified <= total) ? (verified * 35U) / total : 35U;
        s_status.percent = (int)(65U + pct);
    }
    tf_status_unlock();
}

/* ------------------------------------------------------------------------- */
/* DAP ownership + UART bridge pause                                         */
/* ------------------------------------------------------------------------- */

static void tf_uart_resume(void)
{
    if (!s_uart_paused) {
        return;
    }
    s_uart_paused = false;
    if (wt_uart_bridge_resume != NULL) {
        esp_err_t err = wt_uart_bridge_resume();
        if (err == ESP_OK) {
            ESP_LOGI(TF_TAG, "uart bridge resumed");
        } else {
            ESP_LOGW(TF_TAG, "uart bridge resume failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TF_TAG, "uart bridge resume skipped (API not linked)");
    }
}

static void tf_uart_pause(void)
{
    if (s_uart_paused) {
        return;
    }
    if (wt_uart_bridge_pause != NULL) {
        esp_err_t err = wt_uart_bridge_pause();
        if (err == ESP_OK) {
            s_uart_paused = true;
            ESP_LOGI(TF_TAG, "uart bridge paused");
        } else {
            ESP_LOGW(TF_TAG, "uart bridge pause failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TF_TAG, "uart bridge pause skipped (API not linked)");
    }
}

static esp_err_t tf_dap_acquire(void)
{
    if (s_dap_held) {
        return ESP_OK;
    }
    tf_uart_pause();
    dap_handle_request_reset(DELETE_HANDLE);
    if (dap_handle_wait_reset(TF_DAP_HANDOFF_TIMEOUT_MS) != 0) {
        ESP_LOGE(TF_TAG, "DAP hand-off timed out (%u ms)", (unsigned)TF_DAP_HANDOFF_TIMEOUT_MS);
        tf_uart_resume();
        return ESP_ERR_TIMEOUT;
    }
    s_dap_held = true;
    ESP_LOGI(TF_TAG, "DAP acquired for target flash");
    return ESP_OK;
}

static void tf_dap_release(void)
{
    if (!s_dap_held) {
        tf_uart_resume();
        return;
    }
    dap_handle_request_reset(RESET_HANDLE);
    (void)dap_handle_wait_reset(TF_DAP_RESTORE_TIMEOUT_MS);
    s_dap_held = false;
    ESP_LOGI(TF_TAG, "DAP released (USBIP/WS data path restored)");
    tf_uart_resume();
}

/* ------------------------------------------------------------------------- */
/* target probe                                                              */
/* ------------------------------------------------------------------------- */

static void tf_info_clear(void)
{
    memset(&s_info, 0, sizeof(s_info));
    snprintf(s_info.family, sizeof(s_info.family), "unknown");
    snprintf(s_info.series, sizeof(s_info.series), "unknown");
    snprintf(s_info.device, sizeof(s_info.device), "unknown");
    snprintf(s_info.model_candidates, sizeof(s_info.model_candidates), "unknown");
    snprintf(s_info.core, sizeof(s_info.core), "unknown");
    s_info.supported = false;
    s_info.confidence = (uint8_t)WT_TF_CONF_UNKNOWN;
    memset(&s_target, 0, sizeof(s_target));
    s_target.family = TF_FAMILY_UNKNOWN;
    snprintf(s_target.family_str, sizeof(s_target.family_str), "unknown");
    snprintf(s_target.series, sizeof(s_target.series), "unknown");
    snprintf(s_target.device, sizeof(s_target.device), "unknown");
    snprintf(s_target.model_candidates, sizeof(s_target.model_candidates), "unknown");
    snprintf(s_target.core, sizeof(s_target.core), "unknown");
    s_target.confidence = (uint8_t)WT_TF_CONF_UNKNOWN;
}

static void tf_info_fill_from_target(void)
{
    memset(&s_info, 0, sizeof(s_info));
    s_info.detected = true;
    snprintf(s_info.family, sizeof(s_info.family), "%s", s_target.family_str);
    snprintf(s_info.series, sizeof(s_info.series), "%s", s_target.series);
    snprintf(s_info.device, sizeof(s_info.device), "%s", s_target.device);
    snprintf(s_info.model_candidates, sizeof(s_info.model_candidates), "%s",
             s_target.model_candidates);
    snprintf(s_info.core, sizeof(s_info.core), "%s", s_target.core);
    s_info.supported = s_target.supported;
    s_info.swd_clock_hz = s_target.swd_clock_hz;
    s_info.dpidr = s_target.dpidr;
    s_info.cpuid = s_target.cpuid;
    s_info.idcode = s_target.idcode;
    memcpy(s_info.uid, s_target.uid, sizeof(s_info.uid));
    s_info.flash_base = s_target.flash_base;
    s_info.flash_size = s_target.flash_size;
    s_info.max_image_size = s_target.max_image_size;
    s_info.dev_id = s_target.dev_id;
    s_info.flash_kib = s_target.flash_size / 1024U;
    s_info.confidence = s_target.confidence;
}

/* DBGMCU IDCODE sanity: DEV_ID in bits[11:0], REV_ID in bits[31:16]. */
static bool tf_idcode_sane(uint32_t idcode)
{
    uint32_t dev_id = idcode & 0xFFFU;
    uint32_t rev_id = (idcode >> 16) & 0xFFFFU;

    return (dev_id != 0U) && (dev_id != 0xFFFU) && (rev_id != 0xFFFFU);
}

/* "STM32H7" -> "stm32h7" (public family field keeps the old lower-case form). */
static void tf_series_to_family(const char *series, char *out, size_t out_len)
{
    size_t i;

    for (i = 0U; (i + 1U) < out_len && series[i] != '\0'; i++) {
        char c = series[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

/* DEV_ID groups cover several order codes.  The narrowing rules live in
 * tf_stm32_id.c (tf_stm32_model_group/tf_stm32_model_candidates) so they can
 * be covered by host unit tests: the historical H7/F4 special cases narrow the
 * old `device` string, while `model_candidates` additionally gets the standard
 * order-code density hint from FSIZE ("... [x8, 64 KiB]").  Package and exact
 * order code remain unknowable over SWD (the Web layer compares against a
 * user-selected MCU/board profile). */

/* Map the table's flash_ops enum to the runtime ops vtable + family id. */
static const tf_family_ops_t *tf_ops_for_part(const tf_stm32_part_t *part,
                                              tf_family_t *family)
{
    switch (part->flash_ops) {
    case TF_STM32_FLASH_OPS_F4:
        *family = TF_FAMILY_STM32F4;
        return &tf_family_stm32f4_ops;
    case TF_STM32_FLASH_OPS_F7:
        *family = TF_FAMILY_STM32F7;
        return &tf_family_stm32f7_ops;
    case TF_STM32_FLASH_OPS_F1X:
        *family = TF_FAMILY_STM32F1;
        return &tf_family_stm32f1_ops;
    case TF_STM32_FLASH_OPS_G4:
        *family = TF_FAMILY_STM32G4;
        return &tf_family_stm32g4_ops;
    case TF_STM32_FLASH_OPS_L4:
        *family = TF_FAMILY_STM32L4;
        return &tf_family_stm32l4_ops;
    case TF_STM32_FLASH_OPS_G0:
        *family = TF_FAMILY_STM32G0;
        return &tf_family_stm32g0_ops;
    case TF_STM32_FLASH_OPS_WB:
        *family = TF_FAMILY_STM32WB;
        return &tf_family_stm32wb_ops;
    case TF_STM32_FLASH_OPS_WL:
        *family = TF_FAMILY_STM32WL;
        return &tf_family_stm32wl_ops;
    case TF_STM32_FLASH_OPS_H7:
        *family = TF_FAMILY_STM32H7;
        return &tf_family_stm32h7_ops;
    default:
        *family = TF_FAMILY_STM32_IDENTIFIED;
        return NULL;
    }
}

/* Fill s_target for an identified table entry.  Entries with a flash_ops
 * implementation run the family probe (it re-validates DEV_ID/CPUID and reads
 * FSIZE/UID); other series only get generic FSIZE/UID reads and are reported as
 * identified-but-unsupported.  A family probe may also report
 * ESP_ERR_NOT_SUPPORTED for a valid DEV_ID whose size/option-bit combination is
 * not modelled (e.g. 2 MiB dual-bank F4/F7): such a part still keeps the
 * identification data and is marked unsupported instead of failing the whole
 * detection. */
static esp_err_t tf_target_apply_part(const tf_stm32_part_t *part, uint32_t idcode,
                                      uint32_t cpuid, uint32_t clock_hz)
{
    tf_core_class_t core = tf_stm32_core_from_cpuid(cpuid);
    uint32_t flash_size = 0U;
    uint32_t uid[3] = {0U, 0U, 0U};
    char model[56];
    const char *model_str;
    esp_err_t err = ESP_OK;

    s_target.idcode = idcode;
    s_target.cpuid = cpuid;
    s_target.flash_base = TF_MAIN_FLASH_BASE;
    s_target.f4_x32_mode = false;
    s_target.swd_clock_hz = clock_hz;
    s_target.dev_id = part->dev_id;
    s_target.flash_ops = part->flash_ops;
    snprintf(s_target.series, sizeof(s_target.series), "%s", part->series);
    tf_series_to_family(part->series, s_target.family_str, sizeof(s_target.family_str));

    s_target.ops = tf_ops_for_part(part, &s_target.family);
    s_target.supported = false;
    if (s_target.ops != NULL) {
        err = s_target.ops->probe(&s_target);
        if (err == ESP_OK) {
            flash_size = s_target.flash_size;
            s_target.supported = true;
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            /* Valid part, but this size/option-bit variant has no modelled
             * flash layout: keep the identification and downgrade. */
            ESP_LOGW(TF_TAG, "target: DEV_ID 0x%03X (%s) flash variant unsupported",
                     (unsigned)part->dev_id, part->model);
            s_target.ops = NULL;
            s_target.family = TF_FAMILY_STM32_IDENTIFIED;
            s_target.supported = false;
            s_target.flash_size = 0U;
            s_target.max_image_size = 0U;
            memset(s_target.uid, 0, sizeof(s_target.uid));
            tf_series_to_family(part->series, s_target.family_str,
                                sizeof(s_target.family_str));
        } else {
            return err;
        }
    }

    if (!s_target.supported) {
        if (part->flash_size_reg != 0U && part->flash_size_shift != 0xFFU) {
            uint32_t reg = 0U;
            if (tf_swd_read_u32(part->flash_size_reg, &reg) == ESP_OK) {
                uint32_t kb = (reg >> part->flash_size_shift) & 0xFFFFU;
                if (kb >= 16U && kb <= 8192U) {
                    flash_size = kb * 1024U;
                }
            } else {
                (void)tf_swd_recover_bus();
            }
        }
        if (part->uid_base != 0U) {
            if (tf_swd_read_mem(part->uid_base, uid, sizeof(uid)) != ESP_OK) {
                memset(uid, 0, sizeof(uid));
                (void)tf_swd_recover_bus();
            }
        }
        memcpy(s_target.uid, uid, sizeof(s_target.uid));
        s_target.flash_size = flash_size;
        s_target.max_image_size = flash_size;
    }

    model_str = tf_stm32_model_group(part->dev_id, flash_size, part->model, model, sizeof(model));
    snprintf(s_target.device, sizeof(s_target.device), "%s", model_str);
    s_target.dev_id = part->dev_id;
    snprintf(s_target.core, sizeof(s_target.core), "%s", tf_stm32_core_name(core));
    {
        tf_narrow_t narrow = TF_NARROW_UNKNOWN;
        (void)tf_stm32_model_candidates(part->dev_id, flash_size, part->model,
                                        s_target.model_candidates,
                                        sizeof(s_target.model_candidates), &narrow);
        s_target.confidence = (uint8_t)narrow;
    }

    ESP_LOGI(TF_TAG,
             "target identified: %s (%s) DEV_ID=0x%03X REV=0x%04X CPUID=0x%08X (%s) "
             "flash=%u KiB UID@0x%08X supported=%d confidence=%u",
             s_target.model_candidates, s_target.series, (unsigned)(idcode & 0xFFFU),
             (unsigned)((idcode >> 16) & 0xFFFFU), (unsigned)cpuid,
             s_target.core, (unsigned)(flash_size / 1024U),
             (unsigned)part->uid_base, (int)s_target.supported,
             (unsigned)s_target.confidence);
    return ESP_OK;
}

/* CPUID is readable but no DBGMCU IDCODE matched: still a real STM32. */
static void tf_target_set_unknown(uint32_t idcode, uint32_t cpuid, uint32_t dpidr,
                                  uint32_t clock_hz)
{
    memset(&s_target, 0, sizeof(s_target));
    s_target.family = TF_FAMILY_STM32_IDENTIFIED;
    snprintf(s_target.family_str, sizeof(s_target.family_str), "stm32");
    snprintf(s_target.series, sizeof(s_target.series), "unknown");
    snprintf(s_target.core, sizeof(s_target.core), "%s",
             tf_stm32_core_name(tf_stm32_core_from_cpuid(cpuid)));
    if (idcode != 0U) {
        snprintf(s_target.device, sizeof(s_target.device), "unknown (DEV_ID=0x%03X)",
                 (unsigned)(idcode & 0xFFFU));
        s_target.dev_id = (uint16_t)(idcode & 0xFFFU);
    } else {
        snprintf(s_target.device, sizeof(s_target.device), "unknown (no DBGMCU IDCODE)");
        s_target.dev_id = 0U;
    }
    snprintf(s_target.model_candidates, sizeof(s_target.model_candidates), "%s",
             s_target.device);
    s_target.confidence = (uint8_t)WT_TF_CONF_UNKNOWN;
    s_target.dpidr = dpidr;
    s_target.swd_clock_hz = clock_hz;
}

/**
 * Link the DAP to SWD and identify the target; fills s_target/s_info.
 * The caller must already own the DAP engine.
 *
 * Automatic SWD clock probe (P0-8): 4 MHz -> 2 MHz -> 1 MHz -> 500 kHz.  The
 * STM32H7 setup in the field only answers at <=1 MHz (OpenOCD: "cannot read
 * IDR" at 2 MHz), while F4 boards usually work at the faster clocks; the first
 * clock that identifies the target is kept and reported as the actual clock.
 * A manual clock (wt_tf_set_swd_clock) skips the walk.  Each attempt re-runs
 * the full line-reset/JTAG-to-SWD/DP power-up sequence with the new clock.
 */
esp_err_t wt_tf_set_swd_clock(uint32_t clock_hz)
{
    if (clock_hz != 0U && (clock_hz < 100000U || clock_hz > 10000000U)) {
        return ESP_ERR_INVALID_ARG;
    }
    tf_swd_set_forced_clock(clock_hz);
    return ESP_OK;
}

uint32_t wt_tf_get_swd_clock(void)
{
    return tf_swd_get_forced_clock();
}

static esp_err_t tf_probe_link(void)
{
    static const uint32_t clock_try[] = {
        TF_SWD_CLOCK_HZ, TF_SWD_CLOCK_FALLBACK1_HZ, TF_SWD_CLOCK_FALLBACK2_HZ,
        TF_SWD_CLOCK_FALLBACK3_HZ
    };
    uint32_t forced_hz = tf_swd_get_forced_clock();
    uint32_t forced_try[1] = { forced_hz };
    const uint32_t *clocks = forced_hz != 0U ? forced_try : clock_try;
    size_t clock_count = forced_hz != 0U ? 1U : (sizeof(clock_try) / sizeof(clock_try[0]));
    esp_err_t last_err = ESP_ERR_NOT_FOUND;
    bool saw_cpuid = false;

    tf_info_clear();

    for (size_t ci = 0U; ci < clock_count; ci++) {
        uint32_t clock_hz = clocks[ci];
        uint32_t dpidr = 0U;
        uint32_t cpuid = 0U;
        esp_err_t err;

        TF_STAGE(20U);
        err = tf_swd_link_init(clock_hz, &dpidr);
        if (err != ESP_OK) {
            last_err = err;
            ESP_LOGW(TF_TAG, "probe: SWD link init @%u Hz failed (%s)",
                     (unsigned)clock_hz, esp_err_to_name(err));
            (void)tf_swd_recover_bus();
            continue;
        }
        s_target.dpidr = dpidr;
        s_target.swd_clock_hz = clock_hz;
        /* P0-8: remember the clock that actually brought the link up so the
         * status endpoint reports the real value (not the requested max). */
        s_actual_swd_hz = clock_hz;

        TF_STAGE(21U);
        if (tf_swd_read_u32(TF_REG_CPUID, &cpuid) != ESP_OK) {
            last_err = ESP_ERR_NOT_FOUND;
            ESP_LOGW(TF_TAG, "probe: CPUID read @%u Hz failed", (unsigned)clock_hz);
            (void)tf_swd_recover_bus();
            continue;
        }
        s_target.cpuid = cpuid;
        saw_cpuid = true;
        TF_STAGE(22U);

        /* Try the DBGMCU bases ordered by the CPU core class; a wrong base
         * FAULTs and is skipped after clearing the sticky error. */
        {
            tf_core_class_t core = tf_stm32_core_from_cpuid(cpuid);
            const uint32_t *bases = NULL;
            size_t base_count = tf_stm32_dbgmc_probe_bases(core, &bases);
            uint32_t unknown_idcode = 0U;
            bool have_unknown = false;

            ESP_LOGI(TF_TAG, "probe @%u Hz: DPIDR=0x%08X CPUID=0x%08X (%s)",
                     (unsigned)clock_hz, (unsigned)dpidr, (unsigned)cpuid,
                     tf_stm32_core_name(core));
            for (size_t bi = 0U; bi < base_count; bi++) {
                uint32_t idcode = 0U;
                const tf_stm32_part_t *part;

                if (tf_swd_read_u32(bases[bi], &idcode) != ESP_OK) {
                    (void)tf_swd_recover_bus();
                    continue;
                }
                if (!tf_idcode_sane(idcode)) {
                    (void)tf_swd_recover_bus();
                    continue;
                }
                part = tf_stm32_part_find((uint16_t)(idcode & 0xFFFU));
                if (part != NULL) {
                    err = tf_target_apply_part(part, idcode, cpuid, clock_hz);
                    if (err == ESP_OK) {
                        tf_info_fill_from_target();
                        return ESP_OK;
                    }
                    last_err = err;
                    ESP_LOGW(TF_TAG, "probe: DEV_ID 0x%03X apply failed (%s)",
                             (unsigned)(idcode & 0xFFFU), esp_err_to_name(err));
                    (void)tf_swd_recover_bus();
                    continue;
                }
                if (!have_unknown) {
                    have_unknown = true;
                    unknown_idcode = idcode;
                }
                (void)tf_swd_recover_bus();
            }

            if (have_unknown) {
                tf_target_set_unknown(unknown_idcode, cpuid, dpidr, clock_hz);
                tf_info_fill_from_target();
                ESP_LOGW(TF_TAG, "probe: unknown DEV_ID 0x%03X at @%u Hz",
                         (unsigned)(unknown_idcode & 0xFFFU), (unsigned)clock_hz);
                return ESP_OK;
            }
        }

        last_err = ESP_ERR_NOT_FOUND;
        ESP_LOGW(TF_TAG, "probe: no DBGMCU IDCODE @%u Hz", (unsigned)clock_hz);
        (void)tf_swd_recover_bus();
    }

    if (saw_cpuid) {
        /* CPUID answered but DBGMCU was unreadable on every base/speed: report
         * a detected STM32 rather than "not detected" (unsupported for flash). */
        uint32_t last_cpuid = s_target.cpuid;
        uint32_t last_dpidr = s_target.dpidr;
        tf_target_set_unknown(0U, last_cpuid, last_dpidr, s_target.swd_clock_hz);
        tf_info_fill_from_target();
        return ESP_OK;
    }

    snprintf(s_info.family, sizeof(s_info.family), "unknown");
    return last_err;
}

/* ------------------------------------------------------------------------- */
/* session cleanup / failure                                                 */
/* ------------------------------------------------------------------------- */

/** Best-effort: lock flash, reset the target, release DAP, mark the error. */
static void tf_session_fail(esp_err_t err, const char *msg)
{
    bool halted = s_core_halted;

    if (s_target.ops != NULL && s_target.ops->program_end != NULL) {
        (void)s_target.ops->program_end(&s_target);
    }
    if (halted && s_target.ops != NULL) {
        (void)tf_swd_sysreset(true);
    }
    s_core_halted = false;
    s_session_active = false;
    s_sha_active = false;
    tf_dap_release();
    tf_set_error(err, msg);
    ESP_LOGE(TF_TAG, "flash session failed: %s", msg ? msg : esp_err_to_name(err));
}

static bool tf_abort_check(void)
{
    return s_abort_requested;
}

/* ------------------------------------------------------------------------- */
/* programming buffer                                                        */
/* ------------------------------------------------------------------------- */

static size_t tf_program_unit(void)
{
    uint32_t unit = tf_stm32_flash_program_unit(s_target.flash_ops);

    /* Unsupported/unknown parts never reach programming, but the tail flush in
     * tf_job_flash_end calls this only after a supported probe.  Keep an 8-byte
     * fallback for robustness. */
    return (unit != 0U) ? (size_t)unit : 8U;
}

static esp_err_t tf_program_block(const uint8_t *data, size_t len)
{
    esp_err_t err;

    if (len == 0U) {
        return ESP_OK;
    }
    if (tf_abort_check()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = s_target.ops->program(&s_target, s_target.flash_base + s_programmed, data, len);
    if (err != ESP_OK) {
        return err;
    }
    s_programmed += (uint32_t)len;
    return ESP_OK;
}

/** Program the full staging buffer when possible. */
static esp_err_t tf_flush_full_buffer(void)
{
    esp_err_t err = tf_program_block(s_buf, s_buf_len);

    if (err == ESP_OK) {
        s_buf_len = 0U;
        tf_update_progress(s_programmed, 0U);
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* worker jobs (all of these run on the tf_worker task, never on the caller) */
/* ------------------------------------------------------------------------- */

typedef struct {
    wt_tf_info_t *info;
} tf_job_probe_arg_t;

typedef struct {
    uint32_t image_size;
} tf_job_begin_arg_t;

typedef struct {
    const void *data;
    size_t len;
} tf_job_feed_arg_t;

typedef struct {
    bool run;
} tf_job_reset_arg_t;

typedef struct {
    char *out;
    size_t out_len;
} tf_job_diag_arg_t;

typedef struct {
    uint32_t *pc;
    bool *halted;
    bool *lockup;
} tf_job_pc_arg_t;

static esp_err_t tf_job_probe(void *arg)
{
    tf_job_probe_arg_t *a = (tf_job_probe_arg_t *)arg;
    esp_err_t err;

    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    tf_set_state(WT_TF_IDLE);
    err = tf_dap_acquire();
    if (err == ESP_OK) {
        err = tf_probe_link();
        tf_dap_release();
    }
    if (err == ESP_OK) {
        if (a->info != NULL) {
            *a->info = s_info;
        }
        if (s_info.supported) {
            tf_set_message("probe ok: %s @%u kHz", s_info.device,
                           (unsigned)(s_info.swd_clock_hz / 1000U));
        } else {
            tf_set_message("identified %s (%s), flash unsupported", s_info.device,
                           s_info.series);
        }
        ESP_LOGI(TF_TAG, "probe result: family=%s series=%s device=%s supported=%d swd=%u Hz",
                 s_info.family, s_info.series, s_info.device, (int)s_info.supported,
                 (unsigned)s_info.swd_clock_hz);
        s_debug_result.info = s_info;
    } else {
        snprintf(s_info.family, sizeof(s_info.family), "unknown");
        tf_set_message("probe failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t tf_job_flash_begin(void *arg)
{
    tf_job_begin_arg_t *a = (tf_job_begin_arg_t *)arg;
    esp_err_t err;

    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }

    tf_status_lock();
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WT_TF_ERASING;
    s_status.total = a->image_size;
    snprintf(s_status.message, sizeof(s_status.message), "erasing");
    tf_status_unlock();
    s_abort_requested = false;
    s_image_size = a->image_size;
    s_received = 0U;
    s_programmed = 0U;
    s_buf_len = 0U;
    s_core_halted = false;
    s_session_active = true;
    s_op_start_ms = tf_now_ms();

    ESP_LOGI(TF_TAG, "flash begin: image %u bytes", (unsigned)a->image_size);

    err = tf_dap_acquire();
    if (err != ESP_OK) {
        s_session_active = false;
        tf_set_error(err, "DAP hand-off failed");
        return err;
    }
    err = tf_probe_link();
    if (err != ESP_OK) {
        tf_session_fail(err, "target probe failed");
        return err;
    }
    if (!s_target.supported || s_target.ops == NULL) {
        tf_session_fail(ESP_ERR_NOT_SUPPORTED,
                        "target identified but flash not supported for this series");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (a->image_size > s_target.max_image_size) {
        tf_session_fail(ESP_ERR_INVALID_SIZE, "image larger than target flash");
        return ESP_ERR_INVALID_SIZE;
    }
    err = tf_swd_halt();
    if (err != ESP_OK) {
        tf_session_fail(err, "target halt failed");
        return err;
    }
    s_core_halted = true;

    err = s_target.ops->program_begin(&s_target);
    if (err != ESP_OK) {
        tf_session_fail(err, "flash unlock failed");
        return err;
    }
    err = s_target.ops->erase(&s_target, s_target.flash_base, a->image_size);
    if (err != ESP_OK) {
        tf_session_fail(err, "flash erase failed");
        return err;
    }
    if (tf_abort_check()) {
        tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
        return ESP_ERR_INVALID_STATE;
    }

    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    s_sha_active = true;
    tf_status_lock();
    s_status.state = WT_TF_PROGRAMMING;
    s_status.percent = 5;
    snprintf(s_status.message, sizeof(s_status.message), "programming %s",
             s_target.family_str);
    s_status.err = 0;
    tf_status_unlock();

    ESP_LOGI(TF_TAG, "flash begin done: %s, %u KiB erased coverage, DAP held",
             s_target.family_str, (unsigned)((a->image_size + 1023U) / 1024U));
    return ESP_OK;
}

static esp_err_t tf_job_flash_feed(void *arg)
{
    tf_job_feed_arg_t *a = (tf_job_feed_arg_t *)arg;
    const uint8_t *src = (const uint8_t *)a->data;
    size_t len = a->len;

    if (a->data == NULL || a->len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_active || s_status.state != WT_TF_PROGRAMMING) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint64_t)s_received + len > (uint64_t)s_image_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (tf_abort_check()) {
        tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
        return ESP_ERR_INVALID_STATE;
    }

    mbedtls_sha256_update(&s_sha, src, len);
    s_received += (uint32_t)len;

    while (len > 0U) {
        size_t space = sizeof(s_buf) - s_buf_len;
        size_t take = (len < space) ? len : space;

        memcpy(s_buf + s_buf_len, src, take);
        s_buf_len += take;
        src += take;
        len -= take;

        if (s_buf_len == sizeof(s_buf)) {
            esp_err_t err = tf_flush_full_buffer();
            if (err != ESP_OK) {
                tf_session_fail(err, "programming failed");
                return err;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t tf_job_flash_end(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
    size_t unit;
    uint32_t offset;
    mbedtls_sha256_context readback;
    uint8_t chunk[TF_READBACK_CHUNK];
    uint32_t verified = 0U;
    bool sha_match;

    if (!s_session_active || s_status.state != WT_TF_PROGRAMMING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_received != s_image_size) {
        tf_session_fail(ESP_ERR_INVALID_SIZE, "image size mismatch (feed incomplete)");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Flush the tail, padded to one program unit with erased-flash value. */
    unit = tf_program_unit();
    if (s_buf_len > 0U) {
        size_t padded = (s_buf_len + unit - 1U) / unit * unit;
        if (padded > sizeof(s_buf)) {
            padded = sizeof(s_buf);
        }
        memset(s_buf + s_buf_len, 0xFF, padded - s_buf_len);
        err = tf_program_block(s_buf, padded);
        if (err != ESP_OK) {
            tf_session_fail(err, "tail programming failed");
            return err;
        }
        s_buf_len = 0U;
    }
    tf_update_progress(s_image_size, 0U);

    if (tf_abort_check()) {
        tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
        return ESP_ERR_INVALID_STATE;
    }

    /* Finalize the source digest. */
    if (mbedtls_sha256_finish(&s_sha, s_sha_expected) != 0) {
        tf_session_fail(ESP_FAIL, "sha256 finalize failed");
        return ESP_FAIL;
    }
    s_sha_active = false;

    /* Lock the flash before verification/reset. */
    err = s_target.ops->program_end(&s_target);
    if (err != ESP_OK) {
        tf_session_fail(err, "flash lock failed");
        return err;
    }

    tf_set_state(WT_TF_VERIFYING);
    tf_status_lock();
    snprintf(s_status.message, sizeof(s_status.message), "verifying sha256");
    tf_status_unlock();
    tf_update_progress(s_image_size, 0U);

    mbedtls_sha256_init(&readback);
    mbedtls_sha256_starts(&readback, 0);
    offset = 0U;
    while (offset < s_image_size) {
        uint32_t remain = s_image_size - offset;
        uint32_t chunk_len = (remain < TF_READBACK_CHUNK) ? remain : TF_READBACK_CHUNK;

        if (tf_abort_check()) {
            mbedtls_sha256_free(&readback);
            tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
            return ESP_ERR_INVALID_STATE;
        }
        err = tf_swd_read_mem(s_target.flash_base + offset, chunk, chunk_len);
        if (err != ESP_OK) {
            mbedtls_sha256_free(&readback);
            tf_session_fail(err, "flash read-back failed");
            return err;
        }
        mbedtls_sha256_update(&readback, chunk, chunk_len);
        offset += chunk_len;
        verified += chunk_len;
        tf_update_progress(s_image_size, verified);
    }
    if (mbedtls_sha256_finish(&readback, s_sha_actual) != 0) {
        mbedtls_sha256_free(&readback);
        tf_session_fail(ESP_FAIL, "sha256 read-back finalize failed");
        return ESP_FAIL;
    }
    mbedtls_sha256_free(&readback);

    sha_match = (memcmp(s_sha_expected, s_sha_actual, sizeof(s_sha_expected)) == 0);
    if (!sha_match) {
        tf_session_fail(ESP_ERR_INVALID_CRC, "verify sha256 mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    tf_set_state(WT_TF_RESETTING);
    tf_status_lock();
    s_status.percent = 98;
    snprintf(s_status.message, sizeof(s_status.message), "resetting target");
    tf_status_unlock();
    err = tf_swd_sysreset(true);
    if (err != ESP_OK) {
        tf_session_fail(err, "target reset failed");
        return err;
    }
    s_core_halted = false;
    s_session_active = false;
    tf_dap_release();

    {
        uint32_t elapsed = (uint32_t)(tf_now_ms() - s_op_start_ms);
        char exp_hex[65];
        char act_hex[65];

        for (size_t i = 0U; i < 32U; i++) {
            snprintf(&exp_hex[i * 2U], 3U, "%02x", s_sha_expected[i]);
            snprintf(&act_hex[i * 2U], 3U, "%02x", s_sha_actual[i]);
        }
        tf_status_lock();
        s_status.state = WT_TF_DONE;
        s_status.percent = 100;
        s_status.verified = s_image_size;
        s_status.err = 0;
        snprintf(s_status.message, sizeof(s_status.message),
                 "done sha256=%02x%02x%02x%02x %ums",
                 s_sha_expected[0], s_sha_expected[1], s_sha_expected[2],
                 s_sha_expected[3], (unsigned)elapsed);
        tf_status_unlock();

        memset(&s_debug_result, 0, sizeof(s_debug_result));
        s_debug_result.valid = true;
        s_debug_result.sha_match = true;
        memcpy(s_debug_result.sha_expected, exp_hex, sizeof(exp_hex));
        memcpy(s_debug_result.sha_actual, act_hex, sizeof(act_hex));
        s_debug_result.image_size = s_image_size;
        s_debug_result.elapsed_ms = elapsed;
        s_debug_result.info = s_info;
        ESP_LOGI(TF_TAG, "flash done: %u bytes, sha256=%s, %u ms", (unsigned)s_image_size,
                 exp_hex, (unsigned)elapsed);
    }
    return ESP_OK;
}

static esp_err_t tf_job_reset_target(void *arg)
{
    tf_job_reset_arg_t *a = (tf_job_reset_arg_t *)arg;
    esp_err_t err;

    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    tf_set_state(WT_TF_RESETTING);
    err = tf_dap_acquire();
    if (err == ESP_OK) {
        err = tf_probe_link();
        if (err == ESP_OK) {
            err = tf_swd_sysreset(a->run);
        }
        tf_dap_release();
    }
    if (err == ESP_OK) {
        tf_set_state(WT_TF_IDLE);
        tf_set_message("target reset (%s)", a->run ? "run" : "halt");
    } else {
        tf_set_error(err, "target reset failed");
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t wt_tf_probe(wt_tf_info_t *info)
{
    esp_err_t err = tf_ensure_init();
    tf_job_probe_arg_t a = {.info = info};

    if (err != ESP_OK) {
        return err;
    }
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        snprintf(info->family, sizeof(info->family), "unknown");
    }
    return tf_submit(tf_job_probe, &a);
}

esp_err_t wt_tf_flash_begin(uint32_t image_size)
{
    esp_err_t err = tf_ensure_init();
    tf_job_begin_arg_t a = {.image_size = image_size};

    if (err != ESP_OK) {
        return err;
    }
    if (image_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return tf_submit(tf_job_flash_begin, &a);
}

esp_err_t wt_tf_flash_feed(const void *data, size_t len)
{
    esp_err_t err = tf_ensure_init();
    tf_job_feed_arg_t a = {.data = data, .len = len};

    if (err != ESP_OK) {
        return err;
    }
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return tf_submit(tf_job_flash_feed, &a);
}

esp_err_t wt_tf_flash_end(void)
{
    esp_err_t err = tf_ensure_init();

    if (err != ESP_OK) {
        return err;
    }
    return tf_submit(tf_job_flash_end, NULL);
}

esp_err_t wt_tf_abort(void)
{
    bool active;

    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    tf_status_lock();
    active = s_session_active &&
             (s_status.state == WT_TF_ERASING || s_status.state == WT_TF_PROGRAMMING ||
              s_status.state == WT_TF_VERIFYING);
    tf_status_unlock();
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_abort_requested = true;
    ESP_LOGW(TF_TAG, "flash abort requested");
    return ESP_OK;
}

esp_err_t wt_tf_reset_target(bool run)
{
    esp_err_t err = tf_ensure_init();
    tf_job_reset_arg_t a = {.run = run};

    if (err != ESP_OK) {
        return err;
    }
    return tf_submit(tf_job_reset_target, &a);
}

uint32_t wt_tf_get_actual_swd_clock(void)
{
    return s_actual_swd_hz;
}

void wt_tf_get_link_diag(wt_tf_link_diag_t *out)
{
    wifi_ap_record_t ap;
    dap_ring_info_t ring;
    dap_xfer_stats_t xfer;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* Wi-Fi STA link: esp_wifi_sta_get_ap_info() is read-only and returns
     * ESP_ERR_WIFI_NOT_CONNECT / NOT_INIT when there is no station link. */
    memset(&ap, 0, sizeof(ap));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->wifi_connected = true;
        out->wifi_rssi = ap.rssi;
        out->wifi_channel = ap.primary;
        memcpy(out->wifi_ssid, ap.ssid, sizeof(ap.ssid));
        out->wifi_ssid[sizeof(out->wifi_ssid) - 1U] = '\0';
    }

    out->heap_free = (uint32_t)esp_get_free_heap_size();
    out->heap_largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    out->swd_clock_actual_hz = s_actual_swd_hz;
    out->swd_clock_manual_hz = tf_swd_get_forced_clock();

    dap_handle_get_ring_info(&ring);
    out->dap_ring_preallocated = ring.preallocated;
    out->dap_ring_active = ring.active;
    out->dap_ring_request_bytes = ring.request_ring_bytes;
    out->dap_ring_response_bytes = ring.response_ring_bytes;

    dap_handle_get_xfer_stats(&xfer);
    out->dap_xfer_count = xfer.transactions;
    out->dap_xfer_avg_us = xfer.avg_us;
    out->dap_xfer_max_us = xfer.max_us;
    out->dap_xfer_last_us = xfer.last_us;
}

void wt_tf_get_status(wt_tf_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!s_inited) {
        memset(out, 0, sizeof(*out));
        out->state = WT_TF_IDLE;
        snprintf(out->message, sizeof(out->message), "not initialized");
        return;
    }
    tf_status_lock();
    *out = s_status;
    tf_status_unlock();
}

/* ------------------------------------------------------------------------- */
/* debug hooks (used by the Kconfig-gated /tf endpoint only)                 */
/* ------------------------------------------------------------------------- */

void tf_debug_get_result(tf_debug_result_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!s_inited) {
        memset(out, 0, sizeof(*out));
        return;
    }
    tf_status_lock();
    *out = s_debug_result;
    tf_status_unlock();
}

static esp_err_t tf_job_read_pc(void *arg)
{
    tf_job_pc_arg_t *a = (tf_job_pc_arg_t *)arg;
    esp_err_t err;
    uint32_t dhcsr = 0U;
    bool was_halted = false;

    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    err = tf_dap_acquire();
    if (err != ESP_OK) {
        return err;
    }
    err = tf_probe_link();
    if (err == ESP_OK) {
        err = tf_swd_read_u32(TF_REG_DHCSR, &dhcsr);
        if (err == ESP_OK) {
            was_halted = (dhcsr & TF_DHCSR_S_HALT) != 0U;
            if (a->halted != NULL) {
                *a->halted = was_halted;
            }
            if (a->lockup != NULL) {
                *a->lockup = (dhcsr & TF_DHCSR_S_LOCKUP) != 0U;
            }
            err = tf_swd_read_pc(a->pc);
        }
        if (!was_halted) {
            (void)tf_swd_resume();
        }
    }
    tf_dap_release();
    return err;
}

esp_err_t tf_debug_read_pc(uint32_t *pc, bool *halted, bool *lockup)
{
    esp_err_t err = tf_ensure_init();
    tf_job_pc_arg_t a = {.pc = pc, .halted = halted, .lockup = lockup};

    if (err != ESP_OK) {
        return err;
    }
    if (pc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return tf_submit(tf_job_read_pc, &a);
}

static esp_err_t tf_job_link_diag(void *arg)
{
    tf_job_diag_arg_t *a = (tf_job_diag_arg_t *)arg;
    esp_err_t err;

    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    err = tf_dap_acquire();
    if (err == ESP_OK) {
        err = tf_swd_link_diag(a->out, a->out_len);
        tf_dap_release();
    }
    return err;
}

esp_err_t tf_debug_link_diag(char *out, size_t out_len)
{
    esp_err_t err = tf_ensure_init();
    tf_job_diag_arg_t a = {.out = out, .out_len = out_len};

    if (err != ESP_OK) {
        return err;
    }
    if (out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return tf_submit(tf_job_link_diag, &a);
}

uint32_t tf_debug_last_stage(void)
{
    return tf_stage_get();
}

void tf_debug_stage_reset(void)
{
    tf_stage_mark(0U);
}
