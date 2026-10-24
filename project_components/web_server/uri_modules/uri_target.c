/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file uri_target.c
 * @brief Web-driven direct flashing of a target MCU (STM32) over SWD.
 *
 * The browser uploads a raw target firmware image (`.bin`); the ESP32 streams
 * it over the existing CMSIS-DAP engine to the target flash while the body is
 * being received (no staging partition, no PC/OpenOCD involved).  The flash
 * algorithm lives in `components/target_flash` (dap-dev); this file is only the
 * HTTP transport + JSON envelope:
 *
 *   GET  /api/target/info    -> probe the SWD target (module 6, cmd 1)
 *   POST /api/target/flash   -> octet-stream upload + flash (module 6, cmd 2)
 *   GET  /api/target/status   -> engine status snapshot (module 6, cmd 3)
 *   POST /api/target/reset    -> optional {"run":bool} SWD reset (module 6, cmd 4)
 *   GET  /api/discover        -> nearby DAPs via mDNS _http._tcp (P1-19,
 *                                module 6, cmd 6; registered here BEFORE the
 *                                URI modules so the "/" SPA fallback cannot
 *                                swallow it).  Read-only, blocking <= 2 s,
 *                                one query at a time (429 when busy).
 *
 * GET /api/target/status contract (additive diagnostics; existing fields keep
 * their meaning, HTTP stays 200):
 *   state/total/written/verified/percent/err/message/busy  -- flash engine
 *   heap_free        esp_get_free_heap_size() at snapshot time (bytes)
 *   heap_largest     heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
 *   dap_session_active  read-only tcp_server_session_active() (kSock >= 0);
 *                    never kicks the USBIP/elaphureLink/WS-DAP session
 *   swd_clock_hz     last successful /api/target/info probe clock; omitted
 *                    until a probe has run since boot
 *   swd_clock_actual_hz  P0-8 actual clock of the last successful bring-up
 *                    (auto probe ladder 4M/2M/1M/500k); omitted when none yet
 *   swd_clock_manual_hz  manual clock selection (0 = auto); always present
 *   wifi_connected   P0-8 read-only Wi-Fi station link state (bool)
 *   wifi_rssi        dBm; omitted when the station is not connected
 *   wifi_ssid        connected SSID; omitted when not connected
 *   wifi_channel     primary channel; omitted when not connected
 *   dap_xfer_count/avg_us/max_us/last_us  device-side USBIP<->DAP command
 *                    latency counters (NOT a network RTT; 0 before the first
 *                    transaction)
 *   dap_ring_preallocated / dap_ring_active / dap_ring_request_bytes /
 *   dap_ring_response_bytes  P0-2 early DAP ring reservation state
 *   last_error       flash engine status message while err != 0; omitted
 *                    when the last operation had no error
 *   Fields that could not be measured are OMITTED (never fabricated/null).
 * The field set is consumed by the Web dashboard and scripts/hil_gate.py.
 *
 * POST /api/target/flash contract:
 *   - Host/Origin browser guard first (web_host_guard_http_ok);
 *   - Content-Type: application/octet-stream, mandatory Content-Length
 *     (no chunked transfer), size <= wt_tf_info_t.max_image_size;
 *   - mandatory X-Target-SHA256: 64 hex chars, SHA-256 of the uploaded body
 *     (computed by the browser); the device also computes it while streaming
 *     and rejects a mismatch with HTTP 422 before final verification;
 *   - one flash session at a time (HTTP 409 / JSON code 6);
 *   - wt_tf_flash_begin() -> per-4-KiB chunk wt_tf_flash_feed() ->
 *     wt_tf_flash_end() (flush + readback SHA-256 verify + reset/run).
 *     Errors map to HTTP 400/409/413/415/422/500/503 + JSON code 2/3/5/6.
 *
 * Scheduling note: the body is received in the httpd task (the same reason as
 * uri_ota.c: ESP-IDF 5.2 cannot hand an unread request body to another task
 * safely) and each chunk yields with vTaskDelay(1).  The WS heartbeat runs in
 * its own task, but other HTTP requests are only serviced between chunks and
 * during the (seconds-long) erase/verify phases of the flash engine.  This is
 * acceptable for a lab-only direct-flash flow; the browser drives its progress
 * bar from the XHR upload events.
 *
 * Security: internal/lab network use only.  The web login/token feature was
 * removed by project decision; the browser Host/Origin guard stays enabled.
 * During flashing the DAP engine is owned exclusively by the flash engine, so
 * USBIP / WS-DAP / elaphureLink sessions are paused (the component restores
 * them afterwards).
 */
#include "web_host_guard.h"
#include "web_error_json.h"

#include "wt_target_flash.h"
#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#include "birdiebot_wired_dap.h"
#endif
#include "tcp_server.h"

#include <sdkconfig.h>

#include <cJSON.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <mbedtls/sha256.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TAG __FILE_NAME__

/* JSON module id for target operations (TARGET=6, docs/protocol.md extension;
 * the docs contract update is owned by the Lead). */
#define TARGET_MODULE_ID 6
#define TARGET_CMD_INFO   1
#define TARGET_CMD_FLASH  2
#define TARGET_CMD_STATUS 3
#define TARGET_CMD_RESET  4
#define TARGET_CMD_SWD_CLOCK 5
#define TARGET_CMD_DISCOVER 6

#define TARGET_URI_INFO   "/api/target/info"
#define TARGET_URI_FLASH  "/api/target/flash"
#define TARGET_URI_STATUS "/api/target/status"
#define TARGET_URI_RESET  "/api/target/reset"
#define TARGET_URI_SWD_CLOCK "/api/target/swd-clock"
#define TARGET_URI_DISCOVER "/api/discover"
#define DAP_URI_KICK      "/api/dap/kick"

#define TARGET_CHUNK_SIZE      4096
#define TARGET_JSON_MAX        1536
#define TARGET_SHA256_HEX_LEN  64
#define TARGET_RESET_BODY_MAX  128
/* P0-2 heap guard: under this largest contiguous internal block a SWD
 * bring-up cannot be trusted (fragmented NCM+WiFi heap); answer an explicit
 * 503 instead of letting /api/target/info end in a silent SWD timeout. */
#define TARGET_HEAP_GUARD_BYTES 4096U
#define TARGET_UID_HEX_LEN     (2 * 12 + 1)
/* P1-19 discovery response: 8 max-length device rows (name/hostname/ip/port +
 * optional target/version, JSON-escaped) plus the envelope.  Worst case is
 * ~3.8 KiB when every string needs escaping; the formatter returns -1 and the
 * handler falls back to {"ok":true,"devices":[]} rather than truncating. */
#define TARGET_DISCOVER_JSON_MAX 4096

/* JSON error codes (docs/protocol.md §1.1); 7 (auth) is not used any more. */
#define TARGET_CODE_BAD_REQUEST 2
#define TARGET_CODE_INTERNAL    3
#define TARGET_CODE_PROPERTY    5
#define TARGET_CODE_BUSY        6

#define TARGET_ERR_FORBIDDEN    "forbidden origin"
#define TARGET_ERR_CONTENT_TYPE "unsupported content type"
#define TARGET_ERR_CONTENT_LEN  "content-length required"
#define TARGET_ERR_BAD_SHA      "bad sha256 header"
#define TARGET_ERR_TOO_LARGE    "image too large"
#define TARGET_ERR_BUSY         "target flash busy"
#define TARGET_ERR_NO_TARGET    "target not detected"
#define TARGET_ERR_SHA_MISMATCH "sha256 mismatch"

#if CONFIG_BIRDIEBOT_TARGET_FLASH_ENABLE

/* Only one flash session at a time (the httpd task is single-threaded; this is
 * a defensive flag and also tells GET /status that an upload is running). */
static volatile bool s_flash_busy;

/* Diagnostics cache: last successful SWD probe snapshot.  Written by the info
 * handler (httpd task) after wt_tf_probe() succeeds and only read by GET
 * /api/target/status; it never drives a DAP/flash decision.  GET /status must
 * stay cheap and must not probe (a probe would take DAP ownership and disturb
 * an active USBIP/WS-DAP session), so the clock is reported from this cache and
 * omitted until the Web dashboard/hil gate has queried target/info at least
 * once since boot. */
static bool s_last_probe_valid;
static uint32_t s_last_probe_swd_hz;

/* Shared JSON/response buffer: handlers run in the single httpd task and one
 * flash session at a time, so a static buffer keeps the stack small. */
static char s_json[TARGET_JSON_MAX];
static uint8_t s_rx_buf[TARGET_CHUNK_SIZE];

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static const char *target_http_status_str(int status)
{
	switch (status) {
	case 200: return "200 OK";
	case 400: return "400 Bad Request";
	case 403: return "403 Forbidden";
	case 408: return "408 Request Timeout";
	case 409: return "409 Conflict";
	case 411: return "411 Length Required";
	case 413: return "413 Payload Too Large";
	case 415: return "415 Unsupported Media Type";
	case 422: return "422 Unprocessable Entity";
	case 429: return "429 Too Many Requests";
	case 503: return "503 Service Unavailable";
	default:  return "500 Internal Server Error";
	}
}

static esp_err_t target_send_error(httpd_req_t *req, int http_status, int cmd,
                                   const char *err_text, int code)
{
	int n = web_error_json_format(s_json, sizeof(s_json), TARGET_MODULE_ID, cmd,
	                              err_text, code);
	if (n <= 0) {
		return ESP_FAIL;
	}
	httpd_resp_set_status(req, target_http_status_str(http_status));
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, s_json, (size_t) n);
}

/** Send a cJSON root (takes ownership); falls back to an error envelope. */
static esp_err_t target_send_json(httpd_req_t *req, int http_status, int cmd, cJSON *root)
{
	bool ok = (root != NULL) && cJSON_PrintPreallocated(root, s_json, sizeof(s_json), 0);
	cJSON_Delete(root);

	if (!ok) {
		return target_send_error(req, 500, cmd, "json generation error", TARGET_CODE_INTERNAL);
	}
	httpd_resp_set_status(req, target_http_status_str(http_status));
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, s_json, strlen(s_json));
}

static bool target_read_header(httpd_req_t *req, const char *name, char *buf, size_t buf_len)
{
	size_t value_len = httpd_req_get_hdr_value_len(req, name);

	if (value_len == 0 || value_len >= buf_len) {
		return false;
	}
	return httpd_req_get_hdr_value_str(req, name, buf, buf_len) == ESP_OK;
}

/* Error mapping towards HTTP/JSON (dap-dev's wt_target_flash error contract). */
static int target_err_http(esp_err_t err)
{
	switch (err) {
	case ESP_OK:               return 200;
	case ESP_ERR_INVALID_STATE:return 409;
	case ESP_ERR_INVALID_SIZE: return 413;
	case ESP_ERR_INVALID_ARG:  return 400;
	case ESP_ERR_NOT_SUPPORTED:return 400;
	case ESP_ERR_NOT_FOUND:    return 422;
	case ESP_ERR_INVALID_CRC:  return 422;
	case ESP_ERR_TIMEOUT:      return 503;
	default:                   return 500;
	}
}

static int target_err_code(esp_err_t err)
{
	switch (err) {
	case ESP_ERR_INVALID_STATE:return TARGET_CODE_BUSY;
	case ESP_ERR_INVALID_SIZE:
	case ESP_ERR_INVALID_ARG:
	case ESP_ERR_NOT_SUPPORTED:
	case ESP_ERR_NOT_FOUND:
	case ESP_ERR_INVALID_CRC:  return TARGET_CODE_PROPERTY;
	case ESP_ERR_TIMEOUT:      return TARGET_CODE_BUSY;
	default:                   return TARGET_CODE_INTERNAL;
	}
}

static esp_err_t target_send_mapped_error(httpd_req_t *req, int cmd, esp_err_t err,
                                          const char *override_text)
{
	char fallback[64];
	const char *text = override_text;

	if (text == NULL) {
		switch (err) {
		case ESP_ERR_NOT_FOUND:    text = TARGET_ERR_NO_TARGET; break;
		case ESP_ERR_INVALID_CRC:  text = TARGET_ERR_SHA_MISMATCH; break;
		case ESP_ERR_INVALID_STATE:text = TARGET_ERR_BUSY; break;
		case ESP_ERR_INVALID_SIZE: text = TARGET_ERR_TOO_LARGE; break;
		case ESP_ERR_TIMEOUT:      text = "target operation timeout"; break;
		case ESP_ERR_INVALID_ARG:  text = "invalid argument"; break;
		case ESP_ERR_NOT_SUPPORTED: text = "target series not supported for flashing"; break;
		default:
			snprintf(fallback, sizeof(fallback), "%s", esp_err_to_name(err));
			text = fallback;
			break;
		}
	}
	return target_send_error(req, target_err_http(err), cmd, text, target_err_code(err));
}

/** State name used by the JSON status contract (no stringify helper in core). */
static const char *target_state_str(wt_tf_state_t state)
{
	switch (state) {
	case WT_TF_IDLE:         return "IDLE";
	case WT_TF_ERASING:      return "ERASING";
	case WT_TF_PROGRAMMING:  return "PROGRAMMING";
	case WT_TF_VERIFYING:    return "VERIFYING";
	case WT_TF_RESETTING:    return "RESETTING";
	case WT_TF_DONE:         return "DONE";
	case WT_TF_ERROR:        return "ERROR";
	default:                 return "UNKNOWN";
	}
}

/** 96-bit UID (12 raw bytes) -> 24 uppercase hex chars. */
static void target_uid_to_hex(const uint8_t uid[12], char out[TARGET_UID_HEX_LEN])
{
	for (size_t i = 0; i < 12; ++i) {
		snprintf(&out[i * 2], 3, "%02X", uid[i]);
	}
}

/** "0x%08x" helper for the JSON hex-string fields. */
static void target_u32_to_hex(uint32_t value, char out[11])
{
	snprintf(out, 11, "0x%08x", (unsigned) value);
}

/** SHA-256 digest -> 64 lowercase hex chars. */
static void target_sha_to_hex(const uint8_t digest[32], char out[TARGET_SHA256_HEX_LEN + 1])
{
	for (size_t i = 0; i < 32; ++i) {
		snprintf(&out[i * 2], 3, "%02x", digest[i]);
	}
}

/** Case-insensitive comparison of a 64-hex SHA-256 string. */
static bool target_sha_equal(const char *a, const char *b)
{
	if (strlen(a) != TARGET_SHA256_HEX_LEN || strlen(b) != TARGET_SHA256_HEX_LEN) {
		return false;
	}
	for (size_t i = 0; i < TARGET_SHA256_HEX_LEN; ++i) {
		if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) {
			return false;
		}
	}
	return true;
}

static bool target_read_sha256_header(httpd_req_t *req, char out[TARGET_SHA256_HEX_LEN + 1])
{
	if (!target_read_header(req, "X-Target-SHA256", out, TARGET_SHA256_HEX_LEN + 1)) {
		return false;
	}
	if (strlen(out) != TARGET_SHA256_HEX_LEN) {
		return false;
	}
	for (size_t i = 0; i < TARGET_SHA256_HEX_LEN; ++i) {
		if (!isxdigit((unsigned char) out[i])) {
			return false;
		}
	}
	return true;
}

/** Keep the TWDT happy while streaming (only when this task is subscribed). */
static void target_yield_and_feed_wdt(void)
{
	if (esp_task_wdt_status(NULL) == ESP_OK) {
		(void) esp_task_wdt_reset();
	}
	vTaskDelay(1);
}

/* ------------------------------------------------------------------------- */
/* GET /api/target/info                                                      */
/* ------------------------------------------------------------------------- */

/** wt_tf_confidence_t -> stable API string (v3 detection enrichment). */
static const char *target_confidence_str(uint8_t confidence)
{
	switch ((wt_tf_confidence_t) confidence) {
	case WT_TF_CONF_FLASH_NARROWED:
		return "flash-narrowed";
	case WT_TF_CONF_GROUP:
		return "group";
	case WT_TF_CONF_UNKNOWN:
	default:
		return "unknown";
	}
}

static esp_err_t target_info_handler(httpd_req_t *req)
{
	wt_tf_info_t info = {0};

	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_INFO, TARGET_ERR_FORBIDDEN,
		                         TARGET_CODE_PROPERTY);
	}

	/* P0-2 heap guard: make an out-of-memory condition an explicit 503 with
	 * numbers instead of a silent SWD timeout.  The probe itself takes the DAP
	 * hand-off and re-creates the USBIP ring wrappers, which is not safe on a
	 * heap with no contiguous block left. */
	size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
	                                                  MALLOC_CAP_INTERNAL);
	if (largest < TARGET_HEAP_GUARD_BYTES) {
		char err_text[96];
		snprintf(err_text, sizeof(err_text),
		         "low heap: largest free block %u B < %u B",
		         (unsigned)largest, (unsigned)TARGET_HEAP_GUARD_BYTES);
		ESP_LOGW(TAG, "target/info refused: %s", err_text);
		return target_send_error(req, 503, TARGET_CMD_INFO, err_text,
		                         TARGET_CODE_INTERNAL);
	}

	esp_err_t err = wt_tf_probe(&info);
	if (err != ESP_OK || !info.detected) {
		/* "No target" is a normal state the UI polls: keep HTTP 200 and report
		 * ok=false so the page can show the wiring/power hint. */
		return target_send_error(req, 200, TARGET_CMD_INFO,
		                         err == ESP_ERR_NOT_FOUND ? TARGET_ERR_NO_TARGET
		                                                  : esp_err_to_name(err),
		                         TARGET_CODE_PROPERTY);
	}

	/* Diagnostics only: remember the SWD clock for GET /api/target/status
	 * (never used for protocol/flash decisions). */
	s_last_probe_swd_hz = info.swd_clock_hz;
	s_last_probe_valid = true;

	char dpidr[11];
	char cpuid[11];
	char idcode[11];
	char flash_base[11];
	char uid[TARGET_UID_HEX_LEN];
	target_u32_to_hex(info.dpidr, dpidr);
	target_u32_to_hex(info.cpuid, cpuid);
	target_u32_to_hex(info.idcode, idcode);
	target_u32_to_hex(info.flash_base, flash_base);
	target_uid_to_hex(info.uid, uid);

	cJSON *root = cJSON_CreateObject();
	if (root != NULL) {
		cJSON_AddNumberToObject(root, "module", TARGET_MODULE_ID);
		cJSON_AddNumberToObject(root, "cmd", TARGET_CMD_INFO);
		cJSON_AddBoolToObject(root, "ok", true);
		cJSON_AddBoolToObject(root, "detected", info.detected);
		cJSON_AddStringToObject(root, "family", info.family);
		cJSON_AddStringToObject(root, "series", info.series);
		cJSON_AddStringToObject(root, "device", info.device);
		cJSON_AddBoolToObject(root, "supported", info.supported);
		cJSON_AddNumberToObject(root, "swd_clock_hz", info.swd_clock_hz);
		/* P0-8: actual bring-up clock + manual selection (0 = auto). */
		cJSON_AddNumberToObject(root, "swd_clock_actual_hz",
		                        (double) wt_tf_get_actual_swd_clock());
		cJSON_AddNumberToObject(root, "swd_clock_manual_hz",
		                        (double) wt_tf_get_swd_clock());
		cJSON_AddStringToObject(root, "dpidr", dpidr);
		cJSON_AddStringToObject(root, "cpuid", cpuid);
		cJSON_AddStringToObject(root, "idcode", idcode);
		cJSON_AddStringToObject(root, "uid", uid);
		cJSON_AddStringToObject(root, "flash_base", flash_base);
		cJSON_AddNumberToObject(root, "flash_size", info.flash_size);
		cJSON_AddNumberToObject(root, "max_image_size", info.max_image_size);
		/* v3 detection enrichment (additive; old fields above unchanged).
		 * DEV_ID only resolves a device group: model_candidates is narrowed
		 * with FSIZE, package/order code still need a user-confirmed profile. */
		cJSON_AddNumberToObject(root, "dev_id", (double) info.dev_id);
		cJSON_AddNumberToObject(root, "flash_kib", (double) info.flash_kib);
		cJSON_AddStringToObject(root, "model_candidates", info.model_candidates);
		cJSON_AddStringToObject(root, "core", info.core);
		cJSON_AddStringToObject(root, "detection_confidence",
		                        target_confidence_str(info.confidence));
	}
	return target_send_json(req, 200, TARGET_CMD_INFO, root);
}

/* ------------------------------------------------------------------------- */
/* GET /api/target/status                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t target_status_handler(httpd_req_t *req)
{
	wt_tf_status_t status = {0};

	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_STATUS, TARGET_ERR_FORBIDDEN,
		                         TARGET_CODE_PROPERTY);
	}

	wt_tf_get_status(&status);

	cJSON *root = cJSON_CreateObject();
	if (root != NULL) {
		cJSON_AddNumberToObject(root, "module", TARGET_MODULE_ID);
		cJSON_AddNumberToObject(root, "cmd", TARGET_CMD_STATUS);
		cJSON_AddBoolToObject(root, "ok", true);
		cJSON_AddStringToObject(root, "state", target_state_str(status.state));
		cJSON_AddNumberToObject(root, "total", status.total);
		cJSON_AddNumberToObject(root, "written", status.written);
		cJSON_AddNumberToObject(root, "verified", status.verified);
		cJSON_AddNumberToObject(root, "percent", status.percent);
		cJSON_AddNumberToObject(root, "err", status.err);
		cJSON_AddStringToObject(root, "message", status.message);
		cJSON_AddBoolToObject(root, "busy", s_flash_busy);

		/* Read-only diagnostics (additive; fields above keep their meaning and
		 * the response stays HTTP 200).  The Web dashboard/HIL gate consumes
		 * these to detect heap exhaustion/fragmentation and proxy sessions
		 * without a UART console. */
		cJSON_AddNumberToObject(root, "heap_free", (double) esp_get_free_heap_size());
		cJSON_AddNumberToObject(root, "heap_largest",
		                        (double) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		/* kSock >= 0 only; tcp_server_session_active() never kicks the
		 * session (unlike POST /api/dap/kick / curve start DAP hand-off). */
		cJSON_AddBoolToObject(root, "dap_session_active", tcp_server_session_active());
#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)
		cJSON_AddBoolToObject(root, "wired_ncm_active", birdiebot_wired_dap_ncm_active());
		cJSON_AddBoolToObject(root, "wired_dhcp_active", birdiebot_wired_dap_dhcp_active());
		cJSON_AddNumberToObject(root, "wired_dhcp_error", birdiebot_wired_dap_dhcp_error());
		cJSON_AddNumberToObject(root, "wired_dhcp_state", birdiebot_wired_dap_dhcp_state());
		cJSON_AddStringToObject(root, "wired_ip", birdiebot_wired_dap_device_ip());
#endif
		/* Reuse existing state: last /api/target/info probe clock. */
		if (s_last_probe_valid) {
			cJSON_AddNumberToObject(root, "swd_clock_hz", (double) s_last_probe_swd_hz);
		}
		/* P0-8/E: read-only link quality.  Every field is a real measurement
		 * (Wi-Fi driver state, device-side DAP latency counters, heap, SWD
		 * clock actually used); nothing here is a fabricated RTT. */
		wt_tf_link_diag_t link;
		wt_tf_get_link_diag(&link);
		cJSON_AddBoolToObject(root, "wifi_connected", link.wifi_connected);
		if (link.wifi_connected) {
			cJSON_AddNumberToObject(root, "wifi_rssi", link.wifi_rssi);
			cJSON_AddStringToObject(root, "wifi_ssid", link.wifi_ssid);
			cJSON_AddNumberToObject(root, "wifi_channel", link.wifi_channel);
		}
		if (link.swd_clock_actual_hz != 0U) {
			cJSON_AddNumberToObject(root, "swd_clock_actual_hz",
			                        (double) link.swd_clock_actual_hz);
		}
		cJSON_AddNumberToObject(root, "swd_clock_manual_hz",
		                        (double) link.swd_clock_manual_hz);
		cJSON_AddNumberToObject(root, "dap_xfer_count",
		                        (double) link.dap_xfer_count);
		cJSON_AddNumberToObject(root, "dap_xfer_avg_us",
		                        (double) link.dap_xfer_avg_us);
		cJSON_AddNumberToObject(root, "dap_xfer_max_us",
		                        (double) link.dap_xfer_max_us);
		cJSON_AddNumberToObject(root, "dap_xfer_last_us",
		                        (double) link.dap_xfer_last_us);
		cJSON_AddBoolToObject(root, "dap_ring_preallocated",
		                      link.dap_ring_preallocated);
		cJSON_AddBoolToObject(root, "dap_ring_active", link.dap_ring_active);
		cJSON_AddNumberToObject(root, "dap_ring_request_bytes",
		                        (double) link.dap_ring_request_bytes);
		cJSON_AddNumberToObject(root, "dap_ring_response_bytes",
		                        (double) link.dap_ring_response_bytes);
		/* Reuse existing engine status: expose the last failure text without
		 * changing the numeric "err" contract. */
		if (status.err != 0) {
			cJSON_AddStringToObject(root, "last_error", status.message);
		}
	}
	return target_send_json(req, 200, TARGET_CMD_STATUS, root);
}

/* ------------------------------------------------------------------------- */
/* POST /api/target/reset                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t target_reset_handler(httpd_req_t *req)
{
	bool run = true;

	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_RESET, TARGET_ERR_FORBIDDEN,
		                         TARGET_CODE_PROPERTY);
	}

	uint32_t body_len = req->content_len;
	if (body_len > TARGET_RESET_BODY_MAX) {
		return target_send_error(req, 413, TARGET_CMD_RESET, "body too large",
		                         TARGET_CODE_PROPERTY);
	}
	if (body_len > 0) {
		char body[TARGET_RESET_BODY_MAX + 1];
		int received = httpd_req_recv(req, body, body_len);
		if (received <= 0) {
			return target_send_error(req, 400, TARGET_CMD_RESET, "bad json request",
			                         TARGET_CODE_BAD_REQUEST);
		}
		body[received] = '\0';
		cJSON *json = cJSON_ParseWithLength(body, (size_t) received);
		if (json == NULL) {
			return target_send_error(req, 400, TARGET_CMD_RESET, "bad json request",
			                         TARGET_CODE_BAD_REQUEST);
		}
		cJSON *run_json = cJSON_GetObjectItem(json, "run");
		if (cJSON_IsBool(run_json)) {
			run = cJSON_IsTrue(run_json);
		}
		cJSON_Delete(json);
	}

	esp_err_t err = wt_tf_reset_target(run);
	if (err != ESP_OK) {
		return target_send_mapped_error(req, TARGET_CMD_RESET, err, NULL);
	}

	cJSON *root = cJSON_CreateObject();
	if (root != NULL) {
		cJSON_AddNumberToObject(root, "module", TARGET_MODULE_ID);
		cJSON_AddNumberToObject(root, "cmd", TARGET_CMD_RESET);
		cJSON_AddBoolToObject(root, "ok", true);
		cJSON_AddBoolToObject(root, "run", run);
	}
	return target_send_json(req, 200, TARGET_CMD_RESET, root);
}

/* ------------------------------------------------------------------------- */
/* POST /api/target/flash                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t target_flash_handler(httpd_req_t *req)
{
	char content_type[128];
	char content_length[32];
	char transfer_encoding[32];
	char expected_sha[TARGET_SHA256_HEX_LEN + 1];
	wt_tf_info_t info = {0};
	esp_err_t err;

	/* Browser Host/Origin guard first (always enabled; not a login check). */
	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_FLASH, TARGET_ERR_FORBIDDEN,
		                         TARGET_CODE_PROPERTY);
	}

	/* Content-Type: application/octet-stream (strict prefix). */
	if (!target_read_header(req, "Content-Type", content_type, sizeof(content_type)) ||
	    strncasecmp(content_type, "application/octet-stream", 24) != 0) {
		return target_send_error(req, 415, TARGET_CMD_FLASH, TARGET_ERR_CONTENT_TYPE,
		                         TARGET_CODE_PROPERTY);
	}

	/* Mandatory Content-Length; no chunked transfer encoding. */
	if (target_read_header(req, "Transfer-Encoding", transfer_encoding,
	                       sizeof(transfer_encoding)) ||
	    !target_read_header(req, "Content-Length", content_length,
	                        sizeof(content_length)) ||
	    req->content_len == 0) {
		return target_send_error(req, 411, TARGET_CMD_FLASH, TARGET_ERR_CONTENT_LEN,
		                         TARGET_CODE_PROPERTY);
	}

	/* Mandatory X-Target-SHA256: browser-computed SHA-256 of the body. */
	if (!target_read_sha256_header(req, expected_sha)) {
		return target_send_error(req, 400, TARGET_CMD_FLASH, TARGET_ERR_BAD_SHA,
		                         TARGET_CODE_PROPERTY);
	}

	/* Single flash session at a time (checked before probing: a concurrent
	 * session would make probe return ESP_ERR_INVALID_STATE as well). */
	if (s_flash_busy) {
		return target_send_error(req, 409, TARGET_CMD_FLASH, TARGET_ERR_BUSY,
		                         TARGET_CODE_BUSY);
	}

	/* Identify the target before touching the flash (also gives max size). */
	err = wt_tf_probe(&info);
	if (err != ESP_OK || !info.detected) {
		return target_send_error(req, 422, TARGET_CMD_FLASH, TARGET_ERR_NO_TARGET,
		                         TARGET_CODE_PROPERTY);
	}
	if (!info.supported) {
		return target_send_error(req, 400, TARGET_CMD_FLASH,
		                         "target series not supported for flashing",
		                         TARGET_CODE_PROPERTY);
	}
	if (req->content_len > info.max_image_size) {
		return target_send_error(req, 413, TARGET_CMD_FLASH, TARGET_ERR_TOO_LARGE,
		                         TARGET_CODE_PROPERTY);
	}
	s_flash_busy = true;

	/* Erase the sectors covering the image (may take seconds; the engine owns
	 * the DAP until flash_end()/abort()). */
	err = wt_tf_flash_begin(req->content_len);
	if (err != ESP_OK) {
		s_flash_busy = false;
		ESP_LOGE(TAG, "target flash begin failed: %s", esp_err_to_name(err));
		return target_send_mapped_error(req, TARGET_CMD_FLASH, err, NULL);
	}

	mbedtls_sha256_context sha_ctx;
	mbedtls_sha256_init(&sha_ctx);
	mbedtls_sha256_starts(&sha_ctx, 0);

	uint32_t remaining = req->content_len;
	bool client_gone = false;
	bool recv_timeout = false;
	err = ESP_OK;

	while (remaining > 0) {
		size_t want = remaining > TARGET_CHUNK_SIZE ? TARGET_CHUNK_SIZE : remaining;
		int received = httpd_req_recv(req, (char *) s_rx_buf, want);
		if (received == HTTPD_SOCK_ERR_TIMEOUT) {
			recv_timeout = true;
			err = ESP_ERR_TIMEOUT;
			break;
		}
		if (received <= 0) {
			client_gone = true;
			break;
		}

		mbedtls_sha256_update(&sha_ctx, s_rx_buf, (size_t) received);
		err = wt_tf_flash_feed(s_rx_buf, (size_t) received);
		if (err != ESP_OK) {
			break;
		}
		remaining -= (uint32_t) received;
		target_yield_and_feed_wdt();
	}

	char computed_sha[TARGET_SHA256_HEX_LEN + 1] = {0};
	if (!client_gone && err == ESP_OK && remaining == 0) {
		uint8_t digest[32];
		mbedtls_sha256_finish(&sha_ctx, digest);
		target_sha_to_hex(digest, computed_sha);
	}
	mbedtls_sha256_free(&sha_ctx);

	if (client_gone) {
		/* Client went away: abort the session, no response is possible. */
		ESP_LOGW(TAG, "target flash: client disconnected, aborting");
		(void) wt_tf_abort();
		s_flash_busy = false;
		return ESP_OK;
	}

	if (recv_timeout) {
		/* Slow/stalled upload: abort the session and report an upload timeout
		 * (408) rather than a target-engine timeout (503). */
		ESP_LOGW(TAG, "target flash: upload timeout, aborting");
		(void) wt_tf_abort();
		s_flash_busy = false;
		return target_send_error(req, 408, TARGET_CMD_FLASH, "upload timeout",
		                         TARGET_CODE_BUSY);
	}

	if (err == ESP_OK && remaining == 0) {
		if (!target_sha_equal(expected_sha, computed_sha)) {
			ESP_LOGE(TAG, "target flash: sha256 mismatch (expected %s, got %s)",
			         expected_sha, computed_sha);
			(void) wt_tf_abort();
			s_flash_busy = false;
			return target_send_error(req, 422, TARGET_CMD_FLASH, TARGET_ERR_SHA_MISMATCH,
			                         TARGET_CODE_PROPERTY);
		}
		/* Flush + flash readback SHA-256 verify + reset/run. */
		err = wt_tf_flash_end();
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "target flash verify failed: %s", esp_err_to_name(err));
			(void) wt_tf_abort();
			s_flash_busy = false;
			return target_send_mapped_error(req, TARGET_CMD_FLASH, err, NULL);
		}
	} else {
		(void) wt_tf_abort();
		s_flash_busy = false;
		return target_send_mapped_error(req, TARGET_CMD_FLASH, err, NULL);
	}

	wt_tf_status_t status = {0};
	wt_tf_get_status(&status);

	char idcode[11];
	char uid[TARGET_UID_HEX_LEN];
	char flash_base[11];
	target_u32_to_hex(info.idcode, idcode);
	target_uid_to_hex(info.uid, uid);
	target_u32_to_hex(info.flash_base, flash_base);

	cJSON *root = cJSON_CreateObject();
	if (root != NULL) {
		cJSON_AddNumberToObject(root, "module", TARGET_MODULE_ID);
		cJSON_AddNumberToObject(root, "cmd", TARGET_CMD_FLASH);
		cJSON_AddBoolToObject(root, "ok", true);
		cJSON_AddStringToObject(root, "family", info.family);
		cJSON_AddStringToObject(root, "idcode", idcode);
		cJSON_AddStringToObject(root, "uid", uid);
		cJSON_AddNumberToObject(root, "written",
		                        status.written != 0 ? status.written : req->content_len);
		cJSON_AddNumberToObject(root, "verified", status.verified);
		cJSON_AddStringToObject(root, "sha256", computed_sha);
		cJSON_AddStringToObject(root, "flash_base", flash_base);
		cJSON_AddNumberToObject(root, "flash_size", info.flash_size);
		cJSON_AddStringToObject(root, "state", target_state_str(status.state));
		cJSON_AddStringToObject(root, "message", status.message);
	}

	s_flash_busy = false;
	return target_send_json(req, 200, TARGET_CMD_FLASH, root);
}

/* GET/POST /api/target/swd-clock                                           */
/* Manual SWD clock selection for target detect/flash and local sampling.    */
/* 0 = auto probe (2 MHz -> 1 MHz -> 500 kHz); manual 100 kHz..10 MHz.      */
static void target_swd_clock_json(httpd_req_t *req, int http_status,
                                  esp_err_t err, uint32_t hz)
{
	cJSON *root = cJSON_CreateObject();
	if (root != NULL) {
		cJSON_AddNumberToObject(root, "module", TARGET_MODULE_ID);
		cJSON_AddNumberToObject(root, "cmd", TARGET_CMD_SWD_CLOCK);
		cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
		cJSON_AddNumberToObject(root, "hz", (double) hz);
		cJSON_AddStringToObject(root, "mode", hz == 0U ? "auto" : "manual");
		if (err != ESP_OK) {
			cJSON_AddStringToObject(root, "err", "invalid swd clock (0 or 100000..10000000 Hz)");
		}
	}
	target_send_json(req, http_status, TARGET_CMD_SWD_CLOCK, root);
}

static esp_err_t target_swd_clock_get_handler(httpd_req_t *req)
{
	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_SWD_CLOCK, TARGET_ERR_FORBIDDEN,
		                         TARGET_CODE_PROPERTY);
	}
	target_swd_clock_json(req, 200, ESP_OK, wt_tf_get_swd_clock());
	return ESP_OK;
}

static esp_err_t target_swd_clock_set_handler(httpd_req_t *req)
{
	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_SWD_CLOCK, TARGET_ERR_FORBIDDEN,
		                         TARGET_CODE_PROPERTY);
	}
	if (req->content_len == 0 || req->content_len > 128) {
		return target_send_error(req, 400, TARGET_CMD_SWD_CLOCK, "bad json request",
		                         TARGET_CODE_BAD_REQUEST);
	}
	char body[129];
	int received = httpd_req_recv(req, body, req->content_len);
	if (received <= 0) {
		return target_send_error(req, 400, TARGET_CMD_SWD_CLOCK, "bad json request",
		                         TARGET_CODE_BAD_REQUEST);
	}
	body[received] = '\0';
	cJSON *json = cJSON_ParseWithLength(body, (size_t) received);
	if (json == NULL) {
		return target_send_error(req, 400, TARGET_CMD_SWD_CLOCK, "bad json request",
		                         TARGET_CODE_BAD_REQUEST);
	}
	cJSON *hz_json = cJSON_GetObjectItem(json, "hz");
	uint32_t hz = cJSON_IsNumber(hz_json) && hz_json->valuedouble >= 0
	              ? (uint32_t) hz_json->valuedouble : UINT32_MAX;
	cJSON_Delete(json);
	esp_err_t err = wt_tf_set_swd_clock(hz);
	target_swd_clock_json(req, err == ESP_OK ? 200 : 400, err,
	                      err == ESP_OK ? wt_tf_get_swd_clock() : wt_tf_get_swd_clock());
	return ESP_OK;
}

/* POST /api/dap/kick                                                         */
/* Drop the active USBIP/elaphureLink/WS-DAP session so the browser can open  */
/* its own WS-DAP connection on TCP 3240 (Web UI variable-curve page).       */
static esp_err_t dap_kick_handler(httpd_req_t *req)
{
    (void)req;
    tcp_server_kick_session();
    static const char resp[] = "{\"module\":7,\"cmd\":1,\"ok\":true,\"kicked\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------------- */
/* GET /api/discover (P1-19 nearby devices via mDNS)                         */
/* ------------------------------------------------------------------------- */

/* Discovery row buffer (target_flash wt_tf_discover_json formats into it).
 * Static: the httpd task has a 4 KiB stack and mDNS results can be up to
 * 8 devices. */
static char s_discover_json[TARGET_DISCOVER_JSON_MAX];

static esp_err_t target_discover_handler(httpd_req_t *req)
{
	size_t len = 0U;
	esp_err_t err;

	if (!web_host_guard_http_ok(req)) {
		return target_send_error(req, 403, TARGET_CMD_DISCOVER,
		                         TARGET_ERR_FORBIDDEN, TARGET_CODE_PROPERTY);
	}
	/* Read-only mDNS query: no DAP ownership, no target access.  The helper
	 * enforces one query at a time and never fabricates rows; mDNS not running
	 * (WiFi off / target without mDNS) yields {"ok":true,"devices":[]}. */
	err = wt_tf_discover_json(s_discover_json, sizeof(s_discover_json),
	                          WT_TF_DISCOVER_TIMEOUT_MS, &len);
	if (err == ESP_ERR_INVALID_STATE) {
		return target_send_error(req, 429, TARGET_CMD_DISCOVER,
		                         "discovery busy", TARGET_CODE_BUSY);
	}
	if (err != ESP_OK) {
		/* Never fail the Web's discovery poll: answer an empty, valid list. */
		static const char empty[] =
			"{\"ok\":true,\"devices\":[]}";
		httpd_resp_set_status(req, "200 OK");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		return httpd_resp_send(req, empty, HTTPD_RESP_USE_STRLEN);
	}
	httpd_resp_set_status(req, "200 OK");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, s_discover_json, (ssize_t)len);
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

static const httpd_uri_t uri_target_info = {
	.uri = TARGET_URI_INFO,
	.method = HTTP_GET,
	.handler = target_info_handler,
};

static const httpd_uri_t uri_target_flash = {
	.uri = TARGET_URI_FLASH,
	.method = HTTP_POST,
	.handler = target_flash_handler,
};

static const httpd_uri_t uri_target_status = {
	.uri = TARGET_URI_STATUS,
	.method = HTTP_GET,
	.handler = target_status_handler,
};

static const httpd_uri_t uri_target_reset = {
	.uri = TARGET_URI_RESET,
	.method = HTTP_POST,
	.handler = target_reset_handler,
};

static const httpd_uri_t uri_target_swd_clock_get = {
	.uri = TARGET_URI_SWD_CLOCK,
	.method = HTTP_GET,
	.handler = target_swd_clock_get_handler,
};

static const httpd_uri_t uri_target_swd_clock_set = {
	.uri = TARGET_URI_SWD_CLOCK,
	.method = HTTP_POST,
	.handler = target_swd_clock_set_handler,
};

static const httpd_uri_t dap_kick = {
	.uri = DAP_URI_KICK,
	.method = HTTP_POST,
	.handler = dap_kick_handler,
};

static const httpd_uri_t uri_target_discover = {
	.uri = TARGET_URI_DISCOVER,
	.method = HTTP_GET,
	.handler = target_discover_handler,
};

void uri_target_register(httpd_handle_t server)
{
	esp_err_t err = wt_tf_init();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "wt_tf_init failed: %s", esp_err_to_name(err));
	}

	static const struct {
		const char *uri;
		const httpd_uri_t *handler;
	} uris[] = {
		{TARGET_URI_INFO, &uri_target_info},
		{TARGET_URI_FLASH, &uri_target_flash},
		{TARGET_URI_STATUS, &uri_target_status},
		{TARGET_URI_RESET, &uri_target_reset},
		{TARGET_URI_SWD_CLOCK, &uri_target_swd_clock_get},
		{TARGET_URI_SWD_CLOCK, &uri_target_swd_clock_set},
		{DAP_URI_KICK, &dap_kick},
		{TARGET_URI_DISCOVER, &uri_target_discover},
	};

	for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
		err = httpd_register_uri_handler(server, uris[i].handler);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
		} else {
			ESP_LOGI(TAG, "registered %s", uris[i].uri);
		}
	}
}

#endif /* CONFIG_BIRDIEBOT_TARGET_FLASH_ENABLE */
