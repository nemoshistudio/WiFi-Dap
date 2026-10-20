/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "web_uri_module.h"
#include "api_json_router.h"
#include "memory_pool.h"
#include "wt_ws_router.h"
#include "wt_data_def.h"
#include "birdiebot_auth.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include <cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwipopts.h>
#include <lwip/netdb.h>

#define TAG __FILE_NAME__


// Error message templates
#define ERROR_MSG_TEMPLATE(err_msg, err_code) "{\"error\":\"" err_msg "\", \"code\":" #err_code "}"

// Error messages
#define MSG_BAD_REQUEST_ERROR  ERROR_MSG_TEMPLATE("Bad json request", 2)
#define MSG_JSON_ERROR         ERROR_MSG_TEMPLATE("JSON parse error", 3)
#define MSG_SEND_JSON_ERROR    ERROR_MSG_TEMPLATE("JSON generation error", 3)
#define MSG_INTERNAL_ERROR     ERROR_MSG_TEMPLATE("Internal error", 3)
#define MSG_UNSUPPORTED_CMD    ERROR_MSG_TEMPLATE("Unsupported cmd", 4)
#define MSG_PROPERTY_ERROR     ERROR_MSG_TEMPLATE("Property error", 5)
#define MSG_BUSY_ERROR         ERROR_MSG_TEMPLATE("Resource busy", 6)
#define MSG_UNAUTHORIZED       ERROR_MSG_TEMPLATE("unauthorized", 7)
/* docs/ws-binary-router.md §3.2: malformed/oversized inbound binary frame. */
#define MSG_BINARY_ERROR       "{\"error\":\"Bad binary frame\",\"code\":2}"

/* SYSTEM(0)/AUTH(0): pairing-token frame (docs/protocol.md §2.1). */
#define WS_SYSTEM_MODULE_ID    0
#define WS_SYS_CMD_AUTH        0
#define MSG_AUTH_OK            "{\"module\":0,\"cmd\":0,\"ok\":true}"

#define GET_FD_IDX(fd) ((fd) - LWIP_SOCKET_OFFSET)

#define WS_MODULE_ID 3

typedef struct ws_msg_t {
	api_json_req_t json;
	api_json_module_async_t async;
	httpd_handle_t hd;
	int fd;
	httpd_ws_frame_t ws_pkt;
	uint8_t delim[0];
	uint8_t payload[0]; /* size = static_buf_size - offsetof(this, delim) */
} ws_msg_t;

#define PAYLOAD_LEN memory_pool_get_buf_size() - sizeof(ws_msg_t)

struct ws_ctx_t {
	struct ws_client_info_t {
		httpd_handle_t hd;
		int fd; /* range 64 - max_socket ~ 64 */
	} clients[CONFIG_LWIP_MAX_SOCKETS];
	TaskHandle_t task_heartbeat;
	/* valid <client_count> fd are stored at beginning of the array
	 * use GET_FD_IDX to get the index in clients */
	uint8_t valid_fd[CONFIG_LWIP_MAX_SOCKETS];
	int8_t client_count;

	/* protects clients[]/valid_fd[]/client_count: open/close happen in the
	 * httpd task while heartbeat and the ws_router tx task iterate the list. */
	struct {
		SemaphoreHandle_t mutex;
		StaticSemaphore_t xMutexBuffer;
	} list_lock;

	struct {
		SemaphoreHandle_t mutex;
		StaticSemaphore_t xMutexBuffer;
	} lock[CONFIG_LWIP_MAX_SOCKETS];
} ws_ctx;

static int ws_on_text_data(httpd_req_t *req, ws_msg_t *ws_msg);
static int ws_on_binary_data(httpd_req_t *req, ws_msg_t *ws_msg);
static int ws_on_socket_open(httpd_req_t *req);
static int ws_on_close(httpd_req_t *req, httpd_ws_frame_t *ws_pkt, void *msg);

/* send with lock per fd */
static inline int ws_send_frame_safe(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame);

static void ws_async_resp(void *arg);
static void async_send_out_cb(void *arg, int module_status);
static void json_to_text(ws_msg_t *msg);

/* Binary data path (docs/ws-binary-router.md / docs/protocol.md §3). */
static int ws_bin_tx_port(const uint8_t *frame, size_t len);
static void ws_broadcast_frame(httpd_ws_frame_t *pkt, bool authed_only);
static void ws_send_binary_error(httpd_req_t *req);
#if BIRDIEBOT_AUTH_ENABLED
static void ws_send_text_literal(httpd_req_t *req, const char *text);
static void ws_send_unauthorized(httpd_req_t *req, int module, int cmd);
#endif

/* Heartbeat related */
static inline int8_t ws_add_fd(httpd_handle_t hd, int fd);
static inline void ws_rm_fd(int fd);

_Noreturn static void heartbeat_task(void *arg);



static esp_err_t ws_req_handler(httpd_req_t *req)
{
	if (unlikely(req->method == HTTP_GET)) {
		return ws_on_socket_open(req);
	}
#ifdef WT_DEBUG_MODE
	ESP_LOGI(TAG, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info:%d, client_count: %d", req->handle,
	         httpd_req_to_sockfd(req), httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)),
	         ws_ctx.client_count);
#endif

	int err = ESP_OK;
	httpd_ws_frame_t *ws_pkt;
	ws_msg_t *ws_msg;

	ws_msg = memory_pool_get(pdMS_TO_TICKS(10));
	if (unlikely(ws_msg == NULL)) {
		httpd_ws_frame_t resp_pkt;
		resp_pkt.type = HTTPD_WS_TYPE_TEXT;
		resp_pkt.len = strlen(MSG_BUSY_ERROR);
		resp_pkt.payload = (uint8_t *)MSG_BUSY_ERROR;
		resp_pkt.final = 1;
		ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), &resp_pkt);
		goto end;
	}
	ws_pkt = &ws_msg->ws_pkt;
	ws_pkt->len = 0;

	/* get and check frame size */
	err = httpd_ws_recv_frame(req, ws_pkt, 0);
	if (unlikely(err != ESP_OK)) {
		ESP_LOGE(TAG, "ws recv len error");
		return ws_on_close(req, ws_pkt, ws_msg);
	}
#ifdef WT_DEBUG_MODE
	ESP_LOGI(TAG, "frame len: %d, type: %d", ws_pkt->len, ws_pkt->type);
#endif
	if (unlikely(ws_pkt->len > PAYLOAD_LEN)) {
		ESP_LOGE(TAG, "frame len is too big");
		return ws_on_close(req, ws_pkt, ws_msg);
	}
	ws_pkt->payload = ws_msg->payload;
	switch (ws_pkt->type) {
	case HTTPD_WS_TYPE_CONTINUE:
		/* No fragment aggregation (docs/ws-binary-router.md §3/§5): only
		 * FIN=1 frames are routed. Drain the fragment payload so the WS
		 * stream does not desync, then answer with the binary error frame. */
		ESP_LOGE(TAG, "WS Continue not handled (len %u)", (unsigned)ws_pkt->len);
		if (httpd_ws_recv_frame(req, ws_pkt, ws_pkt->len) != ESP_OK) {
			ESP_LOGE(TAG, "ws recv continue data error");
			return ws_on_close(req, ws_pkt, ws_msg);
		}
		ws_send_binary_error(req);
		goto end;
	case HTTPD_WS_TYPE_TEXT:
		/* read incoming data */
		err = httpd_ws_recv_frame(req, ws_pkt, ws_pkt->len);
		if (unlikely(err != ESP_OK)) {
			ESP_LOGE(TAG, "ws recv data error");
			return ws_on_close(req, ws_pkt, ws_msg);
		}
		return ws_on_text_data(req, ws_msg);
	case HTTPD_WS_TYPE_BINARY:
		return ws_on_binary_data(req, ws_msg);
	case HTTPD_WS_TYPE_CLOSE:
		if ((err = httpd_ws_recv_frame(req, ws_pkt, 126)) != ESP_OK) {
			ESP_LOGE(TAG, "Cannot receive the full CLOSE frame");
		}
		return ws_on_close(req, ws_pkt, ws_msg);
	case HTTPD_WS_TYPE_PING:
		/* Now turn the frame to PONG */
		if ((err = httpd_ws_recv_frame(req, ws_pkt, 126)) != ESP_OK) {
			ESP_LOGE(TAG, "Cannot receive the full PONG frame");
			return ws_on_close(req, ws_pkt, ws_msg);
		}
		ws_pkt->type = HTTPD_WS_TYPE_PONG;
		err = ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), ws_pkt);
		if (err) {
			ESP_LOGE(TAG, "Cannot send PONG frame %s", esp_err_to_name(err));
			return ws_on_close(req, ws_pkt, ws_msg);
		}
		goto end;
	case HTTPD_WS_TYPE_PONG:
		err = ESP_OK;
		goto end;
	default:
		err = ESP_FAIL;
		goto end;
	}
end:
	memory_pool_put(ws_msg);
	return err;
}

void ws_set_err_msg(httpd_ws_frame_t *p_frame, api_json_req_status_e ret);

int ws_on_text_data(httpd_req_t *req, ws_msg_t *ws_msg)
{
	int err = ESP_OK;
	int ret;
	httpd_ws_frame_t *ws_pkt;

	ws_pkt = &ws_msg->ws_pkt;
	/* Never log the raw JSON payload at INFO level: it may contain WiFi
	 * passwords (WIFI/CONNECT) or the web-auth pairing token (SYSTEM/0/AUTH). */
	ESP_LOGI(TAG, "ws text frame: len %u", (unsigned) ws_pkt->len);
#ifdef WT_DEBUG_MODE
	ESP_LOGI(TAG, "%.*s", ws_pkt->len, ws_pkt->payload);
#endif
	ESP_LOGI(TAG, "heap min: %lu, cur: %lu", esp_get_minimum_free_heap_size(), esp_get_free_heap_size());

	/* Decode */
	ws_msg->json.in = cJSON_ParseWithLength((char *)ws_pkt->payload, ws_pkt->len);
	if (unlikely(ws_msg->json.in == NULL)) {
		ws_pkt->payload = (uint8_t *)MSG_JSON_ERROR;
		ws_pkt->len = strlen(MSG_JSON_ERROR);
		goto put_buf;
	}

#if BIRDIEBOT_AUTH_ENABLED
	/* Web auth gate (docs/protocol.md §1/§2.1): while the connection is not
	 * authorized only SYSTEM/0/AUTH is processed; every other command gets
	 * {"err":"unauthorized","code":7} and is not routed to a module. */
	{
		int fd = httpd_req_to_sockfd(req);
		int module = -1;
		int cmd = -1;
		cJSON *module_json = cJSON_GetObjectItem(ws_msg->json.in, "module");
		cJSON *cmd_json = cJSON_GetObjectItem(ws_msg->json.in, "cmd");

		if (cJSON_IsNumber(module_json)) {
			module = module_json->valueint;
		}
		if (cJSON_IsNumber(cmd_json)) {
			cmd = cmd_json->valueint;
		}

		if (module == WS_SYSTEM_MODULE_ID && cmd == WS_SYS_CMD_AUTH) {
			const char *token = cJSON_GetStringValue(
				cJSON_GetObjectItem(ws_msg->json.in, "token"));
			if (birdiebot_auth_ws_token_ok(token, fd)) {
				ESP_LOGI(TAG, "ws fd %d authorized by SYSTEM/0/AUTH", fd);
				ws_send_text_literal(req, MSG_AUTH_OK);
			} else {
				ESP_LOGW(TAG, "ws fd %d AUTH failed (missing/invalid token)", fd);
				ws_send_unauthorized(req, WS_SYSTEM_MODULE_ID, WS_SYS_CMD_AUTH);
			}
			cJSON_Delete(ws_msg->json.in);
			memory_pool_put(ws_msg);
			return ESP_OK;
		}

		if (!birdiebot_auth_ws_fd_ok(fd)) {
			ESP_LOGW(TAG, "ws fd %d not authorized (module %d cmd %d)", fd, module, cmd);
			ws_send_unauthorized(req, module, cmd);
			cJSON_Delete(ws_msg->json.in);
			memory_pool_put(ws_msg);
			return ESP_OK;
		}
	}
#endif /* BIRDIEBOT_AUTH_ENABLED */

	ws_msg->json.out = NULL;
	ws_msg->json.out_flag = 0;
	ret = api_json_route(&ws_msg->json, &ws_msg->async);
	if (ret == API_JSON_ASYNC) {
		ws_msg->hd = req->handle;
		ws_msg->fd = httpd_req_to_sockfd(req);
		ws_msg->async.req_task.send_out.cb = async_send_out_cb;
		ws_msg->async.req_task.send_out.arg = ws_msg;
		if (req_queue_push_long_run(&ws_msg->async.req_task, pdMS_TO_TICKS(20))) {
			/* queued failed */
			goto end;
		}
		/* ret, buf will be release latter in async send out */
		return ESP_OK;
	} else if (ret != API_JSON_OK) {
		ws_set_err_msg(ws_pkt, ret);
		goto end;
	} else if (ws_msg->json.out == NULL) {
		/* API exec ok */
		goto end;
	}

	/* api function returns something, send it to http client */
	json_to_text(ws_msg);

end:
	cJSON_Delete(ws_msg->json.in);
put_buf:
	ws_pkt->final = 1;
	ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), ws_pkt);
	memory_pool_put(ws_msg);
	return err;
}

void ws_set_err_msg(httpd_ws_frame_t *p_frame, api_json_req_status_e ret)
{
	p_frame->final = 1;
	switch (ret) {
	case API_JSON_BAD_REQUEST:
		p_frame->len = strlen(MSG_BAD_REQUEST_ERROR);
		p_frame->payload = (uint8_t *)MSG_BAD_REQUEST_ERROR;
		break;
	case API_JSON_BUSY:
		p_frame->len = strlen(MSG_BUSY_ERROR);
		p_frame->payload = (uint8_t *)MSG_BUSY_ERROR;
		break;
	case API_JSON_UNSUPPORTED_CMD:
		p_frame->len = strlen(MSG_UNSUPPORTED_CMD);
		p_frame->payload = (uint8_t *)MSG_UNSUPPORTED_CMD;
		break;
	case API_JSON_PROPERTY_ERR:
		p_frame->len = strlen(MSG_PROPERTY_ERROR);
		p_frame->payload = (uint8_t *)MSG_PROPERTY_ERROR;
		break;
	case API_JSON_UNAUTHORIZED:
		p_frame->len = strlen(MSG_UNAUTHORIZED);
		p_frame->payload = (uint8_t *)MSG_UNAUTHORIZED;
		break;
	default:
		p_frame->len = strlen(MSG_INTERNAL_ERROR);
		p_frame->payload = (uint8_t *)MSG_INTERNAL_ERROR;
		return;
	}
}

_Static_assert(sizeof(httpd_ws_frame_t) <= 16, "bin_data_internal padding not sufficient for httpd_ws_frame_t");

/* Reply with docs/ws-binary-router.md §3.2 error frame (text, FIN=1). */
static void ws_send_binary_error(httpd_req_t *req)
{
	httpd_ws_frame_t pkt = {
		.type = HTTPD_WS_TYPE_TEXT,
		.final = 1,
		.payload = (uint8_t *)MSG_BINARY_ERROR,
		.len = strlen(MSG_BINARY_ERROR),
	};
	ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), &pkt);
}

#if BIRDIEBOT_AUTH_ENABLED
/** Send a static JSON text frame (FIN=1) to the requesting client. */
static void ws_send_text_literal(httpd_req_t *req, const char *text)
{
	httpd_ws_frame_t pkt = {
		.type = HTTPD_WS_TYPE_TEXT,
		.final = 1,
		.payload = (uint8_t *) text,
		.len = strlen(text),
	};
	ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), &pkt);
}

/**
 * @brief Reply docs/protocol.md §1.1 unauthorized error (code 7) to this fd.
 *
 * Echoes the request module/cmd so the front-end can route the error; -1
 * values (unparseable request) are reported as module 0 / cmd 0.
 */
static void ws_send_unauthorized(httpd_req_t *req, int module, int cmd)
{
	char buf[96];
	int len = snprintf(buf, sizeof(buf),
	                   "{\"module\":%d,\"cmd\":%d,\"err\":\"unauthorized\",\"code\":7}",
	                   module < 0 ? 0 : module, cmd < 0 ? 0 : cmd);
	if (len <= 0 || len >= (int) sizeof(buf)) {
		return;
	}
	httpd_ws_frame_t pkt = {
		.type = HTTPD_WS_TYPE_TEXT,
		.final = 1,
		.payload = (uint8_t *) buf,
		.len = (size_t) len,
	};
	ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), &pkt);
}
#endif /* BIRDIEBOT_AUTH_ENABLED */

/**
 * @brief Inbound WS binary frame: validate, split the 4-byte header, dispatch.
 *
 * Contract (docs/ws-binary-router.md §3.2 / docs/protocol.md §3):
 *   - only FIN=1 complete frames enter the router (no fragment aggregation);
 *   - frame length must be >= 4 (header) and <= wt_ws_router_max_payload();
 *   - payload is only valid during the callback, the router does not retain it;
 *   - on any validation/dispatch error answer MSG_BINARY_ERROR.
 * uri_ws does not implement any business logic: it only validates and routes.
 */
int ws_on_binary_data(httpd_req_t *req, ws_msg_t *ws_msg)
{
	httpd_ws_frame_t *ws_pkt = &ws_msg->ws_pkt;
	const size_t hdr_len = sizeof(wt_bin_data_hdr_t);
	const size_t max_payload = wt_ws_router_max_payload();
	int err;

	/* Always drain the frame payload first so the httpd WS stream stays in
	 * sync even when the frame is rejected below. */
	err = httpd_ws_recv_frame(req, ws_pkt, ws_pkt->len);
	if (unlikely(err != ESP_OK)) {
		ESP_LOGE(TAG, "ws recv binary data error: %s", esp_err_to_name(err));
		return ws_on_close(req, ws_pkt, ws_msg);
	}

	if (unlikely(ws_pkt->final != 1 || ws_pkt->len < hdr_len ||
	             max_payload == 0 || ws_pkt->len > max_payload)) {
		ESP_LOGE(TAG, "bad binary frame: final=%d len=%u max=%u",
		         (int)ws_pkt->final, (unsigned)ws_pkt->len, (unsigned)max_payload);
		ws_send_binary_error(req);
		memory_pool_put(ws_msg);
		return ESP_OK;
	}

#if BIRDIEBOT_AUTH_ENABLED
	if (!birdiebot_auth_ws_fd_ok(httpd_req_to_sockfd(req))) {
		ESP_LOGW(TAG, "binary frame from unauthenticated fd %d",
		         (int) httpd_req_to_sockfd(req));
		ws_send_unauthorized(req, ws_pkt->payload[1], -1);
		memory_pool_put(ws_msg);
		return ESP_OK;
	}
#endif

	const uint8_t *frame = ws_pkt->payload;
	int ret = wt_ws_router_rx(frame[0], frame[1], frame[2],
	                          frame + hdr_len, ws_pkt->len - hdr_len);
	if (unlikely(ret != 0)) {
		ESP_LOGW(TAG, "binary frame not dispatched: type=0x%02x module=%u sub=%u len=%u",
		         frame[0], frame[1], frame[2],
		         (unsigned)(ws_pkt->len - hdr_len));
		ws_send_binary_error(req);
	}

	memory_pool_put(ws_msg);
	return ESP_OK;
}

/**
 * @brief WS transport tx port registered with wt_ws_router.
 *
 * Called by the ws_router send task (never from the httpd callback) with a
 * complete binary frame (4B header + payload). The frame buffer is owned by the
 * router and valid for the duration of the call only: httpd_ws_send_frame_async
 * copies it while sending synchronously, so no retention is needed.
 */
static int ws_bin_tx_port(const uint8_t *frame, size_t len)
{
	httpd_ws_frame_t pkt = {
		.type = HTTPD_WS_TYPE_BINARY,
		.final = 1,
		.payload = (uint8_t *)frame, /* httpd_ws_frame_t payload is non-const */
		.len = len,
	};

	/* Data broadcasts go to authorized WS clients only when auth is enabled. */
	ws_broadcast_frame(&pkt, BIRDIEBOT_AUTH_ENABLED ? true : false);
	return 0;
}

int ws_on_socket_open(httpd_req_t *req)
{
	int sock_fd = httpd_req_to_sockfd(req);
	ESP_LOGI(TAG, "ws open: %d", sock_fd);

#if BIRDIEBOT_AUTH_ENABLED
	birdiebot_auth_ws_open(sock_fd);

	if (!birdiebot_auth_ws_origin_ok(req)) {
		/* Host/Origin rejected (DNS-rebinding guard). The HTTP 101 handshake was
		 * already sent by the httpd, so close the session instead of serving. */
		ESP_LOGW(TAG, "ws handshake rejected (Host/Origin), fd %d", sock_fd);
		httpd_sess_trigger_close(req->handle, sock_fd);
		return 1;
	}

	switch (birdiebot_auth_ws_query_ok(req, sock_fd)) {
	case 0:
		ESP_LOGI(TAG, "ws fd %d authorized by ?token=", sock_fd);
		break;
	case -1:
		/* Keep the connection open: the client may still authenticate with the
		 * first-frame SYSTEM/0/AUTH and business requests get code=7 until then. */
		ESP_LOGW(TAG, "ws fd %d presented an invalid ?token=, awaiting AUTH frame", sock_fd);
		break;
	default:
		break; /* no token: pending first-frame AUTH */
	}
#endif /* BIRDIEBOT_AUTH_ENABLED */

	return ws_add_fd(req->handle, sock_fd);
}

/**
 * @param msg must only used as generic ptr, truncated by ws_on_bin()
 * @return
 */
static int ws_on_close(httpd_req_t *req, httpd_ws_frame_t *ws_pkt, void *msg)
{
	/* Read the rest of the CLOSE frame and response */
	/* Please refer to RFC6455 Section 5.5.1 for more details */
	ws_pkt->len = 0;
	ws_pkt->type = HTTPD_WS_TYPE_CLOSE;
	ESP_LOGI(TAG, "ws %d closed", httpd_req_to_sockfd(req));
	ws_rm_fd(httpd_req_to_sockfd(req));
#if BIRDIEBOT_AUTH_ENABLED
	birdiebot_auth_ws_close(httpd_req_to_sockfd(req));
#endif
	/* Use the per-fd send lock: broadcast (heartbeat / ws_router tx task) may
	 * be sending on the same socket from another task right now. */
	int err = ws_send_frame_safe(req->handle, httpd_req_to_sockfd(req), ws_pkt);
	if (err) {
		ESP_LOGE(TAG, "on close %s", esp_err_to_name(err));
	}
	httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
	memory_pool_put(msg);
	return err;
}

static void ws_async_resp(void *arg)
{
	ws_msg_t *req = arg;
	httpd_handle_t hd = req->hd;
	int fd = req->fd;
	int err;

	ESP_LOGI(TAG, "ws async fd : %d", fd);
	err = ws_send_frame_safe(hd, fd, &req->ws_pkt);
	if (unlikely(err)) {
		ESP_LOGE(TAG, "%s", esp_err_to_name(err));
	}
	memory_pool_put(req);
}

void async_send_out_cb(void *arg, int module_status)
{
	ws_msg_t *req = arg;
	cJSON_Delete(req->json.in);
	ESP_LOGI(TAG, "send out %d", module_status);

	if (module_status != API_JSON_OK) {
		req->ws_pkt.payload = req->payload;
		ws_set_err_msg(&req->ws_pkt, module_status);
		int err = ws_send_frame_safe(req->hd, req->fd, &req->ws_pkt);
		if (err) {
			ESP_LOGE(TAG, "send out %s", esp_err_to_name(err));
		}
		goto end;
	}

	int err;
	json_to_text(req);
	err = httpd_queue_work(req->hd, ws_async_resp, req);
	if (likely(err == ESP_OK)) {
		/* msg queued, let callee release the buffer */
		return;
	}

	ESP_LOGE(TAG, "errno: %d, fd %p: %s", errno, req->hd, esp_err_to_name(err));

end:
	/* clean resources */
	memory_pool_put(req);
}

void json_to_text(ws_msg_t *ws_msg)
{
	int err;
	httpd_ws_frame_t *ws_pkt = &ws_msg->ws_pkt;
	/* api function returns something, send it to http client */
	err = !cJSON_PrintPreallocated(ws_msg->json.out, (char *)ws_pkt->payload, PAYLOAD_LEN - 5, 0);
	cJSON_Delete(ws_msg->json.out);
	if (unlikely(err)) {
		ws_pkt->len = strlen(MSG_SEND_JSON_ERROR);
		ws_pkt->payload = (uint8_t *)MSG_SEND_JSON_ERROR;
		ws_pkt->final = 1;
	}
	ws_pkt->len = strlen((char *)ws_pkt->payload);
}


/* Clients array manipulation function
 * */
static inline int8_t ws_add_fd(httpd_handle_t hd, int fd)
{
	int8_t ret = 0;

	xSemaphoreTake(ws_ctx.list_lock.mutex, portMAX_DELAY);
	if (ws_ctx.client_count >= CONFIG_LWIP_MAX_SOCKETS) {
		ret = 1;
		goto out;
	}

	uint8_t idx = GET_FD_IDX(fd);
	ws_ctx.clients[idx].fd = fd;
	ws_ctx.clients[idx].hd = hd;
	ws_ctx.valid_fd[ws_ctx.client_count] = fd;
	ws_ctx.client_count++;
out:
	xSemaphoreGive(ws_ctx.list_lock.mutex);
	return ret;
}

/**
 * @brief replace the fd and hd to be removed by the last item
 * all front items are valid
 */
static inline void ws_rm_fd(int fd)
{
	xSemaphoreTake(ws_ctx.list_lock.mutex, portMAX_DELAY);
	for (int i = 0; i < ws_ctx.client_count; ++i) {
		if (ws_ctx.valid_fd[i] != fd) {
			continue;
		}
		ws_ctx.valid_fd[i] = ws_ctx.valid_fd[ws_ctx.client_count - 1];
		ws_ctx.client_count--;
		break;
	}
	xSemaphoreGive(ws_ctx.list_lock.mutex);
}

static inline int ws_send_frame_safe(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame)
{
	int err;
	xSemaphoreTake(ws_ctx.lock[GET_FD_IDX(fd)].mutex, portMAX_DELAY);
	err = httpd_ws_send_frame_async(hd, fd, frame);
	xSemaphoreGive(ws_ctx.lock[GET_FD_IDX(fd)].mutex);
	return err;
}

/**
 * @brief Send one frame to every connected WS client.
 *
 * Takes a snapshot of the fd list under list_lock, then sends outside of the
 * lock (sending can block on slow sockets). Dead clients are dropped from the
 * list and their sockets closed, mirroring the previous heartbeat behaviour.
 * Any task (heartbeat / ws_router tx task) may call this.
 */
static void ws_broadcast_frame(httpd_ws_frame_t *pkt, bool authed_only)
{
	uint8_t fds[CONFIG_LWIP_MAX_SOCKETS];
	httpd_handle_t hds[CONFIG_LWIP_MAX_SOCKETS];
	int count;

	xSemaphoreTake(ws_ctx.list_lock.mutex, portMAX_DELAY);
	count = ws_ctx.client_count;
	for (int i = 0; i < count; ++i) {
		fds[i] = ws_ctx.valid_fd[i];
		hds[i] = ws_ctx.clients[GET_FD_IDX(fds[i])].hd;
	}
	xSemaphoreGive(ws_ctx.list_lock.mutex);

	for (int i = 0; i < count; ++i) {
		/* Auth is enabled: do not leak data broadcasts (e.g. UART RX) to
		 * sockets that did not present the pairing token. */
		if (authed_only && !birdiebot_auth_ws_fd_ok(fds[i])) {
			continue;
		}
		int err = ws_send_frame_safe(hds[i], fds[i], pkt);
		if (unlikely(err)) {
			ESP_LOGE(TAG, "broadcast send err (fd %d): %s", fds[i], esp_err_to_name(err));
			ws_rm_fd(fds[i]);
#if BIRDIEBOT_AUTH_ENABLED
			birdiebot_auth_ws_close(fds[i]);
#endif
			httpd_sess_trigger_close(hds[i], fds[i]);
		}
	}
}

static inline void ws_broadcast_heartbeat()
{
	static httpd_ws_frame_t ws_pkt = {
		.len = 0,
		.payload = NULL,
		.type = HTTPD_WS_TYPE_TEXT,
		.final = 1,
	};

	ws_broadcast_frame(&ws_pkt, false);
}

_Noreturn
void heartbeat_task(void *arg)
{
	(void)arg;
	while (1) {
		vTaskDelay(pdMS_TO_TICKS(1500));
		ws_broadcast_heartbeat();
	}
};


/**
 * REGISTER MODULE
 * */

static const httpd_uri_t uri_api = {
	.uri       = "/ws",
	.method    = HTTP_GET,
	.handler   = ws_req_handler,
	.user_ctx  = NULL,
	.is_websocket = true,
	.supported_subprotocol = NULL,
	.handle_ws_control_frames = true,
};

static int WS_REQ_INIT(const httpd_uri_t **uri_conf)
{
	*uri_conf = &uri_api;

	ws_ctx.client_count = 0;
	ws_ctx.list_lock.mutex = xSemaphoreCreateMutexStatic(&ws_ctx.list_lock.xMutexBuffer);
	for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
		ws_ctx.lock[i].mutex = xSemaphoreCreateMutexStatic(&ws_ctx.lock[i].xMutexBuffer);
	}

	/* Register the WS binary send port before the server starts handling frames
	 * (docs/ws-binary-router.md §2: must be called before start_webserver()). */
	wt_ws_router_set_tx_port(ws_bin_tx_port);

	xTaskCreate(heartbeat_task, "hb task", 2048, NULL, 3, &ws_ctx.task_heartbeat);
	return 0;
}

static int WS_REQ_EXIT(const httpd_uri_t **uri_conf)
{
	*uri_conf = &uri_api;
	wt_ws_router_set_tx_port(NULL);
	vTaskDelete(ws_ctx.task_heartbeat);
	ws_ctx.task_heartbeat = NULL;
	return 0;
}

WEB_URI_MODULE_REGISTER(101, WS_REQ_INIT, WS_REQ_EXIT)
