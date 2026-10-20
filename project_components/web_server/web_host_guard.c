/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_host_guard.c
 * @brief Browser Host/Origin guard for the web server (CSWSH / DNS rebinding).
 *
 * This is a browser-safety policy, not a login mechanism: it is always active
 * and carries no token/auth state (the Web UI login feature has been removed).
 *
 * Policy (docs/protocol.md §1 / docs/security-review-web.md P1-2):
 *  - Host header must be an IP literal, an entry of
 *    CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN, or CONFIG_LWIP_LOCAL_HOSTNAME.
 *    This blocks DNS-rebinding: an attacker-controlled DNS name that resolves
 *    to the device IP is rejected.
 *  - Origin header, when present (browsers send it for WebSocket upgrades and
 *    non-GET requests), must be same-origin with the request Host or match the
 *    allow-list. Non-browser clients (curl/scripts) send no Origin and stay
 *    usable.
 *
 * Accessing the device through its IP (the documented way) needs no
 * configuration; hostname/mDNS/proxy access requires an allow-list entry.
 */
#include "web_host_guard.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include <sdkconfig.h>

#include <stdbool.h>
#include <string.h>

#define TAG "web_host"

/* Fallback keeps the file compiling when the Kconfig symbol was not generated
 * into sdkconfig.h (host syntax checks / stale sdkconfig). */
#ifndef CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN
#define CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN ""
#endif

#define HOST_GUARD_HOST_MAX 128

static char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

static bool host_guard_split_authority(const char *authority, char *host, size_t host_len)
{
	if (authority == NULL || host == NULL || host_len < 2) {
		return false;
	}
	host[0] = '\0';

	const char *start = authority;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	const char *end = start + strlen(start);
	while (end > start) {
		char c = end[-1];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			end--;
		} else {
			break;
		}
	}
	if (start >= end) {
		return false;
	}

	/* Bracketed IPv6 literal: "[::1]" or "[::1]:80" -> "::1". */
	if (*start == '[') {
		const char *rb = memchr(start, ']', (size_t) (end - start));
		if (rb == NULL) {
			return false;
		}
		size_t hlen = (size_t) (rb - start - 1);
		if (hlen == 0 || hlen + 1 > host_len) {
			return false;
		}
		memcpy(host, start + 1, hlen);
		host[hlen] = '\0';
		return true;
	}

	/* "host" or "host:port": the port is discarded. */
	const char *colon = memchr(start, ':', (size_t) (end - start));
	const char *host_end = colon ? colon : end;
	size_t hlen = (size_t) (host_end - start);
	if (hlen == 0 || hlen + 1 > host_len) {
		return false;
	}
	for (size_t i = 0; i < hlen; ++i) {
		char c = start[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
		if (!ok) {
			return false;
		}
	}
	memcpy(host, start, hlen);
	host[hlen] = '\0';
	return true;
}

static bool host_guard_host_is_ip_literal(const char *host)
{
	if (host == NULL || *host == '\0') {
		return false;
	}
	if (strchr(host, ':') != NULL) {
		return true; /* IPv6 literal (brackets already stripped) */
	}

	unsigned part = 0;
	int digits = 0;
	int dots = 0;
	for (const char *p = host;; ++p) {
		char c = *p;
		if (c >= '0' && c <= '9') {
			if (digits >= 3) {
				return false;
			}
			part = part * 10u + (unsigned) (c - '0');
			digits++;
			if (part > 255) {
				return false;
			}
		} else if (c == '.') {
			if (digits == 0) {
				return false;
			}
			dots++;
			if (dots > 3) {
				return false;
			}
			part = 0;
			digits = 0;
		} else if (c == '\0') {
			break;
		} else {
			return false;
		}
	}
	return dots == 3 && digits > 0;
}

static bool host_guard_host_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
	size_t la = strlen(a);
	size_t lb = strlen(b);
	while (la > 0 && a[la - 1] == '.') {
		la--; /* ignore a trailing root dot: "dap.local." == "dap.local" */
	}
	while (lb > 0 && b[lb - 1] == '.') {
		lb--;
	}
	if (la == 0 || lb == 0 || la != lb) {
		return false;
	}
	for (size_t i = 0; i < la; ++i) {
		if (ascii_lower(a[i]) != ascii_lower(b[i])) {
			return false;
		}
	}
	return true;
}

static bool host_guard_origin_host(const char *origin, char *host, size_t host_len)
{
	if (origin == NULL || host == NULL) {
		return false;
	}
	host[0] = '\0';

	const char *authority = origin;
	const char *scheme_sep = strstr(origin, "://");
	if (scheme_sep != NULL) {
		authority = scheme_sep + 3;
	}

	size_t alen = strcspn(authority, "/?#");
	const char *userinfo = memchr(authority, '@', alen);
	if (userinfo != NULL) {
		alen -= (size_t) (userinfo + 1 - authority);
		authority = userinfo + 1;
	}
	if (alen == 0 || alen >= 192) {
		return false;
	}

	char tmp[192];
	memcpy(tmp, authority, alen);
	tmp[alen] = '\0';
	return host_guard_split_authority(tmp, host, host_len);
}

static bool host_guard_origin_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
	size_t la = strlen(a);
	size_t lb = strlen(b);
	while (la > 0 && (a[la - 1] == '/' || a[la - 1] == ' ' || a[la - 1] == '\t')) {
		la--;
	}
	while (lb > 0 && (b[lb - 1] == '/' || b[lb - 1] == ' ' || b[lb - 1] == '\t')) {
		lb--;
	}
	if (la == 0 || lb == 0 || la != lb) {
		return false;
	}
	for (size_t i = 0; i < la; ++i) {
		if (ascii_lower(a[i]) != ascii_lower(b[i])) {
			return false;
		}
	}
	return true;
}

static int list_next(const char *list, size_t *pos, char *entry, size_t entry_len)
{
	for (;;) {
		const char *p = list + *pos;
		if (*p == '\0') {
			return 0;
		}
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			*pos = (size_t) (p - list);
			return 0;
		}

		const char *comma = strchr(p, ',');
		const char *end = comma ? comma : p + strlen(p);
		size_t len = (size_t) (end - p);
		while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
			len--;
		}

		*pos = (size_t) ((comma ? comma + 1 : end) - list);
		if (len == 0) {
			continue; /* skip empty entries (",," or trailing ",") */
		}
		if (len + 1 > entry_len) {
			return -1;
		}
		memcpy(entry, p, len);
		entry[len] = '\0';
		return 1;
	}
}

static bool host_guard_host_in_list(const char *host, const char *whitelist)
{
	if (host == NULL || *host == '\0' || whitelist == NULL || *whitelist == '\0') {
		return false;
	}

	char entry[192];
	char entry_host[HOST_GUARD_HOST_MAX];
	size_t pos = 0;
	while (list_next(whitelist, &pos, entry, sizeof(entry)) == 1) {
		if (host_guard_origin_host(entry, entry_host, sizeof(entry_host)) &&
		    host_guard_host_equal(host, entry_host)) {
			return true;
		}
	}
	return false;
}

static bool host_guard_origin_in_list(const char *origin, const char *whitelist)
{
	if (origin == NULL || *origin == '\0' || whitelist == NULL || *whitelist == '\0') {
		return false;
	}

	char ohost[HOST_GUARD_HOST_MAX];
	if (!host_guard_origin_host(origin, ohost, sizeof(ohost))) {
		return false;
	}

	char entry[192];
	char entry_host[HOST_GUARD_HOST_MAX];
	size_t pos = 0;
	while (list_next(whitelist, &pos, entry, sizeof(entry)) == 1) {
		if (strstr(entry, "://") != NULL) {
			if (host_guard_origin_equal(origin, entry)) {
				return true;
			}
		} else if (host_guard_origin_host(entry, entry_host, sizeof(entry_host)) &&
		           host_guard_host_equal(ohost, entry_host)) {
			return true;
		}
	}
	return false;
}

static bool host_guard_host_allowed(const char *host, const char *whitelist,
                                 const char *local_hostname)
{
	if (host == NULL || *host == '\0') {
		return false;
	}
	if (host_guard_host_is_ip_literal(host)) {
		return true;
	}
	if (host_guard_host_in_list(host, whitelist)) {
		return true;
	}
	if (local_hostname != NULL && *local_hostname != '\0' &&
	    host_guard_host_equal(host, local_hostname)) {
		return true;
	}
	return false;
}

static bool host_guard_origin_allowed(const char *origin, const char *host,
                                   const char *whitelist)
{
	if (origin == NULL || *origin == '\0') {
		return true; /* non-browser client: no Origin to compare (Host still checked) */
	}
	if (host == NULL || *host == '\0') {
		return false;
	}

	char origin_host[HOST_GUARD_HOST_MAX];
	if (!host_guard_origin_host(origin, origin_host, sizeof(origin_host))) {
		return false;
	}
	if (host_guard_host_equal(origin_host, host)) {
		return true; /* same-origin request from the device UI */
	}
	return host_guard_origin_in_list(origin, whitelist);
}

static bool host_guard_read_header(httpd_req_t *req, const char *name, char *buf, size_t buf_len)
{
	size_t value_len = httpd_req_get_hdr_value_len(req, name);
	if (value_len == 0 || value_len >= buf_len) {
		return false;
	}
	return httpd_req_get_hdr_value_str(req, name, buf, buf_len) == ESP_OK;
}

static const char *host_guard_local_hostname(void)
{
#ifdef CONFIG_LWIP_LOCAL_HOSTNAME
	return CONFIG_LWIP_LOCAL_HOSTNAME;
#else
	return NULL;
#endif
}

static bool host_guard_host_header_ok(httpd_req_t *req)
{
	char host[128];
	char bare[HOST_GUARD_HOST_MAX];

	if (!host_guard_read_header(req, "Host", host, sizeof(host))) {
		ESP_LOGW(TAG, "request rejected: missing Host header");
		return false;
	}
	if (!host_guard_split_authority(host, bare, sizeof(bare))) {
		ESP_LOGW(TAG, "request rejected: malformed Host header");
		return false;
	}

	if (host_guard_host_allowed(bare, CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN,
	                                host_guard_local_hostname())) {
		return true;
	}
	ESP_LOGW(TAG, "request rejected: Host '%s' not an IP literal / allow-listed", bare);
	return false;
}

static bool host_guard_origin_header_ok(httpd_req_t *req)
{
	char origin[256];
	char host[128];
	char bare[HOST_GUARD_HOST_MAX];

	if (!host_guard_read_header(req, "Origin", origin, sizeof(origin))) {
		/* Non-browser clients (curl/scripts) do not send Origin. The Host
		 * check still applies, so this is not a hole. */
		return true;
	}
	if (!host_guard_read_header(req, "Host", host, sizeof(host)) ||
	    !host_guard_split_authority(host, bare, sizeof(bare))) {
		ESP_LOGW(TAG, "request rejected: malformed Origin/Host");
		return false;
	}
	if (host_guard_origin_allowed(origin, bare, CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN)) {
		return true;
	}
	ESP_LOGW(TAG, "request rejected: Origin '%s' does not match Host '%s'", origin, bare);
	return false;
}

static bool host_guard_headers_ok(httpd_req_t *req)
{
	return host_guard_host_header_ok(req) && host_guard_origin_header_ok(req);
}

bool web_host_guard_http_ok(httpd_req_t *req)
{
	if (req == NULL) {
		return false;
	}
	return host_guard_headers_ok(req);
}

bool web_host_guard_ws_ok(httpd_req_t *req)
{
	/* A WebSocket upgrade is an HTTP GET request; the same Host/Origin policy
	 * applies (browsers always send Origin for WS upgrades, so CSWSH is
	 * blocked by the same-origin check). */
	return web_host_guard_http_ok(req);
}
