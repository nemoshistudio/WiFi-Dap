/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file birdiebot_auth_util.h
 * @brief Web auth pure helpers (no ESP-IDF dependency; host-unit-testable).
 *
 * These functions implement the string parsing used by the Web auth backend
 * (project_components/web_server/birdiebot_auth.c) so they can be compiled and
 * tested on the host:
 *
 *   gcc -I project_components/web_server/include \
 *       project_components/web_server/auth_token_util.c tests/host/auth_util_test.c
 *
 * Contract: docs/protocol.md §1/§5 (token via ?token= / Authorization: Bearer,
 * Host/Origin validation to block DNS rebinding).
 */
#ifndef BIRDIEBOT_AUTH_UTIL_H_GUARD
#define BIRDIEBOT_AUTH_UTIL_H_GUARD

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum accepted pairing-token length (mirrors web/src/composables/websocket/auth.ts). */
#define BIRDIEBOT_AUTH_TOKEN_MAX 256

/** Maximum hostname/authority length handled by the helpers. */
#define BIRDIEBOT_AUTH_HOST_MAX 128

/**
 * @brief Compare two pairing tokens in constant time.
 *
 * @return true when both are non-NULL, non-empty, equal and not longer than
 *         BIRDIEBOT_AUTH_TOKEN_MAX.
 */
bool birdiebot_auth_token_equal(const char *expected, const char *provided);

/**
 * @brief Extract the token from a query string such as "/ws?token=abc&x=1".
 *
 * Percent-encoded bytes (%XX) are decoded, '+' is kept literally (encodeURIComponent
 * does not emit '+'). The output is always NUL terminated on success.
 *
 * @param uri      Request URI (NUL terminated).
 * @param out      Token output buffer.
 * @param out_len  Size of @p out (at least 2).
 * @return 1 found, 0 not found, -1 found but malformed/too long.
 */
int birdiebot_auth_extract_query_token(const char *uri, char *out, size_t out_len);

/**
 * @brief Extract the token from an "Authorization: Bearer <token>" header value.
 *
 * The scheme is matched case-insensitively, leading/trailing spaces are trimmed.
 *
 * @return 1 extracted, 0 not a Bearer header, -1 malformed/empty/too long.
 */
int birdiebot_auth_extract_bearer(const char *authorization, char *out, size_t out_len);

/**
 * @brief Split "host[:port]" / "[ipv6][:port]" into the bare host.
 *
 * Brackets around IPv6 literals are removed; the port is discarded (it is not
 * needed for same-origin comparison here). Surrounding spaces are trimmed.
 *
 * @return true when a non-empty host was extracted.
 */
bool birdiebot_auth_split_authority(const char *authority, char *host, size_t host_len);

/** @return true when @p host is an IPv4 literal (0-255 octets) or contains ':'. */
bool birdiebot_auth_host_is_ip_literal(const char *host);

/**
 * @brief Case-insensitive host comparison (trailing dot ignored).
 */
bool birdiebot_auth_host_equal(const char *a, const char *b);

/**
 * @brief Extract the host from an Origin header value ("scheme://host[:port]").
 *
 * A value without a scheme is treated as a bare authority. A path/query/fragment
 * suffix is ignored; userinfo is discarded.
 *
 * @return true when a non-empty host was extracted.
 */
bool birdiebot_auth_origin_host(const char *origin, char *host, size_t host_len);

/**
 * @brief Case-insensitive full-origin comparison (trailing '/' ignored).
 */
bool birdiebot_auth_origin_equal(const char *a, const char *b);

/**
 * @brief Whether @p host matches one entry of a comma-separated allow-list.
 *
 * Entries may be full origins or bare hostnames; spaces around entries are
 * trimmed. An empty/NULL list never matches.
 */
bool birdiebot_auth_host_in_list(const char *host, const char *whitelist);

/**
 * @brief Whether @p origin matches one entry of a comma-separated allow-list.
 *
 * Entries with a scheme are compared as full origins; bare-host entries match
 * the origin host only. An empty/NULL list never matches.
 */
bool birdiebot_auth_origin_in_list(const char *origin, const char *whitelist);

/**
 * @brief Host-header policy used when web auth is enabled (DNS-rebinding guard).
 *
 * A host is allowed when it is an IP literal (the documented access path), an
 * entry of @p whitelist, or equals @p local_hostname (CONFIG_LWIP_LOCAL_HOSTNAME,
 * may be NULL/empty). Hostnames resolving to the device from attacker-controlled
 * DNS are rejected.
 */
bool birdiebot_auth_host_allowed(const char *host, const char *whitelist,
                                 const char *local_hostname);

/**
 * @brief Origin-header policy used when web auth is enabled.
 *
 * A missing Origin (non-browser clients) is allowed. When present, the Origin
 * must be same-origin with the request Host or match @p whitelist.
 */
bool birdiebot_auth_origin_allowed(const char *origin, const char *host,
                                   const char *whitelist);

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_AUTH_UTIL_H_GUARD */
