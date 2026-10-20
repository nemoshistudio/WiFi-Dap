/*
 * Pure logic for the multi WiFi credential memory (WP-201).
 *
 * This file must stay free of ESP-IDF (and FreeRTOS) dependencies so it can be
 * compiled and unit-tested on the host (Wave 3 / WP-210, acceptance item
 * "host 单元测试（WP-210/302）：淘汰策略、轮换排序（含 ts=0 平局）、last_idx
 * 修复、JSON 校验/错误码"). Only the C standard library is used here; the NVS
 * backend and task locking live in wifi_credential.c.
 *
 * Reference: docs/wifi-credential-memory.md §2.2/§2.4/§4.4/§5.1/§6.
 */

#include "wifi_credential.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(((birdiebot_wifi_cred_t *)0)->ssid) == BIRDIEBOT_WIFI_SSID_MAX + 1,
               "ssid buffer must hold the NUL terminator");
_Static_assert(sizeof(((birdiebot_wifi_cred_t *)0)->password) == BIRDIEBOT_WIFI_PASS_MAX + 1,
               "password buffer must hold the NUL terminator");

size_t wifi_cred_strnlen(const char *str, size_t max)
{
	size_t len = 0;

	if (str == NULL) {
		return 0;
	}
	while (len < max && str[len] != '\0') {
		++len;
	}
	return len;
}

bool wifi_cred_ssid_valid(const char *ssid)
{
	size_t len;

	if (ssid == NULL) {
		return false;
	}
	len = wifi_cred_strnlen(ssid, BIRDIEBOT_WIFI_SSID_MAX + 1);
	if (len == 0 || len > BIRDIEBOT_WIFI_SSID_MAX) {
		return false;
	}
	/* Reject embedded NUL truncation surprises: a 33rd byte must not exist. */
	return ssid[len] == '\0';
}

bool wifi_cred_password_valid(const char *password)
{
	size_t len;

	if (password == NULL) {
		return false;
	}
	len = wifi_cred_strnlen(password, BIRDIEBOT_WIFI_PASS_MAX + 1);
	if (len > BIRDIEBOT_WIFI_PASS_MAX) {
		return false; /* longer than 64 bytes */
	}
	if (len != 0 && len < BIRDIEBOT_WIFI_AP_PASS_MIN) {
		return false; /* 1..7 byte passwords are invalid (contract §7.2) */
	}
	return true;
}

bool wifi_cred_ap_password_valid(const char *password)
{
	size_t len;

	if (password == NULL) {
		return false;
	}
	len = wifi_cred_strnlen(password, BIRDIEBOT_WIFI_PASS_MAX + 1);
	return len >= BIRDIEBOT_WIFI_AP_PASS_MIN && len <= BIRDIEBOT_WIFI_AP_PASS_MAX;
}

int wifi_cred_ssid_copy(char dst[BIRDIEBOT_WIFI_SSID_MAX + 1], const char *src)
{
	size_t len;

	if (dst == NULL || !wifi_cred_ssid_valid(src)) {
		if (dst != NULL) {
			dst[0] = '\0';
		}
		return -1;
	}
	len = wifi_cred_strnlen(src, BIRDIEBOT_WIFI_SSID_MAX + 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
	return 0;
}

int wifi_cred_password_copy(char dst[BIRDIEBOT_WIFI_PASS_MAX + 1], const char *src)
{
	size_t len;

	if (dst == NULL || !wifi_cred_password_valid(src)) {
		if (dst != NULL) {
			dst[0] = '\0';
		}
		return -1;
	}
	len = wifi_cred_strnlen(src, BIRDIEBOT_WIFI_PASS_MAX + 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
	return 0;
}

int wifi_cred_find_ssid(const birdiebot_wifi_cred_t *table, uint8_t count, const char *ssid)
{
	if (table == NULL || ssid == NULL) {
		return -1;
	}
	if (count > BIRDIEBOT_WIFI_CRED_MAX) {
		count = BIRDIEBOT_WIFI_CRED_MAX;
	}
	for (uint8_t i = 0; i < count; ++i) {
		if (strncmp(table[i].ssid, ssid, BIRDIEBOT_WIFI_SSID_MAX + 1) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* Rotation ordering key (contract §5.1): last successful credential first,
 * then priority ascending, last_success_ts descending (0 last), index ascending.
 * @param pin_last true when last_idx is a real, non-skipped candidate. */
static bool cred_order_before(const birdiebot_wifi_cred_t *table, uint8_t lhs, uint8_t rhs,
                              bool pin_last, uint8_t last_idx)
{
	if (pin_last) {
		bool lhs_last = (lhs == last_idx);
		bool rhs_last = (rhs == last_idx);

		if (lhs_last != rhs_last) {
			return lhs_last;
		}
	}
	if (table[lhs].priority != table[rhs].priority) {
		return table[lhs].priority < table[rhs].priority;
	}
	if (table[lhs].last_success_ts != table[rhs].last_success_ts) {
		return table[lhs].last_success_ts > table[rhs].last_success_ts;
	}
	return lhs < rhs;
}

/* Eviction: priority descending (highest numeric = lowest priority first),
 * last_success_ts ascending (0 = oldest), index ascending. */
static bool cred_is_better_eviction(const birdiebot_wifi_cred_t *table, uint8_t lhs, uint8_t rhs)
{
	if (table[lhs].priority != table[rhs].priority) {
		return table[lhs].priority > table[rhs].priority;
	}
	if (table[lhs].last_success_ts != table[rhs].last_success_ts) {
		return table[lhs].last_success_ts < table[rhs].last_success_ts;
	}
	return lhs < rhs;
}

int wifi_cred_evict_index(const birdiebot_wifi_cred_t *table, uint8_t count, uint8_t protect_idx)
{
	int best = -1;

	if (table == NULL || count == 0) {
		return -1;
	}
	if (count > BIRDIEBOT_WIFI_CRED_MAX) {
		count = BIRDIEBOT_WIFI_CRED_MAX;
	}
	for (uint8_t i = 0; i < count; ++i) {
		if (i == protect_idx) {
			continue;
		}
		if (best < 0 || cred_is_better_eviction(table, i, (uint8_t)best)) {
			best = (int)i;
		}
	}
	/* count == 1: only the protected entry exists -> contract says it may be
	 * evicted "unless it is the only one"; the caller then keeps it. */
	return best;
}

int wifi_cred_rotate_order(const birdiebot_wifi_cred_t *table, uint8_t count, uint8_t last_idx,
                           const uint8_t *skip_mask, uint8_t *order, uint8_t max)
{
	uint8_t candidates[BIRDIEBOT_WIFI_CRED_MAX];
	uint8_t n = 0;
	uint8_t out;
	bool pin_last;

	if (table == NULL || order == NULL || max == 0 || count == 0) {
		return 0;
	}
	if (count > BIRDIEBOT_WIFI_CRED_MAX) {
		count = BIRDIEBOT_WIFI_CRED_MAX;
	}

	pin_last = (last_idx < count) &&
	           (skip_mask == NULL || skip_mask[last_idx] == 0);

	/* 1. collect *every* eligible candidate first (max may be < count). */
	for (uint8_t i = 0; i < count; ++i) {
		if (skip_mask != NULL && skip_mask[i] != 0) {
			continue;
		}
		candidates[n++] = i;
	}

	/* 2. fully sort all candidates; last success is ranked first, the rest by
	 * priority asc -> last_success_ts desc -> idx asc (contract §5.1).
	 * BIRDIEBOT_WIFI_CRED_MAX is small (<= 8): insertion sort. */
	for (uint8_t i = 1; i < n; ++i) {
		uint8_t key = candidates[i];
		uint8_t pos = i;

		while (pos > 0 &&
		       cred_order_before(table, key, candidates[pos - 1], pin_last, last_idx)) {
			candidates[pos] = candidates[pos - 1];
			--pos;
		}
		candidates[pos] = key;
	}

	/* 3. only now truncate to max, so the result is the true top-N.
	 * (The previous implementation truncated while inserting, which could
	 * return a non-top-N rotation order when max < count.) */
	out = (n < max) ? n : max;
	for (uint8_t i = 0; i < out; ++i) {
		order[i] = candidates[i];
	}
	return (int)out;
}

uint32_t wifi_cred_normalize_ts(uint64_t unix_seconds)
{
	if (unix_seconds < BIRDIEBOT_WIFI_CRED_TS_VALID_MIN) {
		return 0;
	}
	if (unix_seconds > UINT32_MAX) {
		return UINT32_MAX;
	}
	return (uint32_t)unix_seconds;
}

int wifi_cred_make_default_ap_ssid(const uint8_t mac[6], char out[BIRDIEBOT_WIFI_SSID_MAX + 1])
{
	int len;

	if (mac == NULL || out == NULL) {
		return -1;
	}
	/* "WiFi-Dap-XXXX", XXXX = last two MAC bytes, upper case hex (§6). */
	len = snprintf(out, BIRDIEBOT_WIFI_SSID_MAX + 1, "%s%02X%02X",
	               BIRDIEBOT_WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
	if (len < 0 || len > BIRDIEBOT_WIFI_SSID_MAX) {
		return -1;
	}
	return 0;
}

int wifi_cred_record_set(birdiebot_wifi_cred_t *record, const char *ssid, const char *password,
                         uint32_t last_success_ts, uint8_t priority)
{
	if (record == NULL) {
		return -1;
	}
	memset(record, 0, sizeof(*record));
	if (wifi_cred_ssid_copy(record->ssid, ssid) != 0) {
		return -1;
	}
	if (wifi_cred_password_copy(record->password, password) != 0) {
		return -1;
	}
	record->last_success_ts = last_success_ts;
	record->priority = priority;
	return 0;
}

int wifi_cred_record_pack(const birdiebot_wifi_cred_t *record, uint8_t *buf, size_t buf_len)
{
	if (record == NULL || buf == NULL || buf_len != sizeof(birdiebot_wifi_cred_t)) {
		return -1;
	}
	if (!wifi_cred_ssid_valid(record->ssid) || !wifi_cred_password_valid(record->password)) {
		return -1;
	}
	memcpy(buf, record, sizeof(*record));
	return 0;
}

int wifi_cred_record_unpack(const uint8_t *buf, size_t buf_len, birdiebot_wifi_cred_t *out)
{
	if (buf == NULL || out == NULL || buf_len != sizeof(birdiebot_wifi_cred_t)) {
		return -1;
	}
	memcpy(out, buf, sizeof(*out));
	/* §2.4.1: force NUL terminators in case a stale/foreign blob is not. */
	out->ssid[BIRDIEBOT_WIFI_SSID_MAX] = '\0';
	out->password[BIRDIEBOT_WIFI_PASS_MAX] = '\0';
	if (!wifi_cred_ssid_valid(out->ssid) || !wifi_cred_password_valid(out->password)) {
		return -1;
	}
	return 0;
}
