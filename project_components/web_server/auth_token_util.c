/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file auth_token_util.c
 * @brief Web auth pure helpers (no ESP-IDF dependency; host-unit-testable).
 *
 * See include/birdiebot_auth_util.h and docs/protocol.md §1/§5.
 */
#include "birdiebot_auth_util.h"

#include <stdint.h>
#include <string.h>

static char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	c = ascii_lower(c);
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

bool birdiebot_auth_token_equal(const char *expected, const char *provided)
{
	if (expected == NULL || provided == NULL) {
		return false;
	}

	size_t elen = strlen(expected);
	size_t plen = strlen(provided);
	if (elen == 0 || elen > BIRDIEBOT_AUTH_TOKEN_MAX || elen != plen) {
		return false;
	}

	/* Constant-time comparison: never leak the matching prefix length. */
	unsigned diff = 0;
	for (size_t i = 0; i < elen; ++i) {
		diff |= (unsigned) (uint8_t) expected[i] ^ (unsigned) (uint8_t) provided[i];
	}
	return diff == 0;
}

/**
 * @brief Percent-decode @p src into @p out (NUL terminated).
 * @return decoded length, or -1 on invalid escape / output too long.
 */
static int decode_percent(const char *src, size_t src_len, char *out, size_t out_len)
{
	size_t o = 0;

	for (size_t i = 0; i < src_len; ++i) {
		char c = src[i];
		if (c == '%' && i + 2 < src_len) {
			int hi = hex_value(src[i + 1]);
			int lo = hex_value(src[i + 2]);
			if (hi < 0 || lo < 0 || o + 1 >= out_len) {
				return -1;
			}
			out[o++] = (char) ((hi << 4) | lo);
			i += 2;
		} else {
			if (o + 1 >= out_len) {
				return -1;
			}
			out[o++] = c;
		}
	}
	out[o] = '\0';
	return (int) o;
}

int birdiebot_auth_extract_query_token(const char *uri, char *out, size_t out_len)
{
	if (uri == NULL || out == NULL || out_len < 2) {
		return -1;
	}
	out[0] = '\0';

	const char *query = strchr(uri, '?');
	if (query == NULL) {
		return 0;
	}
	query++;

	while (*query != '\0') {
		const char *amp = strchr(query, '&');
		size_t param_len = amp ? (size_t) (amp - query) : strlen(query);

		if (param_len >= 6 && memcmp(query, "token=", 6) == 0) {
			int len = decode_percent(query + 6, param_len - 6, out, out_len);
			return len < 0 ? -1 : 1;
		}
		if (amp == NULL) {
			break;
		}
		query = amp + 1;
	}
	return 0;
}

int birdiebot_auth_extract_bearer(const char *authorization, char *out, size_t out_len)
{
	static const char scheme[] = "bearer";

	if (authorization == NULL || out == NULL || out_len < 2) {
		return -1;
	}
	out[0] = '\0';

	const char *p = authorization;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	for (size_t i = 0; i < sizeof(scheme) - 1; ++i) {
		if (p[i] == '\0' || ascii_lower(p[i]) != scheme[i]) {
			return 0; /* not a Bearer header */
		}
	}
	p += sizeof(scheme) - 1;
	if (*p != ' ' && *p != '\t') {
		return 0;
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	size_t len = strlen(p);
	while (len > 0) {
		char c = p[len - 1];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			len--;
		} else {
			break;
		}
	}
	if (len == 0) {
		return -1; /* "Bearer " with an empty token */
	}
	if (len + 1 > out_len) {
		return -1;
	}
	memcpy(out, p, len);
	out[len] = '\0';
	return 1;
}

bool birdiebot_auth_split_authority(const char *authority, char *host, size_t host_len)
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

bool birdiebot_auth_host_is_ip_literal(const char *host)
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

bool birdiebot_auth_host_equal(const char *a, const char *b)
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

bool birdiebot_auth_origin_host(const char *origin, char *host, size_t host_len)
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
	return birdiebot_auth_split_authority(tmp, host, host_len);
}

bool birdiebot_auth_origin_equal(const char *a, const char *b)
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

/**
 * @brief Fetch the next non-empty comma-separated entry.
 *
 * @param list      Comma-separated list.
 * @param pos       Cursor (in/out).
 * @param entry     Entry output buffer.
 * @param entry_len Size of @p entry.
 * @return 1 entry, 0 end of list, -1 entry does not fit.
 */
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

bool birdiebot_auth_host_in_list(const char *host, const char *whitelist)
{
	if (host == NULL || *host == '\0' || whitelist == NULL || *whitelist == '\0') {
		return false;
	}

	char entry[192];
	char entry_host[BIRDIEBOT_AUTH_HOST_MAX];
	size_t pos = 0;
	while (list_next(whitelist, &pos, entry, sizeof(entry)) == 1) {
		if (birdiebot_auth_origin_host(entry, entry_host, sizeof(entry_host)) &&
		    birdiebot_auth_host_equal(host, entry_host)) {
			return true;
		}
	}
	return false;
}

bool birdiebot_auth_origin_in_list(const char *origin, const char *whitelist)
{
	if (origin == NULL || *origin == '\0' || whitelist == NULL || *whitelist == '\0') {
		return false;
	}

	char ohost[BIRDIEBOT_AUTH_HOST_MAX];
	if (!birdiebot_auth_origin_host(origin, ohost, sizeof(ohost))) {
		return false;
	}

	char entry[192];
	char entry_host[BIRDIEBOT_AUTH_HOST_MAX];
	size_t pos = 0;
	while (list_next(whitelist, &pos, entry, sizeof(entry)) == 1) {
		if (strstr(entry, "://") != NULL) {
			if (birdiebot_auth_origin_equal(origin, entry)) {
				return true;
			}
		} else if (birdiebot_auth_origin_host(entry, entry_host, sizeof(entry_host)) &&
		           birdiebot_auth_host_equal(ohost, entry_host)) {
			return true;
		}
	}
	return false;
}

bool birdiebot_auth_host_allowed(const char *host, const char *whitelist,
                                 const char *local_hostname)
{
	if (host == NULL || *host == '\0') {
		return false;
	}
	if (birdiebot_auth_host_is_ip_literal(host)) {
		return true;
	}
	if (birdiebot_auth_host_in_list(host, whitelist)) {
		return true;
	}
	if (local_hostname != NULL && *local_hostname != '\0' &&
	    birdiebot_auth_host_equal(host, local_hostname)) {
		return true;
	}
	return false;
}

bool birdiebot_auth_origin_allowed(const char *origin, const char *host,
                                   const char *whitelist)
{
	if (origin == NULL || *origin == '\0') {
		return true; /* non-browser client: the token is still required */
	}
	if (host == NULL || *host == '\0') {
		return false;
	}

	char origin_host[BIRDIEBOT_AUTH_HOST_MAX];
	if (!birdiebot_auth_origin_host(origin, origin_host, sizeof(origin_host))) {
		return false;
	}
	if (birdiebot_auth_host_equal(origin_host, host)) {
		return true; /* same-origin request from the device UI */
	}
	return birdiebot_auth_origin_in_list(origin, whitelist);
}
