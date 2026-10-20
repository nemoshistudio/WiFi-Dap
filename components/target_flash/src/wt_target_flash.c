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

#include <esp_log.h>
#include <esp_timer.h>

#include <mbedtls/sha256.h>

#include "DAP_handle.h"

#include "tf_dap.h"
#include "tf_internal.h"
#include "tf_swd.h"
#include "wt_target_flash.h"

/* UART bridge pause/resume (transport-dev, optional at link time). */
extern esp_err_t wt_uart_bridge_pause(void) __attribute__((weak));
extern esp_err_t wt_uart_bridge_resume(void) __attribute__((weak));

#define TF_PROG_BUF_SIZE     1024U
#define TF_READBACK_CHUNK    480U

static SemaphoreHandle_t s_api_lock;
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

/* ------------------------------------------------------------------------- */
/* init / locks                                                              */
/* ------------------------------------------------------------------------- */

esp_err_t wt_tf_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_api_lock = xSemaphoreCreateMutex();
    s_status_lock = xSemaphoreCreateMutex();
    if (s_api_lock == NULL || s_status_lock == NULL) {
        return ESP_FAIL;
    }
    s_inited = true;
    ESP_LOGI(TF_TAG, "target flash engine initialized");
    return ESP_OK;
}

static esp_err_t tf_ensure_init(void)
{
    return s_inited ? ESP_OK : wt_tf_init();
}

/** Take the API mutex without blocking (concurrent call -> INVALID_STATE). */
static esp_err_t tf_api_enter(void)
{
    if (!s_inited) {
        esp_err_t err = wt_tf_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (xSemaphoreTake(s_api_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void tf_api_exit(void)
{
    xSemaphoreGive(s_api_lock);
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
    memset(&s_target, 0, sizeof(s_target));
    s_target.family = TF_FAMILY_UNKNOWN;
    snprintf(s_target.family_str, sizeof(s_target.family_str), "unknown");
}

static void tf_info_fill_from_target(void)
{
    memset(&s_info, 0, sizeof(s_info));
    s_info.detected = true;
    snprintf(s_info.family, sizeof(s_info.family), "%s", s_target.family_str);
    s_info.dpidr = s_target.dpidr;
    s_info.cpuid = s_target.cpuid;
    s_info.idcode = s_target.idcode;
    memcpy(s_info.uid, s_target.uid, sizeof(s_info.uid));
    s_info.flash_base = s_target.flash_base;
    s_info.flash_size = s_target.flash_size;
    s_info.max_image_size = s_target.max_image_size;
}

/**
 * Link the DAP to SWD and identify the target; fills s_target/s_info.
 * The caller must already own the DAP engine.
 */
static esp_err_t tf_probe_link(void)
{
    uint32_t dpidr = 0U;
    uint32_t cpuid = 0U;
    uint32_t partno;
    const tf_family_ops_t *ops;
    esp_err_t err;

    tf_info_clear();

    err = tf_swd_link_init(&dpidr);
    if (err != ESP_OK) {
        snprintf(s_info.family, sizeof(s_info.family), "unknown");
        return (err == ESP_ERR_TIMEOUT) ? err : ESP_ERR_NOT_FOUND;
    }
    s_target.dpidr = dpidr;

    err = tf_swd_read_u32(TF_REG_CPUID, &cpuid);
    if (err != ESP_OK) {
        snprintf(s_info.family, sizeof(s_info.family), "unknown");
        return ESP_ERR_NOT_FOUND;
    }
    s_target.cpuid = cpuid;
    partno = (cpuid >> 4) & 0xFFFU;
    if (partno == 0xC24U) {              /* Cortex-M4 */
        ops = &tf_family_stm32f4_ops;
    } else if (partno == 0xC27U) {       /* Cortex-M7 */
        ops = &tf_family_stm32h7_ops;
    } else {
        ESP_LOGW(TF_TAG, "unsupported CPUID 0x%08X (part 0x%03X)", (unsigned)cpuid,
                 (unsigned)partno);
        return ESP_ERR_NOT_FOUND;
    }
    s_target.ops = ops;
    s_target.family = (ops == &tf_family_stm32f4_ops) ? TF_FAMILY_STM32F4
                                                      : TF_FAMILY_STM32H7;

    err = ops->probe(&s_target);
    if (err != ESP_OK) {
        snprintf(s_info.family, sizeof(s_info.family), "unknown");
        return (err == ESP_ERR_NOT_SUPPORTED) ? ESP_ERR_NOT_FOUND : err;
    }
    tf_info_fill_from_target();
    return ESP_OK;
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
    return (s_target.family == TF_FAMILY_STM32H7) ? 32U : 8U;
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
/* public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t wt_tf_probe(wt_tf_info_t *info)
{
    esp_err_t err;
    esp_err_t ret;

    ret = tf_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        snprintf(info->family, sizeof(info->family), "unknown");
    }

    ret = tf_api_enter();
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_session_active) {
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }
    tf_set_state(WT_TF_IDLE);
    err = tf_dap_acquire();
    if (err == ESP_OK) {
        err = tf_probe_link();
        tf_dap_release();
    }
    if (err == ESP_OK) {
        if (info != NULL) {
            *info = s_info;
        }
        tf_set_message("probe ok: %s", s_info.family);
        s_debug_result.info = s_info;
    } else {
        snprintf(s_info.family, sizeof(s_info.family), "unknown");
        tf_set_message("probe failed: %s", esp_err_to_name(err));
    }
    tf_api_exit();
    return err;
}

esp_err_t wt_tf_flash_begin(uint32_t image_size)
{
    esp_err_t err;

    if (image_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    err = tf_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    err = tf_api_enter();
    if (err != ESP_OK) {
        return err;
    }
    if (s_session_active) {
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }

    tf_status_lock();
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WT_TF_ERASING;
    s_status.total = image_size;
    snprintf(s_status.message, sizeof(s_status.message), "erasing");
    tf_status_unlock();
    s_abort_requested = false;
    s_image_size = image_size;
    s_received = 0U;
    s_programmed = 0U;
    s_buf_len = 0U;
    s_core_halted = false;
    s_session_active = true;
    s_op_start_ms = tf_now_ms();

    ESP_LOGI(TF_TAG, "flash begin: image %u bytes", (unsigned)image_size);

    err = tf_dap_acquire();
    if (err != ESP_OK) {
        s_session_active = false;
        tf_set_error(err, "DAP hand-off failed");
        tf_api_exit();
        return err;
    }
    err = tf_probe_link();
    if (err != ESP_OK) {
        tf_session_fail(err, "target probe failed");
        tf_api_exit();
        return err;
    }
    if (image_size > s_target.max_image_size) {
        tf_session_fail(ESP_ERR_INVALID_SIZE, "image larger than target flash");
        tf_api_exit();
        return ESP_ERR_INVALID_SIZE;
    }
    err = tf_swd_halt();
    if (err != ESP_OK) {
        tf_session_fail(err, "target halt failed");
        tf_api_exit();
        return err;
    }
    s_core_halted = true;

    err = s_target.ops->program_begin(&s_target);
    if (err != ESP_OK) {
        tf_session_fail(err, "flash unlock failed");
        tf_api_exit();
        return err;
    }
    err = s_target.ops->erase(&s_target, s_target.flash_base, image_size);
    if (err != ESP_OK) {
        tf_session_fail(err, "flash erase failed");
        tf_api_exit();
        return err;
    }
    if (tf_abort_check()) {
        tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
        tf_api_exit();
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

    tf_api_exit();
    ESP_LOGI(TF_TAG, "flash begin done: %s, %u KiB erased coverage, DAP held",
             s_target.family_str, (unsigned)((image_size + 1023U) / 1024U));
    return ESP_OK;
}

esp_err_t wt_tf_flash_feed(const void *data, size_t len)
{
    const uint8_t *src = (const uint8_t *)data;

    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tf_api_enter() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_session_active || s_status.state != WT_TF_PROGRAMMING) {
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint64_t)s_received + len > (uint64_t)s_image_size) {
        tf_api_exit();
        return ESP_ERR_INVALID_SIZE;
    }
    if (tf_abort_check()) {
        tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
        tf_api_exit();
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
                tf_api_exit();
                return err;
            }
        }
    }
    tf_api_exit();
    return ESP_OK;
}

esp_err_t wt_tf_flash_end(void)
{
    esp_err_t err = ESP_OK;
    size_t unit;
    uint32_t offset;
    mbedtls_sha256_context readback;
    uint8_t chunk[TF_READBACK_CHUNK];
    uint32_t verified = 0U;
    bool sha_match;

    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tf_api_enter() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_session_active || s_status.state != WT_TF_PROGRAMMING) {
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_received != s_image_size) {
        tf_session_fail(ESP_ERR_INVALID_SIZE, "image size mismatch (feed incomplete)");
        tf_api_exit();
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
            tf_api_exit();
            return err;
        }
        s_buf_len = 0U;
    }
    tf_update_progress(s_image_size, 0U);

    if (tf_abort_check()) {
        tf_session_fail(ESP_ERR_INVALID_STATE, "aborted");
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }

    /* Finalize the source digest. */
    if (mbedtls_sha256_finish(&s_sha, s_sha_expected) != 0) {
        tf_session_fail(ESP_FAIL, "sha256 finalize failed");
        tf_api_exit();
        return ESP_FAIL;
    }
    s_sha_active = false;

    /* Lock the flash before verification/reset. */
    err = s_target.ops->program_end(&s_target);
    if (err != ESP_OK) {
        tf_session_fail(err, "flash lock failed");
        tf_api_exit();
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
            tf_api_exit();
            return ESP_ERR_INVALID_STATE;
        }
        err = tf_swd_read_mem(s_target.flash_base + offset, chunk, chunk_len);
        if (err != ESP_OK) {
            mbedtls_sha256_free(&readback);
            tf_session_fail(err, "flash read-back failed");
            tf_api_exit();
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
        tf_api_exit();
        return ESP_FAIL;
    }
    mbedtls_sha256_free(&readback);

    sha_match = (memcmp(s_sha_expected, s_sha_actual, sizeof(s_sha_expected)) == 0);
    if (!sha_match) {
        tf_session_fail(ESP_ERR_INVALID_CRC, "verify sha256 mismatch");
        tf_api_exit();
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
        tf_api_exit();
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

    tf_api_exit();
    return ESP_OK;
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
    esp_err_t err;

    err = tf_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    err = tf_api_enter();
    if (err != ESP_OK) {
        return err;
    }
    if (s_session_active) {
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }
    tf_set_state(WT_TF_RESETTING);
    err = tf_dap_acquire();
    if (err == ESP_OK) {
        err = tf_probe_link();
        if (err == ESP_OK) {
            err = tf_swd_sysreset(run);
        }
        tf_dap_release();
    }
    if (err == ESP_OK) {
        tf_set_state(WT_TF_IDLE);
        tf_set_message("target reset (%s)", run ? "run" : "halt");
    } else {
        tf_set_error(err, "target reset failed");
    }
    tf_api_exit();
    return err;
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

esp_err_t tf_debug_read_pc(uint32_t *pc, bool *halted, bool *lockup)
{
    esp_err_t err;
    uint32_t dhcsr = 0U;
    bool was_halted = false;

    if (pc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (halted != NULL) {
        *halted = false;
    }
    if (lockup != NULL) {
        *lockup = false;
    }
    err = tf_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    err = tf_api_enter();
    if (err != ESP_OK) {
        return err;
    }
    if (s_session_active) {
        tf_api_exit();
        return ESP_ERR_INVALID_STATE;
    }
    err = tf_dap_acquire();
    if (err == ESP_OK) {
        err = tf_probe_link();
        if (err == ESP_OK) {
            err = tf_swd_read_u32(TF_REG_DHCSR, &dhcsr);
            if (err == ESP_OK) {
                was_halted = (dhcsr & TF_DHCSR_S_HALT) != 0U;
                if (halted != NULL) {
                    *halted = was_halted;
                }
                if (lockup != NULL) {
                    *lockup = (dhcsr & TF_DHCSR_S_LOCKUP) != 0U;
                }
                err = tf_swd_read_pc(pc);
            }
            if (!was_halted) {
                (void)tf_swd_resume();
            }
        }
        tf_dap_release();
    }
    tf_api_exit();
    return err;
}
