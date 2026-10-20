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
#include <stdio.h>
#include <string.h>

#include <esp_http_server.h>
#include <esp_log.h>

#include "web_host_guard.h"
#include "web_uri_module.h"

#include "wt_target_flash.h"
#include "wt_target_flash_debug.h"

#define TF_DEBUG_TAG "target_flash_debug"
#define TF_HTTP_BODY_CHUNK 2048U
#define TF_HTTP_JSON_MAX   1024U

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
             "\"flash_base\":\"0x%08X\",\"flash_size\":%u,\"max_image_size\":%u}",
             (int)st.state, (unsigned)st.total, (unsigned)st.written,
             (unsigned)st.verified, st.percent, st.err, st.message,
             res.valid ? "true" : "false", res.sha_match ? "true" : "false",
             res.sha_expected, res.sha_actual, (unsigned)res.image_size,
             (unsigned)res.elapsed_ms, res.info.family,
             res.info.detected ? "true" : "false", (unsigned)res.info.dpidr,
             (unsigned)res.info.cpuid, (unsigned)res.info.idcode,
             (unsigned)res.info.flash_base, (unsigned)res.info.flash_size,
             (unsigned)res.info.max_image_size);
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
        wt_tf_info_t info;
        char body[TF_HTTP_JSON_MAX];
        err = wt_tf_probe(&info);
        if (err != ESP_OK) {
            tf_http_send_error(req, err, "probe");
            return ESP_OK;
        }
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"detected\":%s,\"family\":\"%s\","
                 "\"dpidr\":\"0x%08X\",\"cpuid\":\"0x%08X\",\"idcode\":\"0x%08X\","
                 "\"uid\":\"%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\","
                 "\"flash_base\":\"0x%08X\",\"flash_size\":%u,\"max_image_size\":%u}",
                 info.detected ? "true" : "false", info.family,
                 (unsigned)info.dpidr, (unsigned)info.cpuid, (unsigned)info.idcode,
                 info.uid[0], info.uid[1], info.uid[2], info.uid[3], info.uid[4],
                 info.uid[5], info.uid[6], info.uid[7], info.uid[8], info.uid[9],
                 info.uid[10], info.uid[11], (unsigned)info.flash_base,
                 (unsigned)info.flash_size, (unsigned)info.max_image_size);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    if (strncmp(cmd, "status", 6) == 0) {
        char body[TF_HTTP_JSON_MAX];
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
            char body[TF_HTTP_JSON_MAX];
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
