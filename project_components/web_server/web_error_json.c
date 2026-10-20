/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_error_json.c
 * @brief Uniform web/WS error payload formatting (pure, host-testable).
 */
#include "web_error_json.h"

#include <stdio.h>
#include <string.h>

/** Copy @p src into @p dst with JSON string escaping; returns bytes written. */
static int json_escape(char *dst, size_t dst_len, const char *src)
{
	size_t o = 0;

	if (dst_len == 0) {
		return -1;
	}
	for (const char *p = src; *p != '\0'; ++p) {
		char c = *p;
		if (c == '"' || c == '\\') {
			if (o + 2 >= dst_len) {
				return -1;
			}
			dst[o++] = '\\';
			dst[o++] = c;
		} else if ((unsigned char) c < 0x20) {
			/* Skip control characters: they are invalid in JSON strings. */
			continue;
		} else {
			if (o + 1 >= dst_len) {
				return -1;
			}
			dst[o++] = c;
		}
	}
	dst[o] = '\0';
	return (int) o;
}

int web_error_json_format(char *buf, size_t buf_len, int module, int cmd,
                          const char *err, int code)
{
	const char *text = (err != NULL && err[0] != '\0') ? err : "internal error";
	char escaped[192];
	int n;

	if (buf == NULL || buf_len == 0) {
		return -1;
	}
	if (json_escape(escaped, sizeof(escaped), text) < 0) {
		return -1;
	}

	if (module >= 0 && cmd >= 0) {
		n = snprintf(buf, buf_len,
		             "{\"module\":%d,\"cmd\":%d,\"err\":\"%s\",\"code\":%d}",
		             module, cmd, escaped, code);
	} else {
		/* No parseable request context (bad JSON / bad binary frame). */
		n = snprintf(buf, buf_len, "{\"err\":\"%s\",\"code\":%d}", escaped, code);
	}
	if (n < 0 || (size_t) n >= buf_len) {
		return -1;
	}
	return n;
}
