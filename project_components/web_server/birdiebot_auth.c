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

static char s_token[BIRDIEBOT_AUTH_TOKEN_MAX + 1];
static bool s_ws_authed[AUTH_WS_MAX];
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
	auth_unlock();
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
		source = "Kconfig BIRDIEBOT_WEB_TOKEN";
	} else if (auth_nvs_load_token(s_token, sizeof(s_token))) {
		source = "stored in wt_nvs";
	} else {
		snprintf(s_token, sizeof(s_token), "%08lx", (unsigned long) esp_random());
		auth_nvs_store_token(s_token);
		source = "random, persisted in wt_nvs";
	}

	/* The pairing code must be discoverable by the user: print it once. It is
	 * a secret, so this is the only place where it is logged. */
	ESP_LOGW(TAG, "Web auth ENABLED; pairing token: %s (%s)", s_token, source);
	ESP_LOGW(TAG, "WS: /ws?token=<token> or first frame SYSTEM/0/AUTH; HTTP: Authorization: Bearer <token>");
}

/* ------------------------------------------------------------------------- */
/* Host / Origin validation (DNS-rebinding hardening)                        */
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
	auth_token_init();
	s_inited = true;
#endif
}

bool birdiebot_auth_http_ok(httpd_req_t *req)
{
#if BIRDIEBOT_AUTH_ENABLED
	if (req == NULL || !s_inited) {
		return false;
	}
	if (!auth_headers_ok(req)) {
		return false;
	}
	if (!auth_request_token_ok(req)) {
		ESP_LOGW(TAG, "HTTP request rejected: missing/invalid token");
		return false;
	}
	return true;
#else
	(void) req;
	return true;
#endif
}

bool birdiebot_auth_ws_origin_ok(httpd_req_t *req)
{
#if BIRDIEBOT_AUTH_ENABLED
	if (req == NULL || !s_inited) {
		return false;
	}
	return auth_headers_ok(req);
#else
	(void) req;
	return true;
#endif
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
#else
	(void) fd;
#endif
}

void birdiebot_auth_ws_close(int fd)
{
#if BIRDIEBOT_AUTH_ENABLED
	auth_ws_set(fd, false);
#else
	(void) fd;
#endif
}
