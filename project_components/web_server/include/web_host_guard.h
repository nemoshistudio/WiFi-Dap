/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_host_guard.h
 * @brief Browser Host/Origin guard for the web server (CSWSH / DNS rebinding).
 *
 * This is a browser-safety policy, NOT a login/auth mechanism (the Web UI
 * login feature has been removed). It is always active:
 *
 *  - Host header must be an IP literal, an entry of
 *    CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN, or CONFIG_LWIP_LOCAL_HOSTNAME, which
 *    blocks DNS rebinding.
 *  - Origin header, when present, must be same-origin with the request Host or
 *    match the allow-list (blocks cross-site WebSocket hijacking, CSWSH).
 *
 * Access through the device IP (the documented usage) needs no configuration;
 * hostname / mDNS / reverse-proxy access requires an allow-list entry.
 */
#ifndef WEB_HOST_GUARD_H_GUARD
#define WEB_HOST_GUARD_H_GUARD

#include <stdbool.h>

#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check the Host/Origin headers of an HTTP request.
 *
 * Used by /api and /api/ota/upload handlers.
 *
 * @return true when the request passes the browser-safety policy.
 */
bool web_host_guard_http_ok(httpd_req_t *req);

/**
 * @brief Check the Host/Origin headers of a WebSocket upgrade request (/ws).
 *
 * @return true when the upgrade may be accepted; on false the handler closes
 *         the session (the HTTP 101 upgrade already happened, so there is no
 *         HTTP 403 response any more).
 */
bool web_host_guard_ws_ok(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* WEB_HOST_GUARD_H_GUARD */
