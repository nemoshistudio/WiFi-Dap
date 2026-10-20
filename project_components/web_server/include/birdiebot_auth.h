/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file birdiebot_auth.h
 * @brief WiFi Dap Web/API token authentication (WP-209b).
 *
 * Contract: docs/protocol.md §1/§5, docs/versioning.md.  Disabled by default
 * (`CONFIG_BIRDIEBOT_WEB_AUTH=n`): every check below returns true and the
 * request path is unchanged (laboratory mode).
 *
 * Host/Origin validation is ALWAYS enforced (also with auth disabled, security
 * review P1-2): browsers can open cross-origin WebSockets without a CORS
 * preflight (CSWSH) and DNS rebinding works without any token, so /ws upgrades,
 * /api and /api/ota/upload reject Hosts that are not an IP literal / allow-list
 * entry and Origins that are not same-origin / allow-listed.  TCP 3240
 * (USBIP/WS-DAP) and TCP 1346 (UART bridge) are NOT covered by this component.
 *
 * When the token auth is enabled (`CONFIG_BIRDIEBOT_WEB_AUTH=y`):
 *   - WS `/ws`: `?token=<pair_code>` on the connect URL and/or the first frame
 *     `{"module":0,"cmd":0,"token":"..."}` (SYSTEM cmd 0 AUTH). State is kept
 *     per socket fd and cleared when the WS connection closes.
 *   - HTTP `/api` (and future `/api/ota/upload`): `Authorization: Bearer
 *     <pair_code>` or `?token=`. Rejected requests get HTTP 401 + JSON
 *     `{"err":"unauthorized","code":7}`.
 *   - Host/Origin headers are validated against the request host, the
 *     configured allow-list and CONFIG_LWIP_LOCAL_HOSTNAME to block DNS
 *     rebinding (see project_components/web_server/Kconfig).  This check runs
 *     even when the token auth is disabled.
 *   - The pairing token is 128-bit (32 hex chars) when auto-generated and is
 *     persisted in wt_nvs; AUTH failures are rate limited per socket
 *     (5/minute) and the comparison is constant time.
 *
 * Public header so other modules (e.g. a future uri_ota.c) can reuse:
 *   if (!birdiebot_auth_http_ok(req)) { ... 401 ... }
 */
#ifndef BIRDIEBOT_AUTH_H_GUARD
#define BIRDIEBOT_AUTH_H_GUARD

#include <stdbool.h>

#include <esp_http_server.h>

/* sdkconfig is needed for the compile-time switch below; skip it in host
 * syntax checks (the header must stay includable outside an IDF build). */
#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

/* 1 when Kconfig CONFIG_BIRDIEBOT_WEB_AUTH is set; call sites can use `#if
 * BIRDIEBOT_AUTH_ENABLED` to compile the auth paths out entirely (zero
 * overhead / zero behaviour change in the default laboratory mode). */
#ifndef BIRDIEBOT_AUTH_ENABLED
#  if defined(CONFIG_BIRDIEBOT_WEB_AUTH)
#    define BIRDIEBOT_AUTH_ENABLED 1
#  else
#    define BIRDIEBOT_AUTH_ENABLED 0
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @return true when CONFIG_BIRDIEBOT_WEB_AUTH is enabled. */
bool birdiebot_auth_enabled(void);

/**
 * @brief Initialize the token and the WS auth state (idempotent).
 *
 * Called by start_webserver() before any request can reach a handler. When
 * CONFIG_BIRDIEBOT_WEB_TOKEN is empty a random 8-hex-digit token is generated
 * and persisted in the wt_nvs partition (namespace "web_auth", key "token");
 * the effective token is printed on the serial console as the pairing code.
 */
void birdiebot_auth_init(void);

/** HTTP check result codes (docs/ota-local-upload.md §3.4: 401 vs 403). */
typedef enum {
	BIRDIEBOT_AUTH_HTTP_OK = 0,
	BIRDIEBOT_AUTH_HTTP_UNAUTHORIZED = 401, /* token missing/invalid */
	BIRDIEBOT_AUTH_HTTP_FORBIDDEN = 403,    /* Host/Origin rejected */
} birdiebot_auth_http_result_t;

/**
 * @brief Full HTTP request check used by /api handlers (incl. OTA upload).
 *
 * Verifies Host/Origin headers and the token (Authorization: Bearer or
 * ?token=).
 *
 * @return true when auth is disabled, or the request passed all checks.
 */
bool birdiebot_auth_http_ok(httpd_req_t *req);

/**
 * @brief Same check as birdiebot_auth_http_ok(), but reports the 401/403 split.
 *
 * @return BIRDIEBOT_AUTH_HTTP_OK (0), BIRDIEBOT_AUTH_HTTP_UNAUTHORIZED (401)
 *         or BIRDIEBOT_AUTH_HTTP_FORBIDDEN (403). Returns OK when auth is
 *         disabled.
 */
int birdiebot_auth_http_check(httpd_req_t *req);

/**
 * @brief WS handshake Host/Origin check (always enforced, also with auth=n).
 *
 * @return true when the headers pass the checks. On false the WS connection
 *         must be closed (HTTP 101 was already sent by the httpd handshake, so
 *         the session is closed instead of returning 403).
 */
bool birdiebot_auth_ws_origin_ok(httpd_req_t *req);

/**
 * @brief Host/Origin check for any HTTP request (always enforced).
 *
 * Same policy as birdiebot_auth_ws_origin_ok(); provided for endpoint handlers
 * that only need the CSWSH/DNS-rebinding part (auth-less lab mode).
 */
bool birdiebot_auth_host_origin_ok(httpd_req_t *req);

/**
 * @brief Validate a `?token=` query parameter at WS handshake time.
 *
 * A missing token is not an error (the client may send the AUTH frame later).
 *
 * @return 1 no token present (connection stays pending), 0 token valid (fd is
 *         marked authorized), -1 token present but invalid.
 */
int birdiebot_auth_ws_query_ok(httpd_req_t *req, int fd);

/**
 * @brief Validate the token of a first-frame SYSTEM/0/AUTH request.
 *
 * Marks the fd authorized on success.
 *
 * @return true when the token matches (fd is authorized).
 */
bool birdiebot_auth_ws_token_ok(const char *token, int fd);

/** @return true when auth is disabled or @p fd is authorized. */
bool birdiebot_auth_ws_fd_ok(int fd);

/** @brief Reset the auth state of @p fd (called on WS handshake). */
void birdiebot_auth_ws_open(int fd);

/** @brief Clear the auth state of @p fd (called on WS close / dead socket). */
void birdiebot_auth_ws_close(int fd);

/**
 * @brief Record one failed AUTH attempt on @p fd (rate limiting, P1-4).
 *
 * @return true when the fd is now rate limited (>5 failures/minute); the caller
 *         should answer with code 7 and may add a short delay.
 */
bool birdiebot_auth_ws_note_auth_failure(int fd);

/** @brief Reset the AUTH failure counter of @p fd. */
void birdiebot_auth_ws_reset_auth_failures(int fd);

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_AUTH_H_GUARD */
