/*
 * SPDX-License-Identifier: MIT
 *
 * P1-19 nearby-device discovery - pure JSON response formatter.
 *
 * Kept free of ESP-IDF/mDNS includes so the exact wire shape is covered by the
 * host regression test (components/target_flash/test/host/run.sh) without a
 * device.  The mDNS query itself lives in tf_discover.c.
 *
 * Response contract (GET /api/discover, HTTP 200; consumed by Web P1-19):
 *
 *   {"ok":true,"devices":[
 *     {"name":"...","hostname":"...","ip":"<device-ip>","port":80,
 *      "rssi":-52,              // omitted when not measured (mDNS has no RSSI)
 *      "target":"STM32H7",      // omitted unless the advertiser publishes TXT
 *      "version":"v1.2.0"}      // ditto
 *   , ...]}
 *
 *   - failure/no result: {"ok":true,"devices":[]} (HTTP 200, never fake rows)
 *   - fields that were not measured are omitted, never null/0 placeholders
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wt_target_flash.h"

/** Append one escaped JSON string body (no surrounding quotes). */
static bool json_escape_append(char *buf, size_t buf_len, size_t *offset,
                               const char *text)
{
    static const char hex[] = "0123456789abcdef";
    size_t off = *offset;

    if (text == NULL) {
        text = "";
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        unsigned char c = *p;

        if (c == '"' || c == '\\') {
            if (off + 3U > buf_len) {
                return false;
            }
            buf[off++] = '\\';
            buf[off++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (off + 3U > buf_len) {
                return false;
            }
            buf[off++] = '\\';
            buf[off++] = (c == '\n') ? 'n' : ((c == '\r') ? 'r' : 't');
        } else if (c < 0x20U) {
            if (off + 7U > buf_len) {
                return false;
            }
            buf[off++] = '\\';
            buf[off++] = 'u';
            buf[off++] = '0';
            buf[off++] = '0';
            buf[off++] = hex[(c >> 4) & 0x0FU];
            buf[off++] = hex[c & 0x0FU];
        } else {
            if (off + 2U > buf_len) {
                return false;
            }
            buf[off++] = (char)c;
        }
    }
    buf[off] = '\0';
    *offset = off;
    return true;
}

/** Append formatted text; returns false on truncation. */
static bool json_appendf(char *buf, size_t buf_len, size_t *offset,
                         const char *fmt, ...)
{
    va_list ap;
    int written;
    size_t remain = buf_len - *offset;

    va_start(ap, fmt);
    written = vsnprintf(buf + *offset, remain, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= remain) {
        return false;
    }
    *offset += (size_t)written;
    return true;
}

/** Append `"key":"value"` (value escaped); key is a trusted literal. */
static bool json_append_str(char *buf, size_t buf_len, size_t *offset,
                            const char *key, const char *value)
{
    if (!json_appendf(buf, buf_len, offset, "\"%s\":\"", key)) {
        return false;
    }
    if (!json_escape_append(buf, buf_len, offset, value)) {
        return false;
    }
    return json_appendf(buf, buf_len, offset, "\"");
}

int wt_tf_discover_format_json(const wt_tf_discover_device_t *devices,
                               size_t count, char *buf, size_t buf_len)
{
    size_t off = 0U;

    if (buf == NULL || buf_len == 0U) {
        return -1;
    }
    if (devices == NULL) {
        count = 0U;
    }
    if (!json_appendf(buf, buf_len, &off, "{\"ok\":true,\"devices\":[")) {
        return -1;
    }
    for (size_t i = 0U; i < count; i++) {
        const wt_tf_discover_device_t *dev = &devices[i];

        if (i > 0U && !json_appendf(buf, buf_len, &off, ",")) {
            return -1;
        }
        if (!json_appendf(buf, buf_len, &off, "{")) {
            return -1;
        }
        /* name is always a string (possibly empty when the advertiser had no
         * instance name; the Web falls back to the host in that case). */
        if (!json_append_str(buf, buf_len, &off, "name", dev->name) ||
            !json_appendf(buf, buf_len, &off, ",") ||
            !json_append_str(buf, buf_len, &off, "hostname", dev->hostname) ||
            !json_appendf(buf, buf_len, &off, ",") ||
            !json_append_str(buf, buf_len, &off, "ip", dev->ip) ||
            !json_appendf(buf, buf_len, &off, ",\"port\":%u",
                          (unsigned)dev->port)) {
            return -1;
        }
        if (dev->has_rssi &&
            !json_appendf(buf, buf_len, &off, ",\"rssi\":%d", (int)dev->rssi)) {
            return -1;
        }
        if (dev->target[0] != '\0' &&
            (!json_appendf(buf, buf_len, &off, ",") ||
             !json_append_str(buf, buf_len, &off, "target", dev->target))) {
            return -1;
        }
        if (dev->version[0] != '\0' &&
            (!json_appendf(buf, buf_len, &off, ",") ||
             !json_append_str(buf, buf_len, &off, "version", dev->version))) {
            return -1;
        }
        if (!json_appendf(buf, buf_len, &off, "}")) {
            return -1;
        }
    }
    if (!json_appendf(buf, buf_len, &off, "]}")) {
        return -1;
    }
    return (int)off;
}
