/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file uri_ota.c
 * @brief OTA local upload endpoint: POST /api/ota/upload (WP-206).
 *
 * Contract: docs/ota-local-upload.md §3, docs/protocol.md §2.5/§1.1.
 * The OTA core (state machine, flash write, verification, module 5 JSON) lives
 * in components/wt_ota (WP-205); this file only implements the HTTP transport:
 *
 *   httpd task (URI handler)                    OTA task
 *   ------------------------                    --------
 *   Host/Origin / Content-Type / Content-Length /   wt_ota_upload_begin()
 *   size checks (no flash erase on errors)          (erase target slot)
 *   DAP ring buffers released (task-66b)       then, per chunk received from
 *   httpd_req_async_handler_begin()  ------>   the httpd task:
 *   recv body chunk by chunk (static                wt_ota_upload_write()
 *   double buffer, hand-off queue)  ------>    wt_ota_upload_finish()
 *   return ESP_OK (body fully consumed)        send HTTP 200 JSON (async req)
 *                                              wt_ota_apply_and_reboot(1000)
 *
 * task-66b: the unified Wi-Fi+USB-NCM image keeps the DAP ring buffers resident
 * (~41 KiB); with them allocated heap_largest is ~4.6 KiB, below the 5120-byte
 * OTA task stack, and every upload used to answer 503 "ota service
 * unavailable".  The handler therefore releases the rings with
 * dap_handle_release_ringbuffers() before deferring the request; a failed
 * upload restores them (dap_handle_restore_ringbuffers()), a successful one
 * reboots the device.
 * Why the body is received in the httpd task (deviation from the literal
 * "recv in the OTA task" wording of docs/ota-local-upload.md §3.2, which the
 * ESP-IDF 5.2 async-request API cannot provide safely): `httpd_req_async_handler
 * _begin()` only defers the *response*. After the handler returns,
 * httpd_req_delete() drains any unconsumed request body in the httpd task
 * (httpd_parse.c), so a second task calling httpd_req_recv() would race with
 * that drain on the same socket. Only the slow operations (flash erase/write/
 * verify and the delayed response) run in the OTA task, i.e. the httpd task
 * performs no flash I/O. A side effect is that other HTTP/WS requests (e.g.
 * OTA/GET_PROGRESS) are only serviced between upload chunks; the browser-side
 * upload progress bar is driven by XHR upload events and is not affected.
 */
#include "web_uri_module.h"
#include "web_host_guard.h"

#include "wt_ota.h"
#include "DAP_handle.h"

#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TAG __FILE_NAME__

/* Kconfig BIRDIEBOT_OTA_BUF_SIZE comes from components/wt_ota; the fallback
 * keeps host syntax checks / stale sdkconfig compiling (default is 4096). */
#ifndef CONFIG_BIRDIEBOT_OTA_BUF_SIZE
#define CONFIG_BIRDIEBOT_OTA_BUF_SIZE 4096
#endif

#define OTA_UPLOAD_URI         "/api/ota/upload"
#define OTA_UPLOAD_BUF_SIZE    CONFIG_BIRDIEBOT_OTA_BUF_SIZE
#define OTA_UPLOAD_BUF_COUNT   2
#if defined(CONFIG_IDF_TARGET_ESP32C5)
/* ESP32-C5 real hardware: the internal heap is much smaller than the S3's.
 * After ota_dap_release_rings() + httpd_req_async_handler_begin() the largest
 * internal block was 4096 B, so xTaskCreate(OTA_UPLOAD_TASK_STACK=5120) failed
 * and every upload answered 503 "ota service unavailable" (measured
 * 2026-10-25).  3584 B is enough for recv + esp_ota_write + SHA-256 + JSON
 * and fits the 4096 B block.  Keep 5120 on the S3/S3-class targets. */
#define OTA_UPLOAD_TASK_STACK  3584
#else
#define OTA_UPLOAD_TASK_STACK  5120
#endif
#define OTA_UPLOAD_TASK_PRIO   5
#define OTA_REBOOT_DELAY_MS    1000
#define OTA_JSON_RESP_MAX      640

/* Stable English error strings from docs/ota-local-upload.md §3.4. */
#define OTA_ERR_BAD_SHA256         "bad sha256 header"
#define OTA_ERR_IMAGE_TOO_SMALL    "image too small"
#define OTA_ERR_IMAGE_TOO_LARGE    "image too large"
#define OTA_ERR_CONTENT_LENGTH_REQ "content-length required"
#define OTA_ERR_UNSUPPORTED_TYPE   "unsupported content type"
#define OTA_ERR_BUSY               "ota session busy"
#define OTA_ERR_UNAVAILABLE        "ota service unavailable"
#define OTA_ERR_FORBIDDEN          "forbidden origin"
#define OTA_ERR_TIMEOUT            "upload timeout"
#define OTA_ERR_DISCONNECTED       "client disconnected"

/* JSON code (docs/protocol.md §1.1). */
#define OTA_JSON_CODE_PROPERTY 5
#define OTA_JSON_CODE_BUSY     6
#define OTA_JSON_CODE_INTERNAL 3

#define OTA_JSON_MODULE 5
#define OTA_JSON_CMD    2 /* upload association, docs/ota-local-upload.md §3.4 */
#define OTA_SLOT_EOS    0xFF

typedef struct {
	uint8_t slot; /* index into s_rx_bufs[] / OTA_SLOT_EOS marker */
	uint32_t len;
} ota_rx_item_t;

typedef enum {
	OTA_RECV_OK = 0,
	OTA_RECV_DISCONNECTED,
	OTA_RECV_TIMEOUT,
	OTA_RECV_INTERNAL,
} ota_recv_result_t;

typedef struct {
	httpd_req_t *async_req;  /* request copy for the deferred response */
	uint32_t content_len;
	char sha256[WT_OTA_SHA256_STR_LEN]; /* "" = no X-SHA256 check */
	ota_recv_result_t recv_result;
	bool req_handled_by_handler; /* body transfer aborted: handler owns the req */
} ota_upload_ctx_t;

static uint8_t s_rx_bufs[OTA_UPLOAD_BUF_COUNT][OTA_UPLOAD_BUF_SIZE];
static QueueHandle_t s_free_queue; /* free buffer slots (ota_rx_item_t) */
static QueueHandle_t s_work_queue; /* filled chunks (ota_rx_item_t) */
static StaticQueue_t s_free_queue_buf;
static StaticQueue_t s_work_queue_buf;
static uint8_t s_free_queue_storage[OTA_UPLOAD_BUF_COUNT * sizeof(ota_rx_item_t)];
static uint8_t s_work_queue_storage[OTA_UPLOAD_BUF_COUNT * sizeof(ota_rx_item_t)];
static char s_response[OTA_JSON_RESP_MAX];
static ota_upload_ctx_t s_ctx;
static volatile bool s_upload_active;

static esp_err_t ota_upload_handler(httpd_req_t *req);
static void ota_upload_task(void *arg);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void ota_json_escape(char *dst, size_t dst_len, const char *src)
{
	size_t o = 0;

	if (dst_len == 0) {
		return;
	}
	for (const char *p = src; p != NULL && *p != '\0' && o + 1 < dst_len; ++p) {
		char c = *p;
		if (c == '"' || c == '\\') {
			if (o + 2 >= dst_len) {
				break;
			}
			dst[o++] = '\\';
			dst[o++] = c;
		} else if ((unsigned char) c < 0x20) {
			continue; /* drop control characters */
		} else {
			dst[o++] = c;
		}
	}
	dst[o] = '\0';
}

static const char *ota_http_status_str(int status)
{
	switch (status) {
	case 400: return "400 Bad Request";
	case 403: return "403 Forbidden";
	case 408: return "408 Request Timeout";
	case 409: return "409 Conflict";
	case 411: return "411 Length Required";
	case 413: return "413 Payload Too Large";
	case 415: return "415 Unsupported Media Type";
	case 422: return "422 Unprocessable Entity";
	case 503: return "503 Service Unavailable";
	default:  return "500 Internal Server Error";
	}
}

static void ota_respond_error(httpd_req_t *req, int http_status, int json_code, const char *err_text)
{
	char escaped[WT_OTA_ERR_MAX_LEN * 2 + 1];

	ota_json_escape(escaped, sizeof(escaped), err_text != NULL ? err_text : "internal error");
	int n = snprintf(s_response, sizeof(s_response),
	                 "{\"module\":%d,\"cmd\":%d,\"ok\":false,\"err\":\"%s\",\"code\":%d}",
	                 OTA_JSON_MODULE, OTA_JSON_CMD, escaped, json_code);
	if (n <= 0) {
		return;
	}
	if (n >= (int) sizeof(s_response)) {
		n = (int) sizeof(s_response) - 1;
	}
	httpd_resp_set_status(req, ota_http_status_str(http_status));
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, s_response, (size_t) n);
}

/**
 * @brief HTTP 200 upload response (docs/ota-local-upload.md §3.4).
 *
 * The snapshot is taken after wt_ota_upload_finish() succeeds, i.e. status is
 * READY (image verified, boot slot switched) and the reboot follows 1000 ms
 * after this response has been sent.
 */
static void ota_respond_success(httpd_req_t *req)
{
	wt_ota_info_t info;
	wt_ota_progress_t progress;

	wt_ota_get_info(&info);
	wt_ota_get_progress(&progress);

	int n = snprintf(s_response, sizeof(s_response),
	                 "{\"module\":%d,\"cmd\":%d,\"ok\":true,"
	                 "\"slot\":\"%s\",\"running\":\"%s\",\"running_ver\":\"%s\","
	                 "\"offset\":%u,\"total\":%u,\"sha256\":\"%s\","
	                 "\"status\":\"%s\",\"reboot_in_ms\":%u}",
	                 OTA_JSON_MODULE, OTA_JSON_CMD,
	                 progress.slot, info.running, info.running_ver,
	                 (unsigned) progress.offset, (unsigned) progress.total,
	                 progress.sha256,
	                 wt_ota_status_to_str(progress.status),
	                 (unsigned) OTA_REBOOT_DELAY_MS);
	if (n <= 0) {
		return;
	}
	if (n >= (int) sizeof(s_response)) {
		n = (int) sizeof(s_response) - 1;
	}
	httpd_resp_set_status(req, "200 OK");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, s_response, (size_t) n);
}

static bool ota_read_header(httpd_req_t *req, const char *name, char *buf, size_t buf_len)
{
	size_t value_len = httpd_req_get_hdr_value_len(req, name);
	if (value_len == 0 || value_len >= buf_len) {
		return false;
	}
	return httpd_req_get_hdr_value_str(req, name, buf, buf_len) == ESP_OK;
}

static bool ota_sha256_header_valid(const char *value)
{
	if (strlen(value) != WT_OTA_SHA256_HEX_LEN) {
		return false;
	}
	for (const char *p = value; *p != '\0'; ++p) {
		char c = *p;
		bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!hex) {
			return false;
		}
	}
	return true;
}

static void ota_free_slot(uint8_t slot)
{
	ota_rx_item_t item = {
		.slot = slot,
		.len = 0,
	};
	(void) xQueueSend(s_free_queue, &item, 0);
}

/* ------------------------------------------------------------------------- */
/* DAP ring-buffer hand-off for the upload (task-66b)                        */
/* ------------------------------------------------------------------------- */
/*
 * Why this exists: the unified Wi-Fi+USB-NCM image keeps the two DAP ring
 * buffers (request 20,480 B + response 20,672 B) resident for the whole boot
 * after the task-66 "CMD_INFO" fix.  With them allocated the device reports
 * heap_free≈17.8 KiB / heap_largest≈4.6 KiB, and this handler's
 * xTaskCreate(.., OTA_UPLOAD_TASK_STACK=5120, ..) cannot allocate its stack:
 * every upload answered 503 "ota service unavailable" (found on real hardware
 * 2026-10-25 after OTA of the task-66 candidate).
 *
 * The upload is a front-end to a reboot, so the DAP data path may be dropped:
 * release the ~41 KiB before deferring the request / creating the OTA task and
 * restore the rings only if the upload fails without rebooting (a successful
 * upload reboots, and the next boot re-creates them in DAP_Thread).
 * GET /api/target/status exposes the transient state as dap_ring_active=false.
 */
#define OTA_DAP_RELEASE_TIMEOUT_MS 500
#define OTA_DAP_RESTORE_TIMEOUT_MS 1500

static void ota_dap_release_rings(void)
{
	esp_err_t err = dap_handle_release_ringbuffers(OTA_DAP_RELEASE_TIMEOUT_MS);
	if (err == ESP_OK) {
		ESP_LOGI(TAG, "DAP ring buffers released for OTA upload "
		         "(heap_free=%u largest=%u B)",
		         (unsigned) esp_get_free_heap_size(),
		         (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
		                                                     MALLOC_CAP_INTERNAL));
	} else {
		/* Best effort: continue and let the normal OTA path report its own
		 * failure (with the old behaviour the upload answered 503 here). */
		ESP_LOGW(TAG, "DAP ring buffer release failed: %s (continuing)",
		         esp_err_to_name(err));
	}
}

static void ota_dap_restore_rings(void)
{
	esp_err_t err = dap_handle_restore_ringbuffers(OTA_DAP_RESTORE_TIMEOUT_MS);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "DAP ring buffer restore failed: %s "
		         "(USBIP DAP path unavailable until reboot)",
		         esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "DAP ring buffers restored after aborted OTA upload");
	}
}

/**
 * @brief Complete the async request and optionally close the TCP session.
 *
 * Called exactly once per upload: by the OTA task on the normal path, or by
 * the httpd handler when the body transfer was aborted (recv error).
 */
static void ota_complete_request(httpd_req_t *async_req, bool close_socket)
{
	httpd_handle_t hd = async_req->handle;
	int fd = httpd_req_to_sockfd(async_req);

	httpd_req_async_handler_complete(async_req);
	if (close_socket && fd >= 0) {
		httpd_sess_trigger_close(hd, fd);
	}
}

/* ------------------------------------------------------------------------- */
/* HTTP handler (httpd task): validate, claim, receive body, hand off chunks */
/* ------------------------------------------------------------------------- */

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
	char content_type[128];
	char content_length_header[32];
	char sha256_header[WT_OTA_SHA256_STR_LEN + 8];
	char transfer_encoding[32];
	wt_ota_info_t info;

	/* Browser Host/Origin guard (CSWSH / DNS rebinding; always active, this is
	 * not a login check) - checked BEFORE any flash erase is triggered, so a
	 * cross-origin / DNS-rebinding request can never start an upload. */
	if (!web_host_guard_http_ok(req)) {
		ota_respond_error(req, 403, OTA_JSON_CODE_PROPERTY, OTA_ERR_FORBIDDEN);
		return ESP_OK;
	}

	/* Content-Type: application/octet-stream (strict prefix, §3.1). */
	if (!ota_read_header(req, "Content-Type", content_type, sizeof(content_type)) ||
	    strncasecmp(content_type, "application/octet-stream", 24) != 0) {
		ota_respond_error(req, 415, OTA_JSON_CODE_PROPERTY, OTA_ERR_UNSUPPORTED_TYPE);
		return ESP_OK;
	}

	/* Content-Length is mandatory; chunked transfer encoding is rejected. */
	if (ota_read_header(req, "Transfer-Encoding", transfer_encoding, sizeof(transfer_encoding)) ||
	    !ota_read_header(req, "Content-Length", content_length_header, sizeof(content_length_header)) ||
	    req->content_len == 0) {
		ota_respond_error(req, 411, OTA_JSON_CODE_PROPERTY, OTA_ERR_CONTENT_LENGTH_REQ);
		return ESP_OK;
	}

	if (req->content_len < WT_OTA_MIN_IMAGE_SIZE) {
		ota_respond_error(req, 400, OTA_JSON_CODE_PROPERTY, OTA_ERR_IMAGE_TOO_SMALL);
		return ESP_OK;
	}

	/* Size checks against the target slot before touching flash. 4 MB
	 * single-app boards report slot_size 0 -> OTA service unavailable. */
	wt_ota_get_info(&info);
	if (info.slot_size == 0) {
		ota_respond_error(req, 503, OTA_JSON_CODE_BUSY, OTA_ERR_UNAVAILABLE);
		return ESP_OK;
	}
	if (req->content_len > info.slot_size || req->content_len > WT_OTA_SLOT_SIZE_MAX) {
		ota_respond_error(req, 413, OTA_JSON_CODE_PROPERTY, OTA_ERR_IMAGE_TOO_LARGE);
		return ESP_OK;
	}

	/* Optional X-SHA256: 64 hex chars when present. */
	bool has_sha256 = false;
	if (ota_read_header(req, "X-SHA256", sha256_header, sizeof(sha256_header))) {
		if (!ota_sha256_header_valid(sha256_header)) {
			ota_respond_error(req, 400, OTA_JSON_CODE_PROPERTY, OTA_ERR_BAD_SHA256);
			return ESP_OK;
		}
		has_sha256 = true;
	}

	if (s_free_queue == NULL || s_work_queue == NULL) {
		ESP_LOGE(TAG, "ota unavailable: queues free=%p work=%p",
		         (void *)s_free_queue, (void *)s_work_queue);
		ota_respond_error(req, 503, OTA_JSON_CODE_BUSY, OTA_ERR_UNAVAILABLE);
		return ESP_OK;
	}

	/* Single-session semantics (§3.1): active session -> 409. The httpd task is
	 * single-threaded, so the claim cannot race with another handler. */
	if (wt_ota_is_busy() || s_upload_active) {
		ota_respond_error(req, 409, OTA_JSON_CODE_BUSY, OTA_ERR_BUSY);
		return ESP_OK;
	}
	s_upload_active = true;
	memset(&s_ctx, 0, sizeof(s_ctx));
	s_ctx.content_len = req->content_len;
	s_ctx.recv_result = OTA_RECV_OK;
	if (has_sha256) {
		/* ota_sha256_header_valid() above guarantees exactly
		 * WT_OTA_SHA256_HEX_LEN characters; copy with an explicit bound so
		 * GCC 14's -Werror=format-truncation (source buffer is deliberately
		 * larger for the "sha256=" prefix) stays quiet, while still never
		 * overflowing the destination. */
		size_t sha_len = strnlen(sha256_header, sizeof(s_ctx.sha256) - 1U);
		memcpy(s_ctx.sha256, sha256_header, sha_len);
		s_ctx.sha256[sha_len] = '\0';
	}

	/* task-66b: give the ~41 KiB of the DAP ring buffers back BEFORE the OTA
	 * task stack is allocated (with the rings resident heap_largest is too
	 * small -> xTaskCreate fails -> every upload answered 503 "ota service
	 * unavailable").  A failed upload restores them in ota_upload_task(); a
	 * successful one reboots the device. */
	ota_dap_release_rings();

	/* The worker first, so it drains the static chunk queue while this handler
	 * receives the body.  The async request copy is created only AFTER the body
	 * transfer: ESP-IDF v5.5's httpd_req_async_handler_begin() zeroes
	 * req->aux->remaining_len ("prevent the main thread from reading the rest
	 * of the request"), so calling it before httpd_req_recv() makes the recv
	 * return 0 immediately (real C5 finding 2026-10-25: upload stalled at
	 * "session error: client disconnected", HTTP 400 from the leftover body).
	 * IDF v5.2.1 did not do that, hence the old order worked before the
	 * unified v5.5.6 upgrade. */
	if (xTaskCreate(ota_upload_task, "ota_upload", OTA_UPLOAD_TASK_STACK, NULL,
	                OTA_UPLOAD_TASK_PRIO, NULL) != pdPASS) {
		ESP_LOGE(TAG, "ota upload task create failed (stack=%u heap free=%u "
		         "largest=%u)", (unsigned)OTA_UPLOAD_TASK_STACK,
		         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
		                                                    MALLOC_CAP_8BIT));
		s_upload_active = false;
		ota_dap_restore_rings();
		ota_respond_error(req, 503, OTA_JSON_CODE_BUSY, OTA_ERR_UNAVAILABLE);
		return ESP_OK;
	}

	/* Receive the declared body and hand the chunks to the OTA task. */
	uint32_t remaining = req->content_len;
	while (remaining > 0) {
		ota_rx_item_t slot_item;
		if (xQueueReceive(s_free_queue, &slot_item, portMAX_DELAY) != pdTRUE) {
			s_ctx.recv_result = OTA_RECV_INTERNAL;
			break;
		}
		uint8_t slot = slot_item.slot;
		size_t want = remaining > OTA_UPLOAD_BUF_SIZE ? OTA_UPLOAD_BUF_SIZE : remaining;
		int received = httpd_req_recv(req, (char *) s_rx_bufs[slot], want);
		if (received == HTTPD_SOCK_ERR_TIMEOUT) {
			ota_free_slot(slot);
			s_ctx.recv_result = OTA_RECV_TIMEOUT;
			break;
		}
		if (received <= 0) {
			ESP_LOGW(TAG, "ota recv failed: %d (remaining=%u heap_largest=%u)",
			         received, (unsigned)remaining,
			         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
			                                                    MALLOC_CAP_8BIT));
			ota_free_slot(slot);
			s_ctx.recv_result = OTA_RECV_DISCONNECTED;
			break;
		}

		ota_rx_item_t work = {
			.slot = slot,
			.len = (uint32_t) received,
		};
		if (xQueueSend(s_work_queue, &work, portMAX_DELAY) != pdTRUE) {
			ota_free_slot(slot);
			s_ctx.recv_result = OTA_RECV_INTERNAL;
			break;
		}
		remaining -= (uint32_t) received;
	}

	if (s_ctx.recv_result != OTA_RECV_OK) {
		/* Body transfer aborted: reply synchronously (no async copy exists) and
		 * let the OTA task only unwind / restore the DAP rings. */
		s_ctx.req_handled_by_handler = true;
		if (s_ctx.recv_result == OTA_RECV_TIMEOUT) {
			ota_respond_error(req, 408, OTA_JSON_CODE_BUSY, OTA_ERR_TIMEOUT);
		}
	} else {
		esp_err_t begin_err = httpd_req_async_handler_begin(req, &s_ctx.async_req);
		if (begin_err != ESP_OK) {
			ESP_LOGE(TAG, "ota async handler begin failed: %s (heap free=%u "
			         "largest=%u)", esp_err_to_name(begin_err),
			         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
			         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
			                                                    MALLOC_CAP_8BIT));
			s_ctx.req_handled_by_handler = true;
			ota_respond_error(req, 503, OTA_JSON_CODE_BUSY, OTA_ERR_UNAVAILABLE);
		}
	}

	/* End-of-stream marker: the OTA task performs the slow part from here. */
	ota_rx_item_t eos = {
		.slot = OTA_SLOT_EOS,
		.len = 0,
	};
	(void) xQueueSend(s_work_queue, &eos, portMAX_DELAY);

	return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* OTA task: flash erase/write/verify + final response + reboot              */
/* ------------------------------------------------------------------------- */

static void ota_upload_task(void *arg)
{
	(void) arg;

	/* The async request copy is created by the httpd handler only after the
	 * body has been received (IDF v5.5 zeroes remaining_len in
	 * httpd_req_async_handler_begin), which happens before the EOS marker is
	 * queued.  Fetch it after the drain loop instead of at task start. */
	httpd_req_t *async_req = NULL;
	esp_err_t err = wt_ota_upload_begin(s_ctx.content_len);
	uint32_t written = 0;

	/* Always drain the hand-off queue up to the EOS marker so the httpd task
	 * can never block on a full queue while unwinding after an error. */
	for (;;) {
		ota_rx_item_t item;
		if (xQueueReceive(s_work_queue, &item, portMAX_DELAY) != pdTRUE) {
			continue;
		}
		if (item.slot == OTA_SLOT_EOS) {
			break;
		}
		if (err == ESP_OK) {
			esp_err_t write_err = wt_ota_upload_write(s_rx_bufs[item.slot], item.len);
			if (write_err != ESP_OK) {
				ESP_LOGE(TAG, "ota write failed: %s", esp_err_to_name(write_err));
				err = write_err;
			} else {
				written += item.len;
				wt_ota_poll(); /* enforce idle/total upload timeouts (wt_ota) */
				if (wt_ota_get_status() == WT_OTA_STATUS_ERROR) {
					err = ESP_ERR_TIMEOUT;
				}
			}
		}
		ota_free_slot(item.slot);
	}

	/* Set by the httpd handler before it queued the EOS marker (normal path),
	 * NULL when the body transfer aborted and the handler replied itself. */
	async_req = s_ctx.async_req;

	if (s_ctx.req_handled_by_handler) {
		/* Body transfer aborted; the handler already replied (408) or the
		 * client is gone. Only record diagnostics / abort the slot. */
		if (s_ctx.recv_result == OTA_RECV_TIMEOUT) {
			(void) wt_ota_upload_fail(OTA_ERR_TIMEOUT);
		} else if (s_ctx.recv_result == OTA_RECV_DISCONNECTED) {
			(void) wt_ota_upload_fail(OTA_ERR_DISCONNECTED);
		} else if (err == ESP_OK) {
			(void) wt_ota_upload_fail("internal error");
		}
		ota_dap_restore_rings(); /* no reboot: USBIP DAP path comes back */
		s_upload_active = false;
		vTaskDelete(NULL);
		return;
	}

	if (err == ESP_OK && written != s_ctx.content_len) {
		err = ESP_ERR_INVALID_SIZE;
	}
	if (err == ESP_OK) {
		err = wt_ota_upload_finish(s_ctx.sha256[0] != '\0' ? s_ctx.sha256 : NULL);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "ota verify failed: %s", wt_ota_err_to_str(err));
			(void) wt_ota_upload_fail(wt_ota_err_to_str(err));
		}
	}

	if (err != ESP_OK) {
		ota_respond_error(async_req, wt_ota_err_to_http_status(err),
		                  wt_ota_err_to_json_code(err), wt_ota_err_to_str(err));
		ota_complete_request(async_req, true);
		/* task-66b: the upload failed without a reboot, so the DAP data path
		 * must come back (the rings were released before this task started). */
		ota_dap_restore_rings();
		s_upload_active = false;
		vTaskDelete(NULL);
		return;
	}

	/* Success: reply first, then schedule the reboot 1000 ms later (§3.4). */
	ota_respond_success(async_req);
	ota_complete_request(async_req, true);
	s_upload_active = false;

	esp_err_t apply_err = wt_ota_apply_and_reboot(OTA_REBOOT_DELAY_MS);
	if (apply_err != ESP_OK) {
		ESP_LOGE(TAG, "ota apply failed: %s", esp_err_to_name(apply_err));
		/* No reboot scheduled: bring the released DAP ring buffers back. */
		ota_dap_restore_rings();
	}

	/* Worker task must not return (FreeRTOS abort); success reboots via timer. */
	vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* URI module registration (smaller priority = registered/checked earlier)   */
/* ------------------------------------------------------------------------- */

static const httpd_uri_t uri_ota = {
	.uri      = OTA_UPLOAD_URI,
	.method   = HTTP_POST,
	.handler  = ota_upload_handler,
	.user_ctx = NULL,
};

static int OTA_UPLOAD_INIT(const httpd_uri_t **uri_conf)
{
	*uri_conf = &uri_ota;

	if (s_free_queue == NULL || s_work_queue == NULL) {
		s_free_queue = xQueueCreateStatic(OTA_UPLOAD_BUF_COUNT, sizeof(ota_rx_item_t),
		                                  s_free_queue_storage, &s_free_queue_buf);
		s_work_queue = xQueueCreateStatic(OTA_UPLOAD_BUF_COUNT, sizeof(ota_rx_item_t),
		                                  s_work_queue_storage, &s_work_queue_buf);
		if (s_free_queue == NULL || s_work_queue == NULL) {
			ESP_LOGE(TAG, "failed to create OTA upload queues");
		} else {
			for (uint8_t i = 0; i < OTA_UPLOAD_BUF_COUNT; ++i) {
				ota_rx_item_t item = {
					.slot = i,
					.len = 0,
				};
				(void) xQueueSend(s_free_queue, &item, 0);
			}
		}
	}

	ESP_LOGI(TAG, "OTA upload endpoint registered: POST %s", OTA_UPLOAD_URI);
	return 0;
}

static int OTA_UPLOAD_EXIT(const httpd_uri_t **uri_conf)
{
	*uri_conf = &uri_ota;
	return 0;
}

WEB_URI_MODULE_REGISTER(0x82, OTA_UPLOAD_INIT, OTA_UPLOAD_EXIT);
