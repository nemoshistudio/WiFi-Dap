/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_error_json.h
 * @brief Uniform web/WS error payload formatting (pure, host-testable).
 *
 * docs/protocol.md §1.1 freezes the failure frame shape:
 *   {"module":<u8>,"cmd":<u16>,"err":"<message>","code":<int>}
 * When the request context could not be parsed (bad JSON / oversized frame),
 * module/cmd are omitted: {"err":"<message>","code":<int>}.
 *
 * The front-end uses `code == 4` (Unsupported cmd) plus module/cmd to hide or
 * disable UI features that an older firmware does not implement (WP-209g /
 * F10), so every error that has a request context must carry module and cmd.
 */
#ifndef WEB_ERROR_JSON_H_GUARD
#define WEB_ERROR_JSON_H_GUARD

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Format a JSON error payload.
 *
 * @param buf      Output buffer (NUL terminated on success).
 * @param buf_len  Size of @p buf.
 * @param module   Request module id, or -1 when unknown.
 * @param cmd      Request cmd id, or -1 when unknown.
 * @param err      Error text (may be NULL -> "internal error"); JSON escaped.
 * @param code     docs/protocol.md §1.1 error code.
 * @return Number of bytes written (excluding NUL), or -1 when @p buf is too
 *         small / arguments invalid.
 */
int web_error_json_format(char *buf, size_t buf_len, int module, int cmd,
                          const char *err, int code);

#ifdef __cplusplus
}
#endif

#endif /* WEB_ERROR_JSON_H_GUARD */
