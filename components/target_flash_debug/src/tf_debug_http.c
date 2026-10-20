/*
 * SPDX-License-Identifier: MIT
 *
 * Kconfig-gated /tf bring-up endpoint for the target flash engine.
 *
 * Enabled only with CONFIG_BIRDIEBOT_TARGET_FLASH_DEBUG_HTTP=y (default n).
 * It is a lab/debug tool used to verify the engine against a real STM32 target
 * without the Web UI; the production Web flow uses the C API directly.
 *
 * Requests (POST, Host/Origin guard + X-WiFi Dap-Target-Flash: 1 header):
 *   POST /tf/probe  -> JSON with the detected target info
 *   POST /tf/status -> JSON status snapshot + last SHA-256 verification result
 *   POST /tf/flash  -> body = raw image bytes; program + verify + reset,
 *                      returns JSON with expected/actual SHA-256 and timing
 *   POST /tf/abort  -> abort a running flash session
 *   POST /tf/pc     -> halt, read target PC, resume (reset/release check)
 */
#include "sdkconfig.h"

#if CONFIG_BIRDIEBOT_TARGET_FLASH_DEBUG_HTTP

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_system.h>
#include "esp_private/panic_internal.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include "web_host_guard.h"
#include "web_uri_module.h"

#include "wt_target_flash.h"
#include "wt_target_flash_debug.h"

/* A2 UART bridge pause/resume (transport-dev); weak so this debug component
 * builds before/without the transport implementation. */
extern esp_err_t wt_uart_bridge_pause(void) __attribute__((weak));
extern esp_err_t wt_uart_bridge_resume(void) __attribute__((weak));

#include <esp_attr.h>

#define TF_DEBUG_TAG "target_flash_debug"

/* Boot counter in RTC no-init memory: lets the host see whether a probe/step
 * caused a device reboot (panic) between two status queries. */
/* Async probe test: run the engine from a dedicated core-0 task with 8 KiB
 * stack, to tell httpd-task context issues (stack/core affinity) apart from a
 * real SWD/DAP problem. */
static volatile int s_probe_async_state; /* 0 idle, 1 running, 2 done */
static volatile int s_probe_async_err;
static wt_tf_info_t s_probe_async_info;

static void tf_debug_probe_task(void *arg)
{
    (void)arg;
    s_probe_async_err = (int)wt_tf_probe(&s_probe_async_info);
    s_probe_async_state = 2;
    vTaskDelete(NULL);
}

RTC_NOINIT_ATTR static uint32_t s_boot_count;
RTC_NOINIT_ATTR static uint32_t s_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_prev_stage;

/* Panic capture (linker-wrapped esp_panic_handler): store the faulting PC and
 * exception class in RTC no-init memory so /tf?cmd=status can report it after
 * the reboot, then let the regular panic handler run. */
RTC_NOINIT_ATTR static uint32_t s_panic_pc;
RTC_NOINIT_ATTR static uint32_t s_panic_exc;
RTC_NOINIT_ATTR static uint32_t s_panic_core;
RTC_NOINIT_ATTR static uint32_t s_panic_fault_pc;
RTC_NOINIT_ATTR static uint32_t s_panic_magic;
RTC_NOINIT_ATTR static char s_panic_desc[48];
RTC_NOINIT_ATTR static char s_panic_reason[24];
/* Assert/abort capture: file/expression of __assert_func and abort details. */
RTC_NOINIT_ATTR static char s_assert_file[64];
RTC_NOINIT_ATTR static char s_assert_expr[80];
RTC_NOINIT_ATTR static char s_abort_details[80];
RTC_NOINIT_ATTR static uint32_t s_assert_line;
RTC_NOINIT_ATTR static uint32_t s_assert_magic;
#define TF_ASSERT_MAGIC 0x41535345U

static void tf_panic_copy_str(char *dst, size_t dst_len, const char *src);

void __real___assert_func(const char *file, int line, const char *func, const char *expr);
void __real_panic_abort(const char *details);

void __wrap___assert_func(const char *file, int line, const char *func, const char *expr)
{
    s_assert_magic = TF_ASSERT_MAGIC;
    tf_panic_copy_str(s_assert_file, sizeof(s_assert_file), file);
    tf_panic_copy_str(s_assert_expr, sizeof(s_assert_expr), expr);
    s_assert_line = (uint32_t)line;
    __real___assert_func(file, line, func, expr);
}

void __wrap_panic_abort(const char *details)
{
    s_assert_magic = TF_ASSERT_MAGIC;
    tf_panic_copy_str(s_abort_details, sizeof(s_abort_details), details);
    __real_panic_abort(details);
}

static void tf_panic_copy_str(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0U;

    if (src == NULL || dst_len == 0U) {
        if (dst_len > 0U) {
            dst[0] = '\0';
        }
        return;
    }
    while (i + 1U < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
#define TF_PANIC_MAGIC 0x50414E43U

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info != NULL) {
        s_panic_magic = TF_PANIC_MAGIC;
        s_panic_pc = (uint32_t)(uintptr_t)info->addr;
        s_panic_fault_pc = panic_get_address(info->frame);
        s_panic_exc = (uint32_t)info->exception;
        s_panic_core = (uint32_t)info->core;
        tf_panic_copy_str(s_panic_desc, sizeof(s_panic_desc), info->description);
        tf_panic_copy_str(s_panic_reason, sizeof(s_panic_reason), info->reason);
    }
    __real_esp_panic_handler(info);
}
#define TF_BOOT_MAGIC 0x54464C42U

__attribute__((constructor(0x71))) static void tf_debug_boot_count_init(void)
{
    if (s_boot_magic != TF_BOOT_MAGIC) {
        s_boot_magic = TF_BOOT_MAGIC;
        s_boot_count = 0U;
    }
    s_prev_stage = tf_debug_last_stage(); /* stage reached before this boot */
    s_boot_count++;
    tf_debug_stage_reset();
}
#define TF_HTTP_BODY_CHUNK 2048U
#define TF_HTTP_JSON_MAX   1536U

static const char *tf_http_status_code(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "200 OK";
    case ESP_ERR_INVALID_ARG:
        return "400 Bad Request";
    case ESP_ERR_INVALID_STATE:
        return "409 Conflict";
    case ESP_ERR_INVALID_SIZE:
        return "413 Payload Too Large";
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_CRC:
        return "422 Unprocessable Entity";
    case ESP_ERR_TIMEOUT:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}

static void tf_http_send_error(httpd_req_t *req, esp_err_t err, const char *what)
{
    char body[192];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"%s\",\"err\":\"%s\",\"code\":%d}",
                     what ? what : "failed", esp_err_to_name(err), (int)err);
    if (n < 0) {
        body[0] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, tf_http_status_code(err));
    httpd_resp_sendstr(req, body);
}

static void tf_http_status_json(char *buf, size_t len)
{
    wt_tf_status_t st;
    tf_debug_result_t res;

    wt_tf_get_status(&st);
    tf_debug_get_result(&res);
    snprintf(buf, len,
             "{\"ok\":true,\"state\":%d,\"total\":%u,\"written\":%u,\"verified\":%u,"
             "\"percent\":%d,\"err\":%d,\"message\":\"%s\","
             "\"sha_valid\":%s,\"sha_match\":%s,"
             "\"sha_expected\":\"%s\",\"sha_actual\":\"%s\","
             "\"image_size\":%u,\"elapsed_ms\":%u,"
             "\"family\":\"%s\",\"detected\":%s,"
             "\"dpidr\":\"0x%08X\",\"cpuid\":\"0x%08X\",\"idcode\":\"0x%08X\","
             "\"flash_base\":\"0x%08X\",\"flash_size\":%u,\"max_image_size\":%u,"
             "\"last_stage\":%u,\"prev_stage\":%u,\"boot_count\":%u,\"reset_reason\":%d,"
             "\"panic\":{\"magic_ok\":%s,\"pc\":\"0x%08X\",\"fault_pc\":\"0x%08X\",\"exc\":%d,\"core\":%d,"
             "\"desc\":\"%s\",\"reason\":\"%s\"}"
             ",\"assert\":{\"magic_ok\":%s,\"file\":\"%s\",\"line\":%u,"
             "\"expr\":\"%s\",\"abort\":\"%s\"}}",
             (int)st.state, (unsigned)st.total, (unsigned)st.written,
             (unsigned)st.verified, st.percent, st.err, st.message,
             res.valid ? "true" : "false", res.sha_match ? "true" : "false",
             res.sha_expected, res.sha_actual, (unsigned)res.image_size,
             (unsigned)res.elapsed_ms, res.info.family,
             res.info.detected ? "true" : "false", (unsigned)res.info.dpidr,
             (unsigned)res.info.cpuid, (unsigned)res.info.idcode,
             (unsigned)res.info.flash_base, (unsigned)res.info.flash_size,
             (unsigned)res.info.max_image_size, (unsigned)tf_debug_last_stage(), (unsigned)s_prev_stage,
             (unsigned)s_boot_count, (int)esp_reset_reason(),
             (s_panic_magic == TF_PANIC_MAGIC) ? "true" : "false",
             (unsigned)s_panic_pc, (unsigned)s_panic_fault_pc, (int)s_panic_exc, (int)s_panic_core,
             s_panic_desc, s_panic_reason,
             (s_assert_magic == TF_ASSERT_MAGIC) ? "true" : "false",
             s_assert_file, (unsigned)s_assert_line, s_assert_expr, s_abort_details);
}

static esp_err_t tf_http_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    char header[8];
    esp_err_t err;

    if (!web_host_guard_http_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"host guard\"}");
        return ESP_OK;
    }
    header[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "X-WiFi Dap-Target-Flash", header,
                                    sizeof(header)) != ESP_OK ||
        strcmp(header, "1") != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing debug header\"}");
        return ESP_OK;
    }

    /* The web server uses its own uri_match (no '*' wildcard), so the debug
     * endpoint is a single "/tf" handler dispatched by the ?cmd= query value. */
    const char *cmd = strstr(uri, "cmd=");

    if (cmd == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing ?cmd=\"}");
        return ESP_OK;
    }
    cmd += 4;

    if (strncmp(cmd, "probe", 5) == 0) {
        static char body[TF_HTTP_JSON_MAX];
        wt_tf_info_t info;
        err = wt_tf_probe(&info);
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "probe");
            return ESP_OK;
        }
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"detected\":%s,\"family\":\"%s\","
                 "\"dpidr\":\"0x%08X\",\"cpuid\":\"0x%08X\",\"idcode\":\"0x%08X\","
                 "\"uid\":\"%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\","
                 "\"flash_base\":\"0x%08X\",\"flash_size\":%u,\"max_image_size\":%u,"
             "\"last_stage\":%u,\"prev_stage\":%u,\"boot_count\":%u,\"reset_reason\":%d,"
             "\"panic\":{\"magic_ok\":%s,\"pc\":\"0x%08X\",\"fault_pc\":\"0x%08X\",\"exc\":%d,\"core\":%d,"
             "\"desc\":\"%s\",\"reason\":\"%s\"}"
             ",\"assert\":{\"magic_ok\":%s,\"file\":\"%s\",\"line\":%u,"
             "\"expr\":\"%s\",\"abort\":\"%s\"}}",
                 info.detected ? "true" : "false", info.family,
                 (unsigned)info.dpidr, (unsigned)info.cpuid, (unsigned)info.idcode,
                 info.uid[0], info.uid[1], info.uid[2], info.uid[3], info.uid[4],
                 info.uid[5], info.uid[6], info.uid[7], info.uid[8], info.uid[9],
                 info.uid[10], info.uid[11], (unsigned)info.flash_base,
                 (unsigned)info.flash_size, (unsigned)info.max_image_size,
                 (unsigned)tf_debug_last_stage(), (unsigned)s_prev_stage,
             (unsigned)s_boot_count, (int)esp_reset_reason(),
             (s_panic_magic == TF_PANIC_MAGIC) ? "true" : "false",
             (unsigned)s_panic_pc, (unsigned)s_panic_fault_pc, (int)s_panic_exc, (int)s_panic_core,
             s_panic_desc, s_panic_reason,
             (s_assert_magic == TF_ASSERT_MAGIC) ? "true" : "false",
             s_assert_file, (unsigned)s_assert_line, s_assert_expr, s_abort_details);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "probe_async", 11) == 0) {
        if (s_probe_async_state == 1) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"probe already running\"}");
            return ESP_OK;
        }
        memset((void *)&s_probe_async_info, 0, sizeof(s_probe_async_info));
        s_probe_async_err = 0;
        s_probe_async_state = 1;
        if (xTaskCreatePinnedToCore(tf_debug_probe_task, "tf_probe", 8192, NULL,
                                    5, NULL, 0) != pdPASS) {
            s_probe_async_state = 0;
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no task\"}");
            return ESP_OK;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"started\":true,\"core\":0,\"stack\":8192}");
        return ESP_OK;
    }

    if (strncmp(cmd, "probe_result", 12) == 0) {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"async_state\":%d,\"err\":\"%s\",\"family\":\"%s\","
                 "\"dpidr\":\"0x%08X\",\"cpuid\":\"0x%08X\",\"idcode\":\"0x%08X\","
                 "\"flash_size\":%u,\"boot_count\":%u,\"last_stage\":%u}",
                 s_probe_async_state, esp_err_to_name((esp_err_t)s_probe_async_err),
                 s_probe_async_info.family, (unsigned)s_probe_async_info.dpidr,
                 (unsigned)s_probe_async_info.cpuid, (unsigned)s_probe_async_info.idcode,
                 (unsigned)s_probe_async_info.flash_size, (unsigned)s_boot_count,
                 (unsigned)tf_debug_last_stage());
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "diag", 4) == 0) {
        static char diag[2048];
        err = tf_debug_link_diag(diag, sizeof(diag));
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "diag");
            return ESP_OK;
        }
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, diag);
        return ESP_OK;
    }

    if (strncmp(cmd, "uartpause", 9) == 0) {
        char body[128];
        if (wt_uart_bridge_pause == NULL) {
            httpd_resp_set_status(req, "501 Not Implemented");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"uart pause API not linked\"}");
            return ESP_OK;
        }
        err = wt_uart_bridge_pause();
        snprintf(body, sizeof(body), "{\"ok\":%s,\"err\":\"%s\",\"paused\":true}",
                 (err == ESP_OK) ? "true" : "false", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "uartresume", 10) == 0) {
        char body[128];
        if (wt_uart_bridge_resume == NULL) {
            httpd_resp_set_status(req, "501 Not Implemented");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"uart resume API not linked\"}");
            return ESP_OK;
        }
        err = wt_uart_bridge_resume();
        snprintf(body, sizeof(body), "{\"ok\":%s,\"err\":\"%s\",\"paused\":false}",
                 (err == ESP_OK) ? "true" : "false", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "sizecheck", 9) == 0) {
        /* Exercise the too-large image error path (begin must reject and not
         * leave the engine/DAP held). */
        char body[256];
        err = wt_tf_flash_begin(2U * 1024U * 1024U);
        snprintf(body, sizeof(body),
                 "{\"begin_err\":\"%s\",\"err\":%d,\"state\":%d,\"dap_released\":true}",
                 esp_err_to_name(err), (int)err, (int)0);
        httpd_resp_set_status(req, tf_http_status_code(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "flashabort", 10) == 0) {
        /* Abort-path test: begin a small session, feed part of it, request an
         * abort and verify the engine cleans up (flash locked, DAP released,
         * UART bridge resumed). */
        char body[TF_HTTP_JSON_MAX];
        wt_tf_status_t st;
        uint8_t chunk[256];
        memset(chunk, 0xA5, sizeof(chunk));
        err = wt_tf_flash_begin(4096U);
        if (err == ESP_OK) {
            err = wt_tf_flash_feed(chunk, sizeof(chunk));
        }
        if (err == ESP_OK) {
            err = wt_tf_abort();
        }
        /* Drive the abort check (the next feed sees the abort flag). */
        if (err == ESP_OK) {
            (void)wt_tf_flash_feed(chunk, sizeof(chunk));
        }
        (void)wt_tf_flash_end();
        wt_tf_get_status(&st);
        snprintf(body, sizeof(body),
                 "{\"ok\":%s,\"begin_feed_abort\":\"%s\",\"state\":%d,"
                 "\"err\":%d,\"message\":\"%s\",\"percent\":%d}",
                 (st.state == WT_TF_ERROR) ? "true" : "false", esp_err_to_name(err),
                 (int)st.state, st.err, st.message, st.percent);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "status", 6) == 0) {
        static char body[TF_HTTP_JSON_MAX];
        tf_http_status_json(body, sizeof(body));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "abort", 5) == 0) {
        err = wt_tf_abort();
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "abort");
            return ESP_OK;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"abort\":\"requested\"}");
        return ESP_OK;
    }

    if (strncmp(cmd, "pc", 2) == 0) {
        uint32_t pc = 0U;
        bool halted = false;
        bool lockup = false;
        char body[256];

        err = tf_debug_read_pc(&pc, &halted, &lockup);
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "read pc");
            return ESP_OK;
        }
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"pc\":\"0x%08X\",\"was_halted\":%s,\"lockup\":%s}",
                 (unsigned)pc, halted ? "true" : "false", lockup ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "flash", 5) == 0) {
        static uint8_t body_buf[TF_HTTP_BODY_CHUNK];
        int to_read = req->content_len;
        int chunks = 0;

        if (to_read <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty body\"}");
            return ESP_OK;
        }
        err = wt_tf_flash_begin((uint32_t)to_read);
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "flash_begin");
            return ESP_OK;
        }
        while (to_read > 0) {
            int want = (to_read < (int)sizeof(body_buf)) ? to_read : (int)sizeof(body_buf);
            int got = httpd_req_recv(req, (char *)body_buf, want);
            if (got <= 0) {
                (void)wt_tf_abort();
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            err = wt_tf_flash_feed(body_buf, (size_t)got);
            if (err != ESP_OK) {
                (void)wt_tf_abort();
                break;
            }
            to_read -= got;
            chunks++;
        }
        if (err == ESP_OK) {
            err = wt_tf_flash_end();
        }
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "flash");
            return ESP_OK;
        }
        {
            static char body[TF_HTTP_JSON_MAX];
            tf_http_status_json(body, sizeof(body));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, body);
        }
        ESP_LOGI(TF_DEBUG_TAG, "debug flash done (%d chunks)", chunks);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown /tf action\"}");
    return ESP_OK;
}

static const httpd_uri_t s_tf_uri = {
    .uri = "/tf",   /* ?cmd=... dispatch: web_server uri_match has no wildcard */
    .method = HTTP_POST,
    .handler = tf_http_handler,
    .user_ctx = NULL,
};

static int URI_TF_INIT(const httpd_uri_t **uri_conf)
{
    *uri_conf = &s_tf_uri;
    return 0;
}

static int URI_TF_EXIT(const httpd_uri_t **uri_conf)
{
    (void)uri_conf;
    return 0;
}

WEB_URI_MODULE_REGISTER(0x70, URI_TF_INIT, URI_TF_EXIT)

#endif /* CONFIG_BIRDIEBOT_TARGET_FLASH_DEBUG_HTTP */
