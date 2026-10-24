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
 *   POST /api/curve/trace/start / stop   target-UART trace prototype (P1.8.3)
 *   GET  /api/curve/trace      raw target-UART trace bytes since=<byte seq>
 *   GET  /api/curve/backtrace  frame-pointer backtrace of the last trigger hit
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

#include "cJSON.h"

#include "web_host_guard.h"
#include "web_uri_module.h"

#include "curve_priv.h"

#define CURVE_URI_PREFIX  "/api/curve/"
#define CURVE_URI_SELFTEST CURVE_URI_PREFIX "selftest"
#define CURVE_URI_START    CURVE_URI_PREFIX "start"
#define CURVE_URI_START_BIN CURVE_URI_PREFIX "start-bin"
#define CURVE_URI_STOP     CURVE_URI_PREFIX "stop"
#define CURVE_URI_STATUS   CURVE_URI_PREFIX "status"
#define CURVE_URI_FRAMES   CURVE_URI_PREFIX "frames"
#define CURVE_URI_TRACE_START CURVE_URI_PREFIX "trace/start"
#define CURVE_URI_TRACE_STOP  CURVE_URI_PREFIX "trace/stop"
#define CURVE_URI_TRACE       CURVE_URI_PREFIX "trace"
#define CURVE_URI_TRACE_LINES CURVE_URI_PREFIX "trace/lines"
#define CURVE_URI_BACKTRACE   CURVE_URI_PREFIX "backtrace"

#define CURVE_DEFAULT_MAX_FRAMES 32U

/* The ESP httpd task has a single instance and a 4 KiB stack: request bodies
 * and response JSON use static buffers (same pattern as tf_debug_http.c). */
/* Request body is heap-allocated only while a POST body is read and released
 * before SWD bring-up.  Keeping this 16 KiB buffer out of .bss leaves the
 * largest internal heap block large enough for target detection when USB-NCM
 * and WiFi share one firmware image. */
static char *s_body;

/** Largest internal 8-bit free block: the JSON body and the frame ring need a
 *  single contiguous allocation, so this is the heap guard metric. */
static size_t curve_largest_free_block(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
}

static char *curve_body_acquire(void)
{
    if (s_body == NULL) {
        s_body = (char *)heap_caps_malloc(CURVE_JSON_BODY_MAX + 1U,
                                          MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    return s_body;
}

/* Forward declarations: these helpers are defined with the handlers below. */
static esp_err_t curve_send_error(httpd_req_t *req, const char *status,
                                  const char *fmt, ...);
static esp_err_t curve_start_respond(httpd_req_t *req);

/** Explicit 503 body for the JSON compatibility path: the unified NCM+WiFi
 *  heap is too fragmented for a single 16 KiB request buffer.  New clients use
 *  the streaming binary path instead of retrying. */
static esp_err_t curve_send_body_nomem(httpd_req_t *req)
{
    return curve_send_error(
        req, "503 Service Unavailable",
        "{\"ok\":false,\"err\":\"no contiguous heap for %u B JSON body "
        "(largest %u B); use POST /api/curve/start-bin\","
        "\"largest_free\":%u}",
        (unsigned)CURVE_JSON_BODY_MAX, (unsigned)curve_largest_free_block(),
        (unsigned)curve_largest_free_block());
}

static void curve_body_release(void)
{
    if (s_body != NULL) {
        heap_caps_free(s_body);
        s_body = NULL;
    }
}
static char s_json[CURVE_STATUS_JSON_MAX];
static curve_config_t s_http_cfg;
/* Target-UART trace response staging buffer (P1.8.3 prototype): copied out of
 * the trace ring under its mutex, then sent unlocked.  The HTTP handlers are
 * serialized by s_curve_http_lock, so one buffer is enough. */
static uint8_t s_trace_out[CURVE_TRACE_RING_BYTES];
/* P1-13 line-trace response staging (copied out of the line ring under its
 * mutex, then formatted into s_json).  Sized for one bounded response. */
static curve_trace_line_t s_trace_lines[CURVE_TRACE_LINES_PER_REQ];

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
    case ESP_ERR_NO_MEM:
        /* Explicit out-of-memory response instead of a silent SWD timeout. */
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

/** Drain an optional small request body (no large body buffer needed). */
static void curve_drain_small_body(httpd_req_t *req)
{
    char scratch[64];
    size_t remaining = req->content_len;
    int timeouts = 0;

    while (remaining > 0U) {
        size_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        int got = httpd_req_recv(req, scratch, chunk);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return;
            }
            continue;
        }
        if (got <= 0) {
            return;
        }
        remaining -= (size_t)got;
    }
}

/** Read a bounded (<= cap-1 byte) request body into a caller stack buffer. */
static esp_err_t curve_read_small_body(httpd_req_t *req, char *buf, size_t cap,
                                       size_t *out_len)
{
    size_t content_len = req->content_len;
    size_t received = 0U;
    int timeouts = 0;

    if (buf == NULL || out_len == NULL || cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    if (content_len >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (received < content_len) {
        int got = httpd_req_recv(req, buf + received, content_len - received);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (got <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)got;
    }
    buf[received] = '\0';
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

    /* Reclaim the previous session's frames ring BEFORE allocating the 16 KiB
     * body: the ring is intentionally kept after stop so frames/snapshot can
     * still be fetched, but leaving it allocated blocks the next start body. */
    if (curve_engine_is_idle()) {
        curve_engine_release_ring();
    }
    /* Heap guard: never attempt the 16 KiB block when the largest free block is
     * already too small; answer 503 with the measured numbers instead. */
    if (curve_largest_free_block() < CURVE_JSON_BODY_MAX + 1U ||
        curve_body_acquire() == NULL) {
        return curve_send_body_nomem(req);
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
    return curve_start_respond(req);
}

/** Start response shared by the JSON and binary (CBS1) start handlers.  Must be
 *  called with s_http_cfg already parsed and the engine running. */
static esp_err_t curve_start_respond(httpd_req_t *req)
{
    curve_status_t status;

    curve_engine_status(&status);
    (void)snprintf(s_json, sizeof(s_json),
                   "{\"ok\":true,\"group_count\":%u,\"series_count\":%u,"
                   "\"sample_bytes\":%u,\"interval_us\":%u,"
                   "\"trigger\":\"%s\",\"breakpoint_address\":%u,"
                   "\"halt\":%s,\"pc_capture\":%s,"
                   "\"watchpoint_address\":%u,\"watchpoint_size\":%u,"
                   "\"watchpoint_access\":\"%s\",\"watch_count\":%u,"
                   "\"watchpoint2_address\":%u,\"watchpoint2_size\":%u,"
                   "\"watchpoint2_access\":\"%s\","
                   "\"watchpoint3_address\":%u,\"watchpoint3_size\":%u,"
                   "\"watchpoint3_access\":\"%s\","
                   "\"watchpoint4_address\":%u,\"watchpoint4_size\":%u,"
                   "\"watchpoint4_access\":\"%s\","
                   "\"snapshot_state\":\"%s\",\"snapshot_after\":%u,"
                   "\"backtrace\":%s,\"sample_every_max\":%u,"
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
                   (unsigned)status.watch_count,
                   (unsigned)s_http_cfg.watch2_address,
                   (unsigned)s_http_cfg.watch2_size,
                   curve_watch_access_name(s_http_cfg.watch2_access),
                   (unsigned)s_http_cfg.watch_addr[2],
                   (unsigned)s_http_cfg.watch_sz[2],
                   curve_watch_access_name(s_http_cfg.watch_acc[2]),
                   (unsigned)s_http_cfg.watch_addr[3],
                   (unsigned)s_http_cfg.watch_sz[3],
                   curve_watch_access_name(s_http_cfg.watch_acc[3]),
                   curve_snapshot_state_name(status.snapshot_state),
                   (unsigned)status.snapshot_after,
                   s_http_cfg.backtrace ? "true" : "false",
                   (unsigned)status.sample_every_max,
                   (unsigned)status.link_hz,
                   (unsigned)status.dpidr);
    return curve_send_json(req, "200 OK", s_json);
}

/* ------------------------------------------------------------------------- */
/* P0-1: POST /api/curve/start-bin - streaming compact binary start request  */
/* ------------------------------------------------------------------------- */

/** Streaming body source for curve_bin_parse(): pulls exactly `len` bytes from
 *  the HTTP request in bounded socket reads.  No intermediate request-body
 *  buffer is allocated, so a fragmented heap cannot fail this path. */
typedef struct {
    httpd_req_t *req;
    size_t remaining;
} curve_http_bin_ctx_t;

static esp_err_t curve_http_bin_read(void *ctx, void *dst, size_t len)
{
    curve_http_bin_ctx_t *bin = (curve_http_bin_ctx_t *)ctx;
    uint8_t *out = (uint8_t *)dst;
    size_t received = 0U;
    int timeouts = 0;

    if (bin == NULL || out == NULL || len > bin->remaining) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (received < len) {
        int got = httpd_req_recv(bin->req, (char *)out + received,
                                 len - received);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (got <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)got;
    }
    bin->remaining -= len;
    return ESP_OK;
}

static esp_err_t curve_handle_start_bin(httpd_req_t *req)
{
    curve_http_bin_ctx_t bin;
    char err_msg[64] = {0};
    esp_err_t err;

    if (req->content_len == 0U) {
        return curve_send_error(req, "400 Bad Request",
                                "{\"ok\":false,\"err\":\"empty CBS1 body\"}");
    }
    if (req->content_len > CURVE_BIN_BODY_MAX) {
        return curve_send_error(req, "413 Payload Too Large",
                                "{\"ok\":false,\"err\":\"CBS1 body > %u bytes\"}",
                                (unsigned)CURVE_BIN_BODY_MAX);
    }
    if (curve_engine_is_idle()) {
        /* Same reason as the JSON start: drop the previous session's ring so
         * the new start has the heap headroom it needs. */
        curve_engine_release_ring();
    }
    if (!curve_engine_is_idle()) {
        return curve_send_error(req, "409 Conflict",
                                "{\"ok\":false,\"err\":\"busy\"}");
    }
    bin.req = req;
    bin.remaining = req->content_len;
    err = curve_bin_parse(curve_http_bin_read, &bin, &s_http_cfg, err_msg,
                          sizeof(err_msg));
    if (err == ESP_ERR_INVALID_SIZE) {
        return curve_send_error(req, "413 Payload Too Large",
                                "{\"ok\":false,\"err\":\"%s\"}",
                                err_msg[0] != '\0' ? err_msg : "truncated body");
    }
    if (err != ESP_OK) {
        return curve_send_error(req, curve_http_status(err),
                                "{\"ok\":false,\"err\":\"%s\"}",
                                err_msg[0] != '\0' ? err_msg
                                                    : esp_err_to_name(err));
    }
    if (bin.remaining != 0U) {
        return curve_send_error(req, "400 Bad Request",
                                "{\"ok\":false,\"err\":\"CBS1 trailing body "
                                "bytes\"}");
    }
    err = curve_engine_start(&s_http_cfg, err_msg, sizeof(err_msg));
    if (err != ESP_OK) {
        return curve_send_error(req, curve_http_status(err),
                                "{\"ok\":false,\"err\":\"%s\"}",
                                err_msg[0] != '\0' ? err_msg
                                                    : esp_err_to_name(err));
    }
    return curve_start_respond(req);
}

static esp_err_t curve_handle_stop(httpd_req_t *req)
{
    char err_msg[64] = {0};
    esp_err_t err;

    /* Optional body, drained so keep-alive connections stay usable. */
    curve_drain_small_body(req);
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
/* P1.8.3 prototype: target-UART trace capture                               */
/* ------------------------------------------------------------------------- */

static esp_err_t curve_handle_trace_start(httpd_req_t *req)
{
    char body[129];
    size_t body_len = 0U;
    char trigger[CURVE_TRACE_TRIGGER_MAX] = {0};
    bool stop_after = false;
    esp_err_t err;

    /* P1-13: optional small JSON config {"trigger":"PATTERN","stop_after":bool}.
     * Read into a stack buffer: the 16 KiB JSON start path is not used here. */
    if (req->content_len > 0U) {
        err = curve_read_small_body(req, body, sizeof(body), &body_len);
        if (err == ESP_ERR_INVALID_SIZE) {
            return curve_send_error(req, "413 Payload Too Large",
                                    "{\"ok\":false,\"err\":\"trace config > %u "
                                    "bytes\"}",
                                    (unsigned)(sizeof(body) - 1U));
        }
        if (err != ESP_OK) {
            return curve_send_error_ok(req, err, "read trace config");
        }
        if (body_len > 0U) {
            cJSON *root = cJSON_ParseWithLength(body, body_len);

            if (root == NULL || !cJSON_IsObject(root)) {
                cJSON_Delete(root);
                return curve_send_error(req, "400 Bad Request",
                                        "{\"ok\":false,\"err\":\"bad trace "
                                        "config JSON\"}");
            }
            const cJSON *trigger_item =
                cJSON_GetObjectItemCaseSensitive(root, "trigger");
            const cJSON *stop_item =
                cJSON_GetObjectItemCaseSensitive(root, "stop_after");

            if (trigger_item != NULL) {
                if (!cJSON_IsString(trigger_item) ||
                    trigger_item->valuestring == NULL ||
                    strlen(trigger_item->valuestring) >= sizeof(trigger)) {
                    cJSON_Delete(root);
                    return curve_send_error(
                        req, "400 Bad Request",
                        "{\"ok\":false,\"err\":\"trace trigger must be a "
                        "string < %u bytes\"}",
                        (unsigned)sizeof(trigger));
                }
                (void)snprintf(trigger, sizeof(trigger), "%s",
                               trigger_item->valuestring);
            }
            if (stop_item != NULL) {
                if (!cJSON_IsBool(stop_item)) {
                    cJSON_Delete(root);
                    return curve_send_error(
                        req, "400 Bad Request",
                        "{\"ok\":false,\"err\":\"trace stop_after must be "
                        "a boolean\"}");
                }
                stop_after = cJSON_IsTrue(stop_item) ? true : false;
            }
            cJSON_Delete(root);
        }
    }
    err = curve_trace_start_with_config(trigger[0] != '\0' ? trigger : NULL,
                                        stop_after);
    if (err != ESP_OK) {
        return curve_send_error_ok(req, err, "trace start");
    }
    (void)snprintf(s_json, sizeof(s_json),
                   "{\"ok\":true,\"active\":true,\"ring_bytes\":%u,"
                   "\"line_ring\":%u,\"trigger\":\"%s\","
                   "\"stop_after\":%s}",
                   (unsigned)CURVE_TRACE_RING_BYTES,
                   (unsigned)CURVE_TRACE_LINE_MAX, trigger,
                   stop_after ? "true" : "false");
    return curve_send_json(req, "200 OK", s_json);
}

static esp_err_t curve_handle_trace_stop(httpd_req_t *req)
{
    esp_err_t err;

    curve_drain_small_body(req);
    err = curve_trace_stop();
    if (err != ESP_OK) {
        return curve_send_error_ok(req, err, "trace stop");
    }
    return curve_send_json(req, "200 OK", "{\"ok\":true,\"active\":false}");
}

static esp_err_t curve_handle_trace(httpd_req_t *req)
{
    uint32_t since = 0U;
    uint32_t first_seq = 0U;
    bool gap = false;
    bool active = false;
    uint32_t total = 0U;
    uint32_t dropped = 0U;
    uint32_t count;
    uint32_t next_seq;
    char header[24];

    if (!curve_query_u32(req->uri, "since", &since)) {
        since = 0U;
    }
    count = curve_trace_read(since, s_trace_out, sizeof(s_trace_out),
                             &first_seq, &gap);
    curve_trace_get_status(&active, &total, &dropped);
    next_seq = (count > 0U) ? (first_seq + count) : since;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Curve-Trace-Active", active ? "1" : "0");
    httpd_resp_set_hdr(req, "X-Curve-Trace-Gap", gap ? "1" : "0");
    (void)snprintf(header, sizeof(header), "%u", (unsigned)next_seq);
    httpd_resp_set_hdr(req, "X-Curve-Trace-Seq", header);
    (void)snprintf(header, sizeof(header), "%u", (unsigned)total);
    httpd_resp_set_hdr(req, "X-Curve-Trace-Total", header);
    (void)snprintf(header, sizeof(header), "%u", (unsigned)dropped);
    httpd_resp_set_hdr(req, "X-Curve-Trace-Dropped", header);

    if (count == 0U) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, (const char *)s_trace_out, (ssize_t)count);
}

/* ------------------------------------------------------------------------- */
/* P1-13: GET /api/curve/trace/lines - timestamped, line-buffered trace      */
/* ------------------------------------------------------------------------- */

/** Copy `src` (len bytes) into dst as a JSON string body, escaping '"' and
 *  '\'.  Returns the number of bytes written (always < dst_len). */
static size_t curve_json_escape(char *dst, size_t dst_len, const char *src,
                                size_t src_len)
{
    size_t out = 0U;

    for (size_t i = 0U; i < src_len && out + 2U < dst_len; i++) {
        char c = src[i];

        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
        }
        dst[out++] = c;
    }
    dst[out] = '\0';
    return out;
}

static esp_err_t curve_handle_trace_lines(httpd_req_t *req)
{
    uint32_t since = 0U;
    uint32_t max = CURVE_TRACE_LINES_PER_REQ;
    uint32_t first_seq = 0U;
    uint32_t count;
    uint32_t total = 0U;
    uint32_t dropped = 0U;
    uint32_t trigger_seq = 0U;
    bool gap = false;
    bool active = false;
    bool triggered = false;
    size_t off;

    if (!curve_query_u32(req->uri, "since", &since)) {
        since = 0U;
    }
    if (!curve_query_u32(req->uri, "max", &max) || max == 0U) {
        max = CURVE_TRACE_LINES_PER_REQ;
    }
    if (max > CURVE_TRACE_LINES_PER_REQ) {
        max = CURVE_TRACE_LINES_PER_REQ;
    }
    count = curve_trace_lines_read(since, max, s_trace_lines, &first_seq, &gap);
    curve_trace_lines_get_status(&active, &total, &dropped, &triggered,
                                 &trigger_seq);

    off = (size_t)snprintf(
        s_json, sizeof(s_json),
        "{\"ok\":true,\"active\":%s,\"gap\":%s,\"next_seq\":%u,"
        "\"total_lines\":%u,\"dropped_lines\":%u,\"triggered\":%s,"
        "\"trigger_seq\":%u,\"lines\":[",
        active ? "true" : "false", gap ? "true" : "false",
        (unsigned)((count > 0U) ? (first_seq + count) : since),
        (unsigned)total, (unsigned)dropped, triggered ? "true" : "false",
        (unsigned)trigger_seq);
    for (uint32_t i = 0U; i < count && off + 220U < sizeof(s_json); i++) {
        char text[CURVE_TRACE_LINE_TEXT_MAX * 2U + 1U];
        (void)curve_json_escape(text, sizeof(text), s_trace_lines[i].text,
                                 s_trace_lines[i].len);
        int written = snprintf(
            s_json + off, sizeof(s_json) - off,
            "%s{\"seq\":%u,\"t_us\":%llu,\"truncated\":%s,"
            "\"text\":\"%s\"}",
            (i == 0U) ? "" : ",", (unsigned)s_trace_lines[i].seq,
            (unsigned long long)s_trace_lines[i].t_us,
            (s_trace_lines[i].flags & CURVE_TRACE_LINE_FLAG_TRUNCATED) != 0U
                ? "true"
                : "false",
            text);

        if (written <= 0) {
            break;
        }
        off += (size_t)written;
    }
    if (off + 4U < sizeof(s_json)) {
        (void)snprintf(s_json + off, sizeof(s_json) - off, "]}");
    }
    return curve_send_json(req, "200 OK", s_json);
}

/* ------------------------------------------------------------------------- */
/* P1.8.2 prototype: backtrace of the last trigger hit                       */
/* ------------------------------------------------------------------------- */

static esp_err_t curve_handle_backtrace(httpd_req_t *req)
{
    curve_backtrace_t bt;
    size_t off;

    if (!curve_engine_backtrace(&bt)) {
        return curve_send_json(req, "200 OK",
                               "{\"ok\":true,\"valid\":false,\"count\":0,"
                               "\"truncated\":false,\"pcs\":[]}");
    }
    off = (size_t)snprintf(s_json, sizeof(s_json),
                           "{\"ok\":true,\"valid\":true,\"count\":%u,"
                           "\"truncated\":%s,\"pcs\":[",
                           (unsigned)bt.count,
                           bt.truncated ? "true" : "false");
    for (uint8_t i = 0U; i < bt.count && off + 24U < sizeof(s_json); i++) {
        int written = snprintf(s_json + off, sizeof(s_json) - off,
                               "%s\"0x%08X\"", (i != 0U) ? "," : "",
                               (unsigned)bt.pcs[i]);

        if (written <= 0) {
            break;
        }
        off += (size_t)written;
    }
    if (off + 3U < sizeof(s_json)) {
        (void)snprintf(s_json + off, sizeof(s_json) - off, "]}");
    }
    return curve_send_json(req, "200 OK", s_json);
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
    if (curve_path_is(uri, CURVE_URI_START_BIN)) {
        return curve_handle_start_bin(req);
    }
    if (curve_path_is(uri, CURVE_URI_START)) {
        return curve_handle_start(req);
    }
    if (curve_path_is(uri, CURVE_URI_STOP)) {
        return curve_handle_stop(req);
    }
    if (curve_path_is(uri, CURVE_URI_TRACE_START)) {
        return curve_handle_trace_start(req);
    }
    if (curve_path_is(uri, CURVE_URI_TRACE_STOP)) {
        return curve_handle_trace_stop(req);
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
    if (curve_path_is(uri, CURVE_URI_TRACE_LINES)) {
        return curve_handle_trace_lines(req);
    }
    if (curve_path_is(uri, CURVE_URI_TRACE)) {
        return curve_handle_trace(req);
    }
    if (curve_path_is(uri, CURVE_URI_BACKTRACE)) {
        return curve_handle_backtrace(req);
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
