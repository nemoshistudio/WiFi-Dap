/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - P1.11 WebSocket frame push.
 *
 * Endpoint: GET /ws/curve (WebSocket, same origin / same host_guard policy as
 * the rest of the web server).  The browser first configures sampling with
 * POST /api/curve/start, then connects and subscribes; the sampler pushes CVF2
 * batches (exactly the same binary format as GET /api/curve/frames) so the
 * page no longer has to poll.  HTTP polling remains fully functional.
 *
 * Client -> server text messages (UTF-8 JSON):
 *   {"cmd":"subscribe"}                      start pushing from the oldest ring frame
 *   {"cmd":"unsubscribe"}                    stop pushing (socket stays open)
 *   {"cmd":"ack","seq":N}                    optional flow-control ack (monotonic)
 *   {"cmd":"ping"}                           -> {"ok":true,"cmd":"pong"}
 * Server -> client:
 *   binary CVF2 batch (16 B header + frames), see docs/curve-local-sampler.md §9
 *   text JSON replies/errors: {"ok":true,...} / {"ok":false,"err":"..."}
 *
 * Backpressure: per-client minimum interval CURVE_WS_PUSH_INTERVAL_MS (20 ms),
 * at most CURVE_WS_MAX_BATCH_FRAMES (32) frames per pushed batch; if heap or a
 * socket send fails the batch is skipped and counted (`dropped_batches`); a
 * client whose socket send fails is detached.  The push runs on its own task so
 * a slow WebSocket client never blocks the sampler worker or the httpd task.
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
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cJSON.h>

#include "web_host_guard.h"
#include "web_uri_module.h"

#include "curve_priv.h"
#include "curve_ws.h"
#include "freertos/idf_additions.h"

#define CURVE_WS_TAG "curve_ws"
#define CURVE_WS_URI "/ws/curve"
#define CURVE_WS_CMD_MAX 256U
#define CURVE_WS_TASK_STACK 4096U
#define CURVE_WS_TASK_PRIO 3U
#define CURVE_WS_TASK_CORE 0 /* no SWD/DAP involvement: any core is fine */

static curve_ws_state_t s_ws;
static httpd_handle_t s_ws_hd[CURVE_WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_lock;      /* state table */
static SemaphoreHandle_t s_ws_send_lock; /* serialize WS frames on all sockets */
static TaskHandle_t s_ws_task;
static bool s_ws_inited;

static void curve_ws_push_task(void *arg);

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

static void curve_ws_init(void)
{
    if (s_ws_inited) {
        return;
    }
    s_ws_lock = xSemaphoreCreateMutex();
    s_ws_send_lock = xSemaphoreCreateMutex();
    if (s_ws_lock == NULL || s_ws_send_lock == NULL) {
        ESP_LOGE(CURVE_WS_TAG, "init: mutex creation failed");
        return;
    }
    curve_ws_state_init(&s_ws);
#if defined(CONFIG_SPIRAM) && (defined(CONFIG_IDF_TARGET_ESP32C5) || \
                                defined(CONFIG_IDF_TARGET_ESP32S3))
    /* C5 internal-RAM budget: the WebSocket curve push task is a background,
     * low-frequency task (no flash writes, no DMA); its stack lives in PSRAM.
     * Fall back to internal RAM if the PSRAM allocation fails. */
    {
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            curve_ws_push_task, "curve_ws", CURVE_WS_TASK_STACK, NULL,
            CURVE_WS_TASK_PRIO, &s_ws_task, CURVE_WS_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            ESP_LOGW(CURVE_WS_TAG, "init: PSRAM curve_ws stack unavailable, "
                     "using internal RAM");
            created = xTaskCreatePinnedToCore(curve_ws_push_task, "curve_ws",
                                              CURVE_WS_TASK_STACK, NULL,
                                              CURVE_WS_TASK_PRIO, &s_ws_task,
                                              CURVE_WS_TASK_CORE);
        }
        if (created != pdPASS) {
            ESP_LOGE(CURVE_WS_TAG, "init: push task creation failed");
            s_ws_task = NULL;
            return;
        }
    }
#else
    if (xTaskCreatePinnedToCore(curve_ws_push_task, "curve_ws", CURVE_WS_TASK_STACK,
                                NULL, CURVE_WS_TASK_PRIO, &s_ws_task,
                                CURVE_WS_TASK_CORE) != pdPASS) {
        ESP_LOGE(CURVE_WS_TAG, "init: push task creation failed");
        s_ws_task = NULL;
        return;
    }
#endif
    s_ws_inited = true;
    ESP_LOGI(CURVE_WS_TAG, "WebSocket curve push ready on %s", CURVE_WS_URI);
}

void curve_ws_notify_frame(void)
{
    if (s_ws_inited && s_ws_task != NULL) {
        (void)xTaskNotifyGive(s_ws_task);
    }
}

/* ------------------------------------------------------------------------- */
/* frame sending                                                             */
/* ------------------------------------------------------------------------- */

static esp_err_t ws_send_text_req(httpd_req_t *req, const char *fmt, ...)
{
    char body[192];
    va_list ap;
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .final = 1,
    };
    esp_err_t err;

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    frame.payload = (uint8_t *)body;
    frame.len = strlen(body);
    (void)xSemaphoreTake(s_ws_send_lock, portMAX_DELAY);
    err = httpd_ws_send_frame(req, &frame);
    (void)xSemaphoreGive(s_ws_send_lock);
    return err;
}

static esp_err_t ws_send_binary_async(int slot, const uint8_t *data, size_t len)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .final = 1,
        .payload = (uint8_t *)data, /* httpd copies while sending synchronously */
        .len = len,
    };
    esp_err_t err;

    (void)xSemaphoreTake(s_ws_send_lock, portMAX_DELAY);
    err = httpd_ws_send_frame_async(s_ws_hd[slot], s_ws.clients[slot].fd, &frame);
    (void)xSemaphoreGive(s_ws_send_lock);
    return err;
}

/* ------------------------------------------------------------------------- */
/* push task                                                                 */
/* ------------------------------------------------------------------------- */

static void curve_ws_push_once(void)
{
    uint32_t oldest = 0U;
    uint32_t latest = 0U;
    uint32_t now_ms;
    bool have_frames;

    if (!s_ws_inited) {
        return;
    }
    have_frames = curve_engine_ring_seqs(&oldest, &latest);
    now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (int slot = 0; slot < (int)CURVE_WS_MAX_CLIENTS; slot++) {
        uint32_t since = 0U;
        uint32_t first_seq = 0U;
        uint32_t count;
        bool gap = false;
        uint8_t *buf = NULL;
        size_t len = 0U;
        bool no_data = true;
        esp_err_t err;
        int fd;
        httpd_handle_t hd;

        if (!have_frames) {
            break;
        }
        (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        fd = s_ws.clients[slot].fd;
        hd = (fd >= 0) ? s_ws_hd[slot] : NULL;
        if (fd >= 0 && hd != NULL) {
            count = curve_ws_plan_push(&s_ws, fd, oldest, latest,
                                       CURVE_WS_MAX_BATCH_FRAMES, now_ms, &since,
                                       &first_seq, &gap);
        } else {
            count = 0U;
        }
        (void)xSemaphoreGive(s_ws_lock);
        if (count == 0U) {
            continue;
        }

        err = curve_engine_alloc_frames(since, count, &buf, &len, &no_data);
        if (err != ESP_OK || no_data || buf == NULL || len < CURVE_FRAME_HEADER_BYTES) {
            free(buf);
            (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
            curve_ws_mark_dropped(&s_ws, fd, now_ms);
            (void)xSemaphoreGive(s_ws_lock);
            ESP_LOGW(CURVE_WS_TAG, "fd %d: batch skipped (%s/no_data)", fd,
                     esp_err_to_name(err));
            continue;
        }
        {
            /* Use the real header values in case the ring moved between the
             * plan and the copy (sampler worker runs concurrently). */
            uint16_t actual = (uint16_t)(buf[6] | ((uint16_t)buf[7] << 8));
            uint32_t actual_first = (uint32_t)buf[8] |
                                    ((uint32_t)buf[9] << 8) |
                                    ((uint32_t)buf[10] << 16) |
                                    ((uint32_t)buf[11] << 24);

            err = ws_send_binary_async(slot, buf, len);
            free(buf);
            (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
            if (err == ESP_OK) {
                curve_ws_mark_pushed(&s_ws, fd, actual_first, actual, now_ms);
            } else {
                ESP_LOGW(CURVE_WS_TAG, "fd %d: WS send failed (%s), detaching",
                         fd, esp_err_to_name(err));
                curve_ws_mark_dropped(&s_ws, fd, now_ms);
                curve_ws_detach(&s_ws, fd);
                s_ws_hd[slot] = NULL;
            }
            (void)xSemaphoreGive(s_ws_lock);
            if (err != ESP_OK && hd != NULL) {
                httpd_sess_trigger_close(hd, fd);
            }
        }
    }
}

static void curve_ws_push_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        curve_ws_push_once();
    }
}

/* ------------------------------------------------------------------------- */
/* subscription handler                                                      */
/* ------------------------------------------------------------------------- */

static void ws_reply(httpd_req_t *req, const char *fmt, ...)
{
    char body[192];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    (void)ws_send_text_req(req, "%s", body);
}

static bool ws_handle_subscription(httpd_req_t *req, const char *payload, size_t len)
{
    cJSON *root;
    const cJSON *cmd;
    int fd = httpd_req_to_sockfd(req);

    if (len == 0U || len > CURVE_WS_CMD_MAX) {
        ws_reply(req, "{\"ok\":false,\"err\":\"empty/oversized command\"}");
        return false;
    }
    root = cJSON_Parse(payload);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ws_reply(req, "{\"ok\":false,\"err\":\"bad json\"}");
        return false;
    }
    cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL) {
        cJSON_Delete(root);
        ws_reply(req, "{\"ok\":false,\"err\":\"cmd missing\"}");
        return false;
    }

    (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    if (strcmp(cmd->valuestring, "subscribe") == 0) {
        bool ok = curve_ws_subscribe(&s_ws, fd);
        (void)xSemaphoreGive(s_ws_lock);
        if (ok) {
            ws_reply(req,
                     "{\"ok\":true,\"cmd\":\"subscribe\",\"interval_ms\":%u,"
                     "\"max_batch\":%u,\"format\":\"CVF2\"}",
                     (unsigned)CURVE_WS_PUSH_INTERVAL_MS,
                     (unsigned)CURVE_WS_MAX_BATCH_FRAMES);
            /* kick one immediate batch from the oldest frame */
            curve_ws_notify_frame();
        } else {
            ws_reply(req, "{\"ok\":false,\"err\":\"not connected\"}");
        }
    } else if (strcmp(cmd->valuestring, "unsubscribe") == 0) {
        bool ok = curve_ws_unsubscribe(&s_ws, fd);
        (void)xSemaphoreGive(s_ws_lock);
        ws_reply(req, ok ? "{\"ok\":true,\"cmd\":\"unsubscribe\"}"
                         : "{\"ok\":false,\"err\":\"not connected\"}");
    } else if (strcmp(cmd->valuestring, "ack") == 0) {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        uint32_t ack_seq = 0U;
        bool ok;

        if (cJSON_IsNumber(seq) && seq->valuedouble >= 0.0 &&
            seq->valuedouble <= 4294967295.0) {
            ack_seq = (uint32_t)seq->valuedouble;
        }
        ok = curve_ws_ack(&s_ws, fd, ack_seq);
        (void)xSemaphoreGive(s_ws_lock);
        (void)ok; /* batching is push-driven; ack is informational for now */
    } else if (strcmp(cmd->valuestring, "ping") == 0) {
        (void)xSemaphoreGive(s_ws_lock);
        ws_reply(req, "{\"ok\":true,\"cmd\":\"pong\"}");
    } else {
        (void)xSemaphoreGive(s_ws_lock);
        ws_reply(req, "{\"ok\":false,\"err\":\"unknown cmd\"}");
    }
    cJSON_Delete(root);
    return true;
}

/**
 * @brief Register the /ws/curve client (handshake context only).
 *
 * The Host/Origin guard needs the handshake request headers, so it must run
 * before the socket is used for curve frames.  IDF <= 5.2 reached this through
 * the URI handler; IDF >= 5.5 uses httpd_uri_t::ws_post_handshake_cb instead.
 *
 * @return client slot (>= 0) on success, -1 when the session was rejected.
 */
static int curve_ws_register_req(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    int slot;

    curve_ws_init();
    if (!s_ws_inited) {
        return -1;
    }
    if (!web_host_guard_ws_ok(req)) {
        ESP_LOGW(CURVE_WS_TAG, "handshake rejected (Host/Origin), fd %d", fd);
        httpd_sess_trigger_close(req->handle, fd);
        return -1;
    }
    (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    slot = curve_ws_attach(&s_ws, fd);
    if (slot >= 0) {
        s_ws_hd[slot] = req->handle;
    }
    (void)xSemaphoreGive(s_ws_lock);
    if (slot < 0) {
        ESP_LOGW(CURVE_WS_TAG, "too many WS clients, closing fd %d", fd);
        httpd_sess_trigger_close(req->handle, fd);
        return -1;
    }
    ESP_LOGI(CURVE_WS_TAG, "fd %d attached (slot %d)", fd, slot);
    return slot;
}

#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
static esp_err_t curve_ws_post_handshake_cb(httpd_req_t *req)
{
    return curve_ws_register_req(req) >= 0 ? ESP_OK : ESP_FAIL;
}
#endif

static esp_err_t curve_ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t pkt;
    esp_err_t err;

    curve_ws_init();
    if (!s_ws_inited) {
        return ESP_FAIL;
    }
    if (req->method == HTTP_GET) {
        return curve_ws_register_req(req) >= 0 ? ESP_OK : ESP_FAIL;
    }

    /* IDF >= 5.5 does not invoke the handler for the handshake request.  When
     * the build has no post-handshake hook, register on the first client frame
     * (the page sends {"cmd":"subscribe"} / "ping" right after open); without
     * this the push task would never see a client.  Attach is idempotent, so
     * the normal post-handshake path already did it in almost all cases. */
    (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    bool attached = curve_ws_find(&s_ws, fd) != NULL;
    (void)xSemaphoreGive(s_ws_lock);
    if (!attached) {
        int slot;
        (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        slot = curve_ws_attach(&s_ws, fd);
        if (slot >= 0) {
            s_ws_hd[slot] = req->handle;
        }
        (void)xSemaphoreGive(s_ws_lock);
        if (slot < 0) {
            ESP_LOGW(CURVE_WS_TAG, "too many WS clients, closing fd %d", fd);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_FAIL;
        }
        ESP_LOGI(CURVE_WS_TAG, "fd %d attached lazily (slot %d)", fd, slot);
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.len = 0U;
    /* First call: read only the frame length/type. */
    err = httpd_ws_recv_frame(req, &pkt, 0U);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    switch (pkt.type) {
    case HTTPD_WS_TYPE_TEXT: {
        char payload[CURVE_WS_CMD_MAX + 1U];

        if (pkt.len == 0U || pkt.len > CURVE_WS_CMD_MAX) {
            /* Do not drain arbitrary payloads: close the session (the only
             * way to resynchronise the stream on this idf version). */
            ESP_LOGW(CURVE_WS_TAG, "fd %d: bad command length %u", fd,
                     (unsigned)pkt.len);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_FAIL;
        }
        pkt.payload = (uint8_t *)payload;
        if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) {
            return ESP_FAIL;
        }
        payload[pkt.len] = '\0';
        (void)ws_handle_subscription(req, payload, pkt.len);
        return ESP_OK;
    }
    case HTTPD_WS_TYPE_CLOSE: {
        uint8_t close_payload[126];

        /* Drain only a payload the CLOSE frame actually carries.  Calling
         * httpd_ws_recv_frame() again for a zero-length frame re-reads the next
         * frame header and blocks in recv for recv_wait_timeout (10 s), which
         * stalls the single httpd task (and every HTTP/WS client behind it).
         * Browsers send a zero-payload CLOSE frame on ws.close() without a
         * code. */
        if (pkt.len > 0U) {
            if (pkt.len > sizeof(close_payload)) {
                ESP_LOGW(CURVE_WS_TAG, "fd %d: oversized close payload", fd);
                httpd_sess_trigger_close(req->handle, fd);
                return ESP_FAIL;
            }
            pkt.payload = close_payload;
            if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) {
                return ESP_FAIL;
            }
        }
        (void)xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        for (int slot = 0; slot < (int)CURVE_WS_MAX_CLIENTS; slot++) {
            if (s_ws.clients[slot].fd == fd) {
                curve_ws_detach(&s_ws, fd);
                s_ws_hd[slot] = NULL;
                break;
            }
        }
        (void)xSemaphoreGive(s_ws_lock);
        ESP_LOGI(CURVE_WS_TAG, "fd %d closed", fd);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_OK;
    }
    case HTTPD_WS_TYPE_PING: {
        uint8_t ping_payload[126];

        /* Same zero-length rule as CLOSE above. */
        if (pkt.len > sizeof(ping_payload)) {
            ESP_LOGW(CURVE_WS_TAG, "fd %d: oversized ping payload", fd);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_FAIL;
        }
        if (pkt.len > 0U) {
            pkt.payload = ping_payload;
            if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) {
                return ESP_FAIL;
            }
        }
        pkt.type = HTTPD_WS_TYPE_PONG;
        (void)xSemaphoreTake(s_ws_send_lock, portMAX_DELAY);
        err = httpd_ws_send_frame(req, &pkt);
        (void)xSemaphoreGive(s_ws_send_lock);
        return err;
    }
    case HTTPD_WS_TYPE_PONG:
        return ESP_OK;
    default:
        /* CONTINUE/BINARY: no fragmented or binary client->server messages. */
        ESP_LOGW(CURVE_WS_TAG, "fd %d: unsupported WS frame type %d", fd,
                 (int)pkt.type);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_FAIL;
    }
}

/* ------------------------------------------------------------------------- */
/* registration                                                              */
/* ------------------------------------------------------------------------- */

static const httpd_uri_t s_curve_ws_uri = {
    .uri = CURVE_WS_URI,
    .method = HTTP_GET,
    .handler = curve_ws_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .supported_subprotocol = NULL,
    .handle_ws_control_frames = true,
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    /* IDF >= 5.5: handshake no longer invokes .handler; attach the push client
     * (and enforce Host/Origin) from the post-handshake callback. */
    .ws_post_handshake_cb = curve_ws_post_handshake_cb,
#endif
};

static int CURVE_WS_URI_INIT(const httpd_uri_t **uri_conf)
{
    *uri_conf = &s_curve_ws_uri;
    return 0;
}

static int CURVE_WS_URI_EXIT(const httpd_uri_t **uri_conf)
{
    (void)uri_conf;
    return 0;
}

WEB_URI_MODULE_REGISTER(0x74, CURVE_WS_URI_INIT, CURVE_WS_URI_EXIT)
