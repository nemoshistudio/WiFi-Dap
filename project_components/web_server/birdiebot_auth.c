/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file birdiebot_auth.c
 * @brief Web/API token authentication implementation (WP-209b).
 *
 * See include/birdiebot_auth.h for the behaviour contract. The pure string
 * parsing helpers live in auth_token_util.c so they are host-testable.
 *
 * Token source (when CONFIG_BIRDIEBOT_WEB_AUTH=y):
 *   1. CONFIG_BIRDIEBOT_WEB_TOKEN when non-empty (fixed pairing code);
 *   2. otherwise the token persisted in wt_nvs (namespace "web_auth", key
 *      "token") - stable across reboots;
 *   3. otherwise a random 8-hex-digit token that is generated once and stored
 *      in wt_nvs. The effective token is printed once at start-up as the
 *      "pairing token" (serial console / UART log).
 */
#include "birdiebot_auth.h"
#include "birdiebot_auth_util.h"

#include <sdkconfig.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwipopts.h>

#include <nvs.h>
#include <nvs_flash.h>

#include <stdio.h>
#include <string.h>

#define TAG "web_auth"

/* Fallbacks keep the file compiling when the Kconfig symbols were not (yet)
 * generated into sdkconfig.h; the defaults match project_components/web_server/Kconfig. */
#ifndef CONFIG_BIRDIEBOT_WEB_TOKEN
#define CONFIG_BIRDIEBOT_WEB_TOKEN ""
#endif
#ifndef CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN
#define CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN ""
#endif

#define AUTH_WS_MAX CONFIG_LWIP_MAX_SOCKETS
#define AUTH_NVS_PARTITION "wt_nvs"
#define AUTH_NVS_NAMESPACE "web_auth"
#define AUTH_NVS_KEY "token"

#if BIRDIEBOT_AUTH_ENABLED

/* AUTH failure rate limiting (security review P1-4): at most
 * AUTH_FAIL_MAX failures per AUTH_FAIL_WINDOW_MS per socket fd. */
#define AUTH_FAIL_MAX       5
#define AUTH_FAIL_WINDOW_MS 60000

typedef struct {
	uint32_t window_start_ms;
	uint8_t failures;
} auth_fail_state_t;

static char s_token[BIRDIEBOT_AUTH_TOKEN_MAX + 1];
static bool s_ws_authed[AUTH_WS_MAX];
static auth_fail_state_t s_ws_fail[AUTH_WS_MAX];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_inited;

static void auth_lock(void)
{
	if (s_lock != NULL) {
		xSemaphoreTake(s_lock, portMAX_DELAY);
	}
}

static void auth_unlock(void)
{
	if (s_lock != NULL) {
		xSemaphoreGive(s_lock);
	}
}

static int auth_fd_index(int fd)
{
	int idx = fd - LWIP_SOCKET_OFFSET;
	return (idx >= 0 && idx < AUTH_WS_MAX) ? idx : -1;
}

static void auth_ws_set(int fd, bool authed)
{
	int idx = auth_fd_index(fd);
	if (idx < 0) {
		return;
	}
	auth_lock();
	s_ws_authed[idx] = authed;
	if (authed) {
		memset(&s_ws_fail[idx], 0, sizeof(s_ws_fail[idx]));
	}
	auth_unlock();
}

/** Reset the AUTH failure counter of one fd (successful auth / new connection). */
static void auth_ws_reset_failures(int fd)
{
	int idx = auth_fd_index(fd);

	if (idx < 0) {
		return;
	}
	auth_lock();
	memset(&s_ws_fail[idx], 0, sizeof(s_ws_fail[idx]));
	auth_unlock();
}

/** Record one AUTH failure; returns true when the fd is now rate limited. */
static bool auth_ws_note_failure(int fd)
{
	int idx = auth_fd_index(fd);
	bool limited = false;
	uint32_t now = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);

	if (idx < 0) {
		return true;
	}
	auth_lock();
	if (s_ws_fail[idx].window_start_ms == 0 ||
	    (uint32_t) (now - s_ws_fail[idx].window_start_ms) > AUTH_FAIL_WINDOW_MS) {
		s_ws_fail[idx].window_start_ms = now;
		s_ws_fail[idx].failures = 0;
	}
	if (s_ws_fail[idx].failures < 0xFF) {
		s_ws_fail[idx].failures++;
	}
	limited = s_ws_fail[idx].failures > AUTH_FAIL_MAX;
	auth_unlock();
	return limited;
}

/* ------------------------------------------------------------------------- */
/* Token source / persistence                                                */
/* ------------------------------------------------------------------------- */

static bool auth_nvs_load_token(char *out, size_t out_len)
{
	nvs_handle_t handle;
	size_t len = out_len;
	esp_err_t err;

	if (nvs_open_from_partition(AUTH_NVS_PARTITION, AUTH_NVS_NAMESPACE,
	                            NVS_READONLY, &handle) != ESP_OK) {
		return false;
	}
	err = nvs_get_str(handle, AUTH_NVS_KEY, out, &len);
	nvs_close(handle);

	if (err != ESP_OK || out[0] == '\0' || strlen(out) > BIRDIEBOT_AUTH_TOKEN_MAX) {
		return false;
	}
	return true;
}

static void auth_nvs_store_token(const char *token)
{
	nvs_handle_t handle;
	esp_err_t err;

	if (nvs_open_from_partition(AUTH_NVS_PARTITION, AUTH_NVS_NAMESPACE,
	                            NVS_READWRITE, &handle) != ESP_OK) {
		ESP_LOGW(TAG, "wt_nvs partition/%s namespace unavailable; token will not persist",
		         AUTH_NVS_NAMESPACE);
		return;
	}
	err = nvs_set_str(handle, AUTH_NVS_KEY, token);
	if (err == ESP_OK) {
		err = nvs_commit(handle);
	}
	nvs_close(handle);

	if (err != ESP_OK) {
		ESP_LOGW(TAG, "failed to persist generated token (%s); it will change on reboot",
		         esp_err_to_name(err));
	}
}

/** Minimum length of an auto-generated token (128-bit = 32 hex chars, P1-4). */
#define AUTH_TOKEN_MIN_GENERATED_LEN 32

/** Generate a 128-bit token as 32 lowercase hex chars (requires out_len >= 33). */
static void auth_generate_token(char *out, size_t out_len)
{
	uint32_t words[4];

	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
		words[i] = esp_random();
	}
	snprintf(out, out_len, "%08lx%08lx%08lx%08lx",
	         (unsigned long) words[0], (unsigned long) words[1],
	         (unsigned long) words[2], (unsigned long) words[3]);
}

static void auth_token_init(void)
{
	const char *source;

	const char *fixed = CONFIG_BIRDIEBOT_WEB_TOKEN;
	if (fixed[0] != '\0') {
		snprintf(s_token, sizeof(s_token), "%s", fixed);
		if (strlen(fixed) >= sizeof(s_token)) {
			ESP_LOGW(TAG, "CONFIG_BIRDIEBOT_WEB_TOKEN too long, truncated to %d chars",
			         BIRDIEBOT_AUTH_TOKEN_MAX);
		}
		if (strlen(s_token) < AUTH_TOKEN_MIN_GENERATED_LEN) {
			ESP_LOGW(TAG, "CONFIG_BIRDIEBOT_WEB_TOKEN is shorter than %d chars; "
			         "use at least 32 hex chars (128-bit) on non-lab deployments",
			         AUTH_TOKEN_MIN_GENERATED_LEN);
		}
		source = "Kconfig BIRDIEBOT_WEB_TOKEN";
	} else if (auth_nvs_load_token(s_token, sizeof(s_token)) &&
	           strlen(s_token) >= AUTH_TOKEN_MIN_GENERATED_LEN) {
		source = "stored in wt_nvs";
	} else {
		/* Security review P1-4: 128-bit token (32 hex chars) generated from four
		 * esp_random() words. A legacy 8-hex token loaded from NVS is rotated
		 * once, since 32-bit pairing codes are brute-forceable. */
		auth_generate_token(s_token, sizeof(s_token));
		auth_nvs_store_token(s_token);
		source = "random 128-bit, persisted in wt_nvs";
	}

	/* The pairing code must be discoverable by the user (serial console), so it
	 * is printed once at start-up. WARNING: the log line itself is a secret -
	 * do not paste serial logs/screenshots into public reports. */
	ESP_LOGW(TAG, "Web auth ENABLED; pairing token: %s (%s)", s_token, source);
	ESP_LOGW(TAG, "SECURITY: the pairing token above is secret; serial logs/screenshots leak it");
	ESP_LOGW(TAG, "WS: /ws?token=<token> or first frame SYSTEM/0/AUTH; HTTP: Authorization: Bearer <token>");
	ESP_LOGW(TAG, "Note: this protects only ports 80/WS-API; TCP 3240 (USBIP/WS-DAP) and 1346 (UART bridge) are unauthenticated");
}

#endif /* BIRDIEBOT_AUTH_ENABLED - token generation/persistence */

/* ------------------------------------------------------------------------- */
/* Host / Origin validation (DNS-rebinding hardening)                        */
/*                                                                           */
/* NOTE (security review P1-2): this policy is ALWAYS compiled and enforced, */
/* independently of CONFIG_BIRDIEBOT_WEB_AUTH. A browser page can open a     */
/* WebSocket cross-origin without a CORS preflight (CSWSH) and DNS rebinding */
/* is possible without token auth, so /ws upgrades, /api and upcoming        */
/* endpoints (/api/ota/upload) are Host/Origin checked in lab mode too.      */
/* ------------------------------------------------------------------------- */

static bool auth_read_header(httpd_req_t *req, const char *name, char *buf, size_t buf_len)
{
	size_t value_len = httpd_req_get_hdr_value_len(req, name);
	if (value_len == 0 || value_len >= buf_len) {
		return false;
	}
	return httpd_req_get_hdr_value_str(req, name, buf, buf_len) == ESP_OK;
}

static const char *auth_local_hostname(void)
{
#ifdef CONFIG_LWIP_LOCAL_HOSTNAME
	return CONFIG_LWIP_LOCAL_HOSTNAME;
#else
	return NULL;
#endif
}

static bool auth_host_header_ok(httpd_req_t *req)
{
	char host[128];
	char bare[BIRDIEBOT_AUTH_HOST_MAX];

	if (!auth_read_header(req, "Host", host, sizeof(host))) {
		ESP_LOGW(TAG, "request rejected: missing Host header");
		return false;
	}
	if (!birdiebot_auth_split_authority(host, bare, sizeof(bare))) {
		ESP_LOGW(TAG, "request rejected: malformed Host header");
		return false;
	}

	if (birdiebot_auth_host_allowed(bare, CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN,
	                                auth_local_hostname())) {
		return true;
	}
	ESP_LOGW(TAG, "request rejected: Host '%s' not an IP literal / allow-listed", bare);
	return false;
}

static bool auth_origin_header_ok(httpd_req_t *req)
{
	char origin[256];
	char host[128];
	char bare[BIRDIEBOT_AUTH_HOST_MAX];

	if (!auth_read_header(req, "Origin", origin, sizeof(origin))) {
		/* Non-browser clients (curl/scripts) do not send Origin. The token is
		 * still required, so this is not a hole. */
		return true;
	}
	if (!auth_read_header(req, "Host", host, sizeof(host)) ||
	    !birdiebot_auth_split_authority(host, bare, sizeof(bare))) {
		ESP_LOGW(TAG, "request rejected: malformed Origin/Host");
		return false;
	}
	if (birdiebot_auth_origin_allowed(origin, bare, CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN)) {
		return true;
	}
	ESP_LOGW(TAG, "request rejected: Origin '%s' does not match Host '%s'", origin, bare);
	return false;
}

static bool auth_headers_ok(httpd_req_t *req)
{
	return auth_host_header_ok(req) && auth_origin_header_ok(req);
}

#if BIRDIEBOT_AUTH_ENABLED
static bool auth_request_token_ok(httpd_req_t *req)
{
	char header[384];
	char token[BIRDIEBOT_AUTH_TOKEN_MAX + 1];

	if (auth_read_header(req, "Authorization", header, sizeof(header))) {
		if (birdiebot_auth_extract_bearer(header, token, sizeof(token)) == 1 &&
		    birdiebot_auth_token_equal(s_token, token)) {
			return true;
		}
	}
	if (birdiebot_auth_extract_query_token(req->uri, token, sizeof(token)) == 1 &&
	    birdiebot_auth_token_equal(s_token, token)) {
		return true;
	}
	return false;
}

#endif /* BIRDIEBOT_AUTH_ENABLED */

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool birdiebot_auth_enabled(void)
{
#if BIRDIEBOT_AUTH_ENABLED
	return true;
#else
	return false;
#endif
}

void birdiebot_auth_init(void)
{
#if BIRDIEBOT_AUTH_ENABLED
	if (s_inited) {
		return;
	}

	s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
	memset(s_ws_authed, 0, sizeof(s_ws_authed));
	memset(s_ws_fail, 0, sizeof(s_ws_fail));
	auth_token_init();
	s_inited = true;
#endif
}

int birdiebot_auth_http_check(httpd_req_t *req)
{
	if (req == NULL) {
		return BIRDIEBOT_AUTH_HTTP_FORBIDDEN;
	}

	/* P1-2: Host/Origin is enforced even with CONFIG_BIRDIEBOT_WEB_AUTH=n
	 * (CSWSH / DNS rebinding hardening is independent from the token). */
	if (!auth_headers_ok(req)) {
		return BIRDIEBOT_AUTH_HTTP_FORBIDDEN;
	}

#if BIRDIEBOT_AUTH_ENABLED
	if (!s_inited) {
		return BIRDIEBOT_AUTH_HTTP_UNAUTHORIZED;
	}
	if (!auth_request_token_ok(req)) {
		ESP_LOGW(TAG, "HTTP request rejected: missing/invalid token");
		return BIRDIEBOT_AUTH_HTTP_UNAUTHORIZED;
	}
#endif
	return BIRDIEBOT_AUTH_HTTP_OK;
}

bool birdiebot_auth_http_ok(httpd_req_t *req)
{
	return birdiebot_auth_http_check(req) == BIRDIEBOT_AUTH_HTTP_OK;
}

bool birdiebot_auth_ws_origin_ok(httpd_req_t *req)
{
	/* P1-2: always enforced (also in lab mode) to block CSWSH/DNS rebinding. */
	if (req == NULL) {
		return false;
	}
	return auth_headers_ok(req);
}

bool birdiebot_auth_host_origin_ok(httpd_req_t *req)
{
	return birdiebot_auth_ws_origin_ok(req);
}

int birdiebot_auth_ws_query_ok(httpd_req_t *req, int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	char token[BIRDIEBOT_AUTH_TOKEN_MAX + 1];
	int ret;

	if (req == NULL || !s_inited) {
		return -1;
	}
	ret = birdiebot_auth_extract_query_token(req->uri, token, sizeof(token));
	if (ret == 0) {
		return 1; /* no ?token=: keep the connection pending for the AUTH frame */
	}
	if (ret > 0 && birdiebot_auth_token_equal(s_token, token)) {
		auth_ws_set(fd, true);
		return 0;
	}
	return -1;
#else
	(void) req;
	(void) fd;
	return 1;
#endif
}

bool birdiebot_auth_ws_token_ok(const char *token, int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	if (token == NULL || !s_inited) {
		return false;
	}
	if (!birdiebot_auth_token_equal(s_token, token)) {
		return false;
	}
	auth_ws_set(fd, true);
	return true;
#else
	(void) token;
	(void) fd;
	return true;
#endif
}

bool birdiebot_auth_ws_fd_ok(int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	int idx;
	bool ok;

	if (!s_inited) {
		return false;
	}
	idx = auth_fd_index(fd);
	if (idx < 0) {
		return false;
	}
	auth_lock();
	ok = s_ws_authed[idx];
	auth_unlock();
	return ok;
#else
	(void) fd;
	return true;
#endif
}

void birdiebot_auth_ws_open(int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	auth_ws_set(fd, false);
	auth_ws_reset_failures(fd);
#else
	(void) fd;
#endif
}

void birdiebot_auth_ws_close(int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	auth_ws_set(fd, false);
	auth_ws_reset_failures(fd);
#else
	(void) fd;
#endif
}

bool birdiebot_auth_ws_note_auth_failure(int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	return auth_ws_note_failure(fd);
#else
	(void) fd;
	return false;
#endif
}

void birdiebot_auth_ws_reset_auth_failures(int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	auth_ws_reset_failures(fd);
#else
	(void) fd;
#endif
}
