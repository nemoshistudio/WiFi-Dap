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
 * login feature has been removed). It is switchable:
 *
 *  - CONFIG_BIRDIEBOT_WEB_HOST_GUARD=y (full-feature build):
 *      Host header must be an IP literal, an entry of
 *      CONFIG_BIRDIEBOT_WEB_ALLOWED_ORIGIN, or CONFIG_LWIP_LOCAL_HOSTNAME,
 *      which blocks DNS rebinding. Origin header, when present, must be
 *      same-origin with the request Host or match the allow-list (blocks
 *      cross-site WebSocket hijacking, CSWSH).
 *  - CONFIG_BIRDIEBOT_WEB_HOST_GUARD=n (slim build default, 2026-10-26):
 *      every Host/Origin is accepted (the call sites are compiled to true).
 *      SECURITY RISK: no CSWSH/DNS-rebinding protection; deploy only on a
 *      trusted lab LAN (docs/security.md, docs/feature-flags.md M1).
 *
 * Access through the device IP (the documented usage) needs no configuration;
 * hostname / mDNS / reverse-proxy access requires an allow-list entry in the
 * full-feature build.
 */
#ifndef WEB_HOST_GUARD_H_GUARD
#define WEB_HOST_GUARD_H_GUARD

#include <stdbool.h>

#include <esp_http_server.h>
#include <sdkconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_BIRDIEBOT_WEB_HOST_GUARD)

/**
 * @brief Check the Host/Origin headers of an HTTP request.
 *
 * Used by the /api and /api/target handlers.
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

#else /* !CONFIG_BIRDIEBOT_WEB_HOST_GUARD */

/* Slim build (M1): the implementation file is compiled out and the handler
 * call sites evaluate to "allowed".  Keeping the same expression means the
 * source of every handler is unchanged, so re-enabling the guard is a single
 * Kconfig switch (docs/feature-flags.md M1). */
#define web_host_guard_http_ok(req) (true)
#define web_host_guard_ws_ok(req) (true)

#endif /* CONFIG_BIRDIEBOT_WEB_HOST_GUARD */

#ifdef __cplusplus
}
#endif

#endif /* WEB_HOST_GUARD_H_GUARD */
