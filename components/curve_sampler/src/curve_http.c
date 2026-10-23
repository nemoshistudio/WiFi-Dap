/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - HTTP endpoints.
 *
 * Registered as two independent URI modules through the web_server
 * WEB_URI_MODULE_REGISTER constructor mechanism (the same pattern as
 * components/target_flash_debug):
 *
 *   POST /api/curve/selftest   Phase 0 SWD timing probe
 *   POST /api/curve/start      start local sampling (<= 16 KiB JSON body)
 *   POST /api/curve/stop       stop sampling, target resumed, DAP released
 *   GET  /api/curve/status     sampler status JSON
 *   GET  /api/curve/frames     CVF1 binary frame batch (application/octet-stream)
 *
 * The web_server has a single URI handler slot per module and its custom
 * uri_match() supports prefix matching for a reference URI ending in '/', so
 * one POST module + one GET module on "/api/curve/" dispatch by req->uri.  This
 * keeps the generic /api handler and its 2 KiB memory pool untouched.
 *
 * The httpd task only parses configuration and copies frames: the SWD/DAP work
 * is done by the sampler worker task (curve_engine.c).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "web_host_guard.h"
#include "web_uri_module.h"

#include "curve_priv.h"

#define CURVE_URI_PREFIX  "/api/curve/"
#define CURVE_URI_SELFTEST CURVE_URI_PREFIX "selftest"
#define CURVE_URI_START    CURVE_URI_PREFIX "start"
#define CURVE_URI_STOP     CURVE_URI_PREFIX "stop"
#define CURVE_URI_STATUS   CURVE_URI_PREFIX "status"
#define CURVE_URI_FRAMES   CURVE_URI_PREFIX "frames"

#define CURVE_DEFAULT_MAX_FRAMES 32U

/* The ESP httpd task has a single instance and a 4 KiB stack: request bodies
 * and response JSON use static buffers (same pattern as tf_debug_http.c). */
/* Request body buffer (kept in .bss: the ESP32-S3 heap's largest free block
 * after WiFi is smaller than this buffer).  The 512-series compact v3 start
 * body is ~17 KiB, hence the 20 KiB cap. */
static char s_body_storage[CURVE_JSON_BODY_MAX + 1U];
static char *s_body = s_body_storage;

static char *curve_body_acquire(void)
{
    return s_body;
}

static void curve_body_release(void)
{
}
static char s_json[1280];
static curve_config_t s_http_cfg;

/* ------------------------------------------------------------------------- */
/* response helpers                                                          */
/* ------------------------------------------------------------------------- */

static const char *curve_http_status(esp_err_t err)
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
    case ESP_ERR_TIMEOUT:
        return "503 Service Unavailable";
    case ESP_ERR_NOT_FOUND:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}

static esp_err_t curve_send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t curve_send_error(httpd_req_t *req, const char *status,
                                  const char *fmt, ...)
{
    va_list ap;
    char body[192];

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    return curve_send_json(req, status, body);
}

static esp_err_t curve_send_error_ok(httpd_req_t *req, esp_err_t err, const char *what)
{
    char body[192];

    (void)snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s: %s\"}",
                   what != NULL ? what : "failed", esp_err_to_name(err));
    return curve_send_json(req, curve_http_status(err), body);
}

/* ------------------------------------------------------------------------- */
/* request helpers                                                           */
/* ------------------------------------------------------------------------- */

static bool curve_path_is(const char *uri, const char *path)
{
    size_t len = strlen(path);

    if (uri == NULL || strncmp(uri, path, len) != 0) {
        return false;
    }
    return uri[len] == '\0' || uri[len] == '?';
}

static bool curve_query_u32(const char *uri, const char *key, uint32_t *out)
{
    const char *query;
    size_t key_len = strlen(key);

    if (uri == NULL || out == NULL) {
        return false;
    }
    query = strchr(uri, '?');
    if (query == NULL) {
        return false;
    }
    query++;
    while (*query != '\0') {
        const char *amp = strchr(query, '&');
        size_t seg_len = (amp != NULL) ? (size_t)(amp - query) : strlen(query);

        if (seg_len > key_len && strncmp(query, key, key_len) == 0 &&
            query[key_len] == '=') {
            char value[12];
            size_t value_len = seg_len - key_len - 1U;
            char *end = NULL;
            unsigned long parsed;

            if (value_len == 0U || value_len >= sizeof(value)) {
                return false;
            }
            memcpy(value, query + key_len + 1U, value_len);
            value[value_len] = '\0';
            parsed = strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > 0xFFFFFFFFUL) {
                return false;
            }
            *out = (uint32_t)parsed;
            return true;
        }
        if (amp == NULL) {
            break;
        }
        query = amp + 1;
    }
    return false;
}

/** Read the whole request body (bounded by CURVE_JSON_BODY_MAX). */
static esp_err_t curve_read_body(httpd_req_t *req, size_t *out_len)
{
    size_t content_len = req->content_len;
    size_t received = 0U;
    int timeouts = 0;

    if (out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    if (s_body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (content_len == 0U) {
        s_body[0] = '\0';
        return ESP_OK;
    }
    if (content_len > CURVE_JSON_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (received < content_len) {
        int got = httpd_req_recv(req, s_body + received, content_len - received);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return ESP_FAIL;
            }
            continue;
        }
        if (got <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)got;
    }
    s_body[received] = '\0';
    *out_len = received;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* handlers                                                                  */
/* ------------------------------------------------------------------------- */

static esp_err_t curve_handle_selftest(httpd_req_t *req)
{
    curve_selftest_t st;
    esp_err_t err;
    char err_msg[64] = {0};

    err = curve_engine_selftest(&st, err_msg, sizeof(err_msg));
    if (err != ESP_OK) {
        return curve_send_error(req, curve_http_status(err),
                                "{\"ok\":false,\"err\":\"%s\"}",
                                err_msg[0] != '\0' ? err_msg : "selftest failed");
    }
    (void)snprintf(s_json, sizeof(s_json),
                   "{\"ok\":true,\"iterations\":%u,\"completed\":%u,\"failed\":%u,"
                   "\"min_us\":%u,\"median_us\":%u,\"p90_us\":%u,\"max_us\":%u,"
                   "\"link_hz\":%u,\"dpidr\":\"0x%08X\","
                   "\"halt_us\":%u,\"regs_us\":%u,\"mem_us\":%u,\"resume_us\":%u}",
                   (unsigned)st.iterations, (unsigned)st.completed,
                   (unsigned)st.failed, (unsigned)st.min_us,
                   (unsigned)st.median_us, (unsigned)st.p90_us,
                   (unsigned)st.max_us, (unsigned)st.link_hz,
                   (unsigned)st.dpidr, (unsigned)st.halt_us,
                   (unsigned)st.regs_us, (unsigned)st.mem_us,
                   (unsigned)st.resume_us);
    return curve_send_json(req, "200 OK", s_json);
}

static esp_err_t curve_handle_start(httpd_req_t *req)
{
    size_t body_len = 0U;
    esp_err_t err;
    char err_msg[64] = {0};
    curve_status_t status;

    if (curve_body_acquire() == NULL) {
        return curve_send_error(req, "500 Internal Server Error",
                                "{\"ok\":false,\"err\":\"no memory for body\"}");
    }
    err = curve_read_body(req, &body_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return curve_send_error(req, "413 Payload Too Large",
                                "{\"ok\":false,\"err\":\"JSON body > %u bytes\"}",
                                (unsigned)CURVE_JSON_BODY_MAX);
    }
    if (err != ESP_OK) {
        return curve_send_error_ok(req, err, "read body");
    }
    if (body_len == 0U) {
        return curve_send_error(req, "400 Bad Request",
                                "{\"ok\":false,\"err\":\"empty body\"}");
    }
    /* The engine keeps a const pointer to s_http_cfg while a session runs;
     * only (re)parse when idle. */
    if (!curve_engine_is_idle()) {
        return curve_send_error(req, "409 Conflict",
                                "{\"ok\":false,\"err\":\"busy\"}");
    }

    /* Free the previous session's ring before parsing: a large ring can eat
     * the heap headroom cJSON needs, especially while the browser keeps its
     * global WebSocket alive. */
    curve_engine_release_ring();
    err = curve_config_parse(s_body, body_len, &s_http_cfg, err_msg, sizeof(err_msg));
    curve_body_release();
    if (err != ESP_OK) {
        return curve_send_error(req, curve_http_status(err),
                                "{\"ok\":false,\"err\":\"%s\"}", err_msg);
    }

    err = curve_engine_start(&s_http_cfg, err_msg, sizeof(err_msg));
    if (err != ESP_OK) {
        return curve_send_error(req, curve_http_status(err),
                                "{\"ok\":false,\"err\":\"%s\"}",
                                err_msg[0] != '\0' ? err_msg : esp_err_to_name(err));
    }

    curve_engine_status(&status);
    (void)snprintf(s_json, sizeof(s_json),
                   "{\"ok\":true,\"group_count\":%u,\"series_count\":%u,"
                   "\"sample_bytes\":%u,\"interval_us\":%u,"
                   "\"trigger\":\"%s\",\"breakpoint_address\":%u,"
                   "\"halt\":%s,\"pc_capture\":%s,"
                   "\"watchpoint_address\":%u,\"watchpoint_size\":%u,"
                   "\"watchpoint_access\":\"%s\","
                   "\"link_hz\":%u,\"dpidr\":\"0x%08X\"}",
                   (unsigned)s_http_cfg.group_count,
                   (unsigned)s_http_cfg.series_count,
                   (unsigned)s_http_cfg.sample_bytes,
                   (unsigned)s_http_cfg.interval_us,
                   curve_trigger_name(s_http_cfg.trigger),
                   (unsigned)s_http_cfg.breakpoint_address,
                   s_http_cfg.halt ? "true" : "false",
                   status.pc_available ? "true" : "false",
                   (unsigned)s_http_cfg.watch_address,
                   (unsigned)s_http_cfg.watch_size,
                   curve_watch_access_name(s_http_cfg.watch_access),
                   (unsigned)status.link_hz,
                   (unsigned)status.dpidr);
    return curve_send_json(req, "200 OK", s_json);
}

static esp_err_t curve_handle_stop(httpd_req_t *req)
{
    char err_msg[64] = {0};
    esp_err_t err;

    /* Optional body, drained so keep-alive connections stay usable. */
    size_t body_len = 0U;
    (void)curve_read_body(req, &body_len);
    err = curve_engine_stop(CURVE_STOP_TIMEOUT_MS, err_msg, sizeof(err_msg));
    if (err != ESP_OK) {
        return curve_send_error(req, curve_http_status(err),
                                "{\"ok\":false,\"err\":\"%s\"}",
                                err_msg[0] != '\0' ? err_msg : esp_err_to_name(err));
    }
    return curve_send_json(req, "200 OK", "{\"ok\":true,\"running\":false}");
}

static esp_err_t curve_handle_status(httpd_req_t *req)
{
    curve_status_t status;

    curve_engine_status(&status);
    (void)curve_status_format_json(&status, s_json, sizeof(s_json));
    return curve_send_json(req, "200 OK", s_json);
}

static esp_err_t curve_handle_frames(httpd_req_t *req)
{
    uint32_t since = 0U;
    uint32_t max = CURVE_DEFAULT_MAX_FRAMES;
    bool no_data = false;
    uint8_t *buf = NULL;
    size_t len = 0U;
    esp_err_t err;

    if (!curve_query_u32(req->uri, "since", &since)) {
        since = 0U;
    }
    if (!curve_query_u32(req->uri, "max", &max) || max == 0U) {
        max = CURVE_DEFAULT_MAX_FRAMES;
    }
    if (max > CURVE_MAX_FRAMES_PER_REQ) {
        max = CURVE_MAX_FRAMES_PER_REQ;
    }

    err = curve_engine_alloc_frames(since, max, &buf, &len, &no_data);
    if (err != ESP_OK) {
        return curve_send_error_ok(req, err, "frames");
    }
    if (no_data) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)buf, (ssize_t)len);
    free(buf);
    return err;
}

/* ------------------------------------------------------------------------- */
/* URI dispatch                                                              */
/* ------------------------------------------------------------------------- */

static esp_err_t curve_post_handler_impl(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (uri == NULL) {
        return ESP_FAIL;
    }
    if (!web_host_guard_http_ok(req)) {
        return curve_send_error(req, "403 Forbidden",
                                "{\"ok\":false,\"err\":\"host guard\"}");
    }

    if (curve_path_is(uri, CURVE_URI_SELFTEST)) {
        /* Body is optional ("{}" or none); drain any content first. */
        size_t body_len = 0U;
        if (curve_body_acquire() == NULL) {
            return curve_send_error(req, "500 Internal Server Error",
                                    "{\"ok\":false,\"err\":\"no memory for body\"}");
        }
        esp_err_t err = curve_read_body(req, &body_len);
        curve_body_release();
        if (err == ESP_ERR_INVALID_SIZE) {
            return curve_send_error(req, "413 Payload Too Large",
                                    "{\"ok\":false,\"err\":\"JSON body > %u bytes\"}",
                                    (unsigned)CURVE_JSON_BODY_MAX);
        }
        if (err != ESP_OK) {
            return curve_send_error_ok(req, err, "read body");
        }
        return curve_handle_selftest(req);
    }
    if (curve_path_is(uri, CURVE_URI_START)) {
        return curve_handle_start(req);
    }
    if (curve_path_is(uri, CURVE_URI_STOP)) {
        return curve_handle_stop(req);
    }
    return curve_send_error(req, "404 Not Found",
                            "{\"ok\":false,\"err\":\"unknown /api/curve action\"}");
}

static esp_err_t curve_get_handler_impl(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (uri == NULL) {
        return ESP_FAIL;
    }
    if (!web_host_guard_http_ok(req)) {
        return curve_send_error(req, "403 Forbidden",
                                "{\"ok\":false,\"err\":\"host guard\"}");
    }
    if (curve_path_is(uri, CURVE_URI_STATUS)) {
        return curve_handle_status(req);
    }
    if (curve_path_is(uri, CURVE_URI_FRAMES)) {
        return curve_handle_frames(req);
    }
    return curve_send_error(req, "404 Not Found",
                            "{\"ok\":false,\"err\":\"unknown /api/curve action\"}");
}

/* Shared lazily allocated s_body / static s_http_cfg: serialize handlers.  Even though the
 * current web_server uses one httpd task, requests can arrive through more
 * than one server instance in future and the ESP HTTP stack may dispatch
 * handlers concurrently; an overwritten body buffer would otherwise surface as
 * a spurious "invalid JSON object" while a browser polls status/frames. */
static SemaphoreHandle_t s_curve_http_lock;

static SemaphoreHandle_t curve_http_lock(void)
{
    if (s_curve_http_lock == NULL) {
        s_curve_http_lock = xSemaphoreCreateMutex();
    }
    return s_curve_http_lock;
}

static esp_err_t curve_post_handler(httpd_req_t *req)
{
    SemaphoreHandle_t lock = curve_http_lock();
    if (lock != NULL) {
        (void)xSemaphoreTake(lock, portMAX_DELAY);
    }
    esp_err_t err = curve_post_handler_impl(req);
    if (lock != NULL) {
        (void)xSemaphoreGive(lock);
    }
    return err;
}

static esp_err_t curve_get_handler(httpd_req_t *req)
{
    SemaphoreHandle_t lock = curve_http_lock();
    if (lock != NULL) {
        (void)xSemaphoreTake(lock, portMAX_DELAY);
    }
    esp_err_t err = curve_get_handler_impl(req);
    if (lock != NULL) {
        (void)xSemaphoreGive(lock);
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* registration: one module per HTTP method on the /api/curve/ prefix        */
/* ------------------------------------------------------------------------- */

static const httpd_uri_t s_curve_post_uri = {
    .uri = CURVE_URI_PREFIX, /* prefix match (web_server uri_match: trailing '/') */
    .method = HTTP_POST,
    .handler = curve_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t s_curve_get_uri = {
    .uri = CURVE_URI_PREFIX,
    .method = HTTP_GET,
    .handler = curve_get_handler,
    .user_ctx = NULL,
};

static int CURVE_PREFIX_POST_INIT(const httpd_uri_t **uri_conf)
{
    *uri_conf = &s_curve_post_uri;
    return 0;
}

static int CURVE_PREFIX_GET_INIT(const httpd_uri_t **uri_conf)
{
    *uri_conf = &s_curve_get_uri;
    return 0;
}

static int CURVE_URI_EXIT(const httpd_uri_t **uri_conf)
{
    (void)uri_conf;
    return 0;
}

/* Priorities run before the "/" SPA fallback (0x90) and after the target flash
 * debug endpoint (0x70): ascending order = registration order in web_server. */
WEB_URI_MODULE_REGISTER(0x72, CURVE_PREFIX_POST_INIT, CURVE_URI_EXIT)
WEB_URI_MODULE_REGISTER(0x73, CURVE_PREFIX_GET_INIT, CURVE_URI_EXIT)
