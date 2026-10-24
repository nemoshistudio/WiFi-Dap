/*
 * SPDX-License-Identifier: MIT
 *
 * P1-19 nearby-device discovery - mDNS `_http._tcp` query (read-only).
 *
 * The ESP32 already advertises its Web UI through project_components/wt_mdns
 * (`_http._tcp` with a TXT `name`).  This file uses the same ESP-IDF mDNS
 * component to look for other DAP devices on the LAN:
 *
 *   mdns_query_ptr("_http", "_tcp", 2000 ms, 8) -> SRV/TXT/A results
 *
 * Only real data is reported: name (TXT "name" or instance name), hostname
 * (SRV target), first IPv4 A record, port, plus optional TXT target/version.
 * mDNS carries no signal strength, so `rssi` stays omitted unless a TXT key
 * provides it in the future (never fabricated).  The device filters out its own
 * mDNS answer (hostname match) and deduplicates by (hostname, port).
 *
 * Blocking contract / re-entrancy: the query runs in the HTTP caller task for
 * at most `timeout_ms` (<= 2 s by default, one query at a time).  A second
 * concurrent call gets ESP_ERR_INVALID_STATE immediately (HTTP 429) instead of
 * queueing behind the mDNS lock.  If mDNS is not running (or the target has no
 * WiFi/mDNS) the result is an empty device list (HTTP 200), never an error.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <mdns.h>

#include "wt_target_flash.h"

#define DISCOVER_TAG "tf_discover"

/* Single-query guard (set while mdns_query_ptr() is running).  GCC atomics:
 * the httpd task and any future caller must observe the flag without a lock. */
static uint8_t s_discover_busy;

/** Copy a possibly non-NUL-terminated mDNS string, keeping valid UTF-8 bytes. */
static void copy_text(char *dst, size_t dst_len, const char *src,
                      size_t src_len)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    if (src_len >= dst_len) {
        src_len = dst_len - 1U;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

/** Find one TXT value by key (case-insensitive key compare). */
static bool txt_lookup(const mdns_result_t *res, const char *key, char *dst,
                       size_t dst_len)
{
    if (res == NULL || res->txt == NULL || key == NULL) {
        return false;
    }
    for (size_t i = 0U; i < res->txt_count; i++) {
        const char *item_key = res->txt[i].key;
        size_t value_len;

        if (item_key == NULL || strcasecmp(item_key, key) != 0) {
            continue;
        }
        value_len = (res->txt_value_len != NULL)
                        ? (size_t)res->txt_value_len[i]
                        : strlen(res->txt[i].value != NULL ? res->txt[i].value
                                                           : "");
        copy_text(dst, dst_len, res->txt[i].value, value_len);
        return true;
    }
    return false;
}

/** First IPv4 A record as "a.b.c.d" (empty when none). */
static bool first_ipv4(const mdns_result_t *res, char *dst, size_t dst_len)
{
    for (const mdns_ip_addr_t *addr = res->addr; addr != NULL;
         addr = addr->next) {
        if (addr->addr.type != ESP_IPADDR_TYPE_V4) {
            continue;
        }
        (void)snprintf(dst, dst_len, "%u.%u.%u.%u",
                       (unsigned)esp_ip4_addr1_16(&addr->addr.u_addr.ip4),
                       (unsigned)esp_ip4_addr2_16(&addr->addr.u_addr.ip4),
                       (unsigned)esp_ip4_addr3_16(&addr->addr.u_addr.ip4),
                       (unsigned)esp_ip4_addr4_16(&addr->addr.u_addr.ip4));
        return dst[0] != '\0';
    }
    dst[0] = '\0';
    return false;
}

/** Strip a trailing '.', ".local" / ".local." suffix for comparison. */
static void normalize_hostname(const char *host, char *dst, size_t dst_len)
{
    size_t len;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    copy_text(dst, dst_len, host, host != NULL ? strlen(host) : 0U);
    len = strlen(dst);
    while (len > 0U && dst[len - 1U] == '.') {
        dst[--len] = '\0';
    }
    if (len > 6U && strcasecmp(dst + len - 6U, ".local") == 0) {
        dst[len - 6U] = '\0';
    }
}

/** Case-insensitive ASCII substring test (newlib may not provide strcasestr
 *  without _GNU_SOURCE; only ASCII markers are matched case-insensitively). */
static bool contains_ci(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    needle_len = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0U;

        while (i < needle_len && p[i] != '\0' &&
               tolower((unsigned char)p[i]) ==
                   tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

/** True when the TXT/name/hostname looks like this project's DAP.
 *
 * `_http._tcp` is a generic service type (routers, printers, NAS boxes also
 * advertise it), so a plain query would fill the Web "nearby devices" list
 * with unrelated web UIs.  The project's wt_mdns advertisement publishes a
 * TXT "name" (human readable, contains "WiFi Dap"/"调试器") and a
 * "wifi-dap-XXXX" hostname; older/custom builds may not, hence the
 * marker/TXT-key fallbacks.  A dedicated TXT marker or service type in
 * wt_mdns would make this exact (out of this task's write scope).
 */
static bool looks_like_dap(const mdns_result_t *res, const char *hostname)
{
    char scratch[WT_TF_DISCOVER_NAME_MAX];

    if (hostname != NULL && strncasecmp(hostname, "wifi-dap", 13) == 0) {
        return true;
    }
    if (txt_lookup(res, "name", scratch, sizeof(scratch))) {
        if (contains_ci(scratch, "birdiebot") ||
            strstr(scratch, "\xe8\xb0\x83\xe8\xaf\x95\xe5\x99\xa8") != NULL) {
            return true;
        }
    }
    /* Forward-compatible enrichment keys that only a DAP would publish. */
    if (txt_lookup(res, "target", scratch, sizeof(scratch)) ||
        txt_lookup(res, "chip", scratch, sizeof(scratch)) ||
        txt_lookup(res, "dap", scratch, sizeof(scratch)) ||
        txt_lookup(res, "usbip", scratch, sizeof(scratch))) {
        return true;
    }
    return false;
}

static bool already_listed(const wt_tf_discover_device_t *out, size_t count,
                           const char *hostname, const char *ip, uint16_t port)
{
    for (size_t i = 0U; i < count; i++) {
        if (out[i].port != port) {
            continue;
        }
        if (hostname[0] != '\0' && out[i].hostname[0] != '\0') {
            if (strcasecmp(out[i].hostname, hostname) == 0) {
                return true;
            }
        } else if (ip[0] != '\0' && strcmp(out[i].ip, ip) == 0) {
            return true;
        }
    }
    return false;
}

/** Query without the busy guard: caller must hold s_discover_busy. */
static int discover_query(wt_tf_discover_device_t *out, size_t max_devices,
                          uint32_t timeout_ms)
{
    mdns_result_t *results = NULL;
    /* mdns_hostname_get() requires a buffer of at least MDNS_NAME_BUF_LEN. */
    char self_host[MDNS_NAME_BUF_LEN];
    char self_norm[MDNS_NAME_BUF_LEN];
    size_t count = 0U;
    esp_err_t err;

    if (out == NULL || max_devices == 0U) {
        return -(int)ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, max_devices * sizeof(*out));
    if (timeout_ms == 0U) {
        timeout_ms = WT_TF_DISCOVER_TIMEOUT_MS;
    }
    if (timeout_ms < 100U) {
        timeout_ms = 100U;
    }
    if (timeout_ms > 5000U) {
        timeout_ms = 5000U;
    }

    self_host[0] = '\0';
    self_norm[0] = '\0';
    if (mdns_hostname_get(self_host) == ESP_OK) {
        normalize_hostname(self_host, self_norm, sizeof(self_norm));
    }

    err = mdns_query_ptr("_http", "_tcp", timeout_ms, WT_TF_DISCOVER_MAX,
                         &results);
    if (err != ESP_OK) {
        /* mDNS not initialized (WiFi off / target without mDNS): a discovery
         * poll is not a fault, answer an empty list. */
        ESP_LOGD(DISCOVER_TAG, "mDNS query unavailable: %s",
                 esp_err_to_name(err));
        return 0;
    }

    for (mdns_result_t *res = results; res != NULL && count < max_devices;
         res = res->next) {
        wt_tf_discover_device_t *dev = &out[count];
        char hostname[WT_TF_DISCOVER_HOST_MAX];
        char norm[WT_TF_DISCOVER_HOST_MAX];

        if (res->port == 0U) {
            continue;
        }
        normalize_hostname(res->hostname, norm, sizeof(norm));
        if (norm[0] == '\0') {
            continue; /* a usable host is mandatory for the Web to switch */
        }
        if (self_norm[0] != '\0' && strcasecmp(norm, self_norm) == 0) {
            continue; /* our own advertisement */
        }
        normalize_hostname(res->hostname, hostname, sizeof(hostname));
        if (!looks_like_dap(res, hostname)) {
            continue; /* generic _http._tcp peer, not a WiFi Dap */
        }
        if (!first_ipv4(res, dev->ip, sizeof(dev->ip))) {
            continue; /* hostname may not resolve on every LAN; require A */
        }
        if (already_listed(out, count, hostname, dev->ip, res->port)) {
            continue;
        }
        copy_text(dev->hostname, sizeof(dev->hostname), hostname,
                  strlen(hostname));
        dev->port = res->port;
        if (txt_lookup(res, "name", dev->name, sizeof(dev->name))) {
            /* TXT name (the project advertises a human-readable name). */
        } else if (res->instance_name != NULL) {
            copy_text(dev->name, sizeof(dev->name), res->instance_name,
                      strlen(res->instance_name));
        } else {
            copy_text(dev->name, sizeof(dev->name), hostname, strlen(hostname));
        }
        /* Optional TXT enrichment (not advertised today -> empty/omitted). */
        (void)txt_lookup(res, "target", dev->target, sizeof(dev->target));
        if (dev->target[0] == '\0') {
            (void)txt_lookup(res, "chip", dev->target, sizeof(dev->target));
        }
        (void)txt_lookup(res, "version", dev->version, sizeof(dev->version));
        if (dev->version[0] == '\0') {
            (void)txt_lookup(res, "fw", dev->version, sizeof(dev->version));
        }
        /* mDNS has no RSSI: keep has_rssi=false (field omitted in JSON). */
        count++;
    }
    if (results != NULL) {
        mdns_query_results_free(results);
    }
    ESP_LOGI(DISCOVER_TAG, "mDNS discovery: %u nearby device(s)",
             (unsigned)count);
    return (int)count;
}

int wt_tf_discover(wt_tf_discover_device_t *out, size_t max_devices,
                   uint32_t timeout_ms)
{
    int count;

    if (__atomic_test_and_set(&s_discover_busy, __ATOMIC_ACQ_REL)) {
        ESP_LOGW(DISCOVER_TAG, "discovery busy, refusing concurrent query");
        return -(int)ESP_ERR_INVALID_STATE;
    }
    count = discover_query(out, max_devices, timeout_ms);
    __atomic_clear(&s_discover_busy, __ATOMIC_RELEASE);
    return count;
}

esp_err_t wt_tf_discover_json(char *buf, size_t buf_len, uint32_t timeout_ms,
                              size_t *out_len)
{
    static wt_tf_discover_device_t devices[WT_TF_DISCOVER_MAX];
    int count;
    int written;
    esp_err_t result = ESP_OK;

    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (buf == NULL || buf_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Keep the single-query guard across query AND formatting: the device
     * array is static, so a concurrent caller must not rewrite it while this
     * response is being formatted. */
    if (__atomic_test_and_set(&s_discover_busy, __ATOMIC_ACQ_REL)) {
        return ESP_ERR_INVALID_STATE;
    }
    count = discover_query(devices, WT_TF_DISCOVER_MAX, timeout_ms);
    if (count < 0) {
        result = (esp_err_t)(-count);
    } else {
        written = wt_tf_discover_format_json(devices, (size_t)count, buf,
                                             buf_len);
        if (written < 0) {
            /* Should never happen: the buffer is sized for 8 max-length rows.
             * Fall back to the valid empty response instead of failing. */
            written = snprintf(buf, buf_len, "{\"ok\":true,\"devices\":[]}");
            if (written < 0 || (size_t)written >= buf_len) {
                result = ESP_ERR_INVALID_SIZE;
            }
        }
        if (result == ESP_OK) {
            if (out_len != NULL) {
                *out_len = (size_t)written;
            }
        }
    }
    __atomic_clear(&s_discover_busy, __ATOMIC_RELEASE);
    return result;
}
