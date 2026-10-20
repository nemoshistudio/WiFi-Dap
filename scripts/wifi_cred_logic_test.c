/*
 * Host unit test for the WiFi credential pure logic (WP-201 / WP-210 input).
 *
 * Build & run via scripts/test_wifi_cred_logic.sh (host gcc/clang, no ESP-IDF,
 * no Docker, no hardware). Output is TAP-ish so it can be wrapped by CI later.
 *
 * The test only uses project_components/wifi_manager/wifi_cred_logic.c, which is
 * deliberately free of ESP-IDF/FreeRTOS dependencies. It covers the acceptance
 * items of docs/wifi-credential-memory.md §10:
 *   - 103-byte packed blob layout / pack / unpack / forced NUL repair
 *   - SSID / password / SoftAP password validation and bounded copies
 *   - eviction policy (priority max -> oldest last_success, current one protected)
 *   - rotation order (last success first, priority asc, last_success desc,
 *     ts=0 ties by index, skip mask, max cap, invalid last_idx fallback)
 *   - last_success_ts normalization (unsynced device stores 0)
 *   - default AP SSID "WiFi-Dap-XXXX" (last two STA MAC bytes, upper case)
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_credential.h"

static int g_test_no;
static int g_failed;

#define CHECK(cond, name)                                                       \
	do {                                                                    \
		++g_test_no;                                                    \
		if (cond) {                                                     \
			printf("ok %d - %s\n", g_test_no, name);                \
		} else {                                                        \
			printf("not ok %d - %s\n", g_test_no, name);            \
			printf("#   at %s:%d\n", __FILE__, __LINE__);           \
			++g_failed;                                             \
		}                                                               \
	} while (0)

static void group(const char *name)
{
	printf("# ---------------------------------------------------------------------\n");
	printf("# %s\n", name);
}

static birdiebot_wifi_cred_t make_cred(const char *ssid, const char *password,
                                       uint32_t last_success_ts, uint8_t priority)
{
	birdiebot_wifi_cred_t record;

	if (wifi_cred_record_set(&record, ssid, password, last_success_ts, priority) != 0) {
		printf("Bail out! test setup: record_set rejected valid input\n");
		exit(1);
	}
	return record;
}

static void test_layout(void)
{
	birdiebot_wifi_cred_t record;

	group("record layout (docs/wifi-credential-memory.md §2.2)");
	memset(&record, 0, sizeof(record));
	CHECK(sizeof(record) == 103, "sizeof(birdiebot_wifi_cred_t) == 103");
	CHECK(offsetof(birdiebot_wifi_cred_t, ssid) == 0, "ssid is at offset 0");
	CHECK(offsetof(birdiebot_wifi_cred_t, password) == 33, "password is at offset 33");
	CHECK(offsetof(birdiebot_wifi_cred_t, last_success_ts) == 98,
	      "last_success_ts is at offset 98");
	CHECK(offsetof(birdiebot_wifi_cred_t, priority) == 102, "priority is at offset 102");
}

static void test_validation(void)
{
	const char *ssid_32 = "12345678901234567890123456789012";      /* 32 */
	const char *ssid_33 = "123456789012345678901234567890123";     /* 33 */
	const char *pass_8 = "12345678";
	const char *pass_64 =
		"1234567890123456789012345678901234567890123456789012345678901234"; /* 64 */
	const char *pass_65 =
		"12345678901234567890123456789012345678901234567890123456789012345"; /* 65 */

	group("validation (contract §7.2)");
	CHECK(wifi_cred_strnlen("abc", 5) == 3, "strnlen stops at NUL");
	CHECK(wifi_cred_strnlen("abc", 2) == 2, "strnlen honours the max bound");
	CHECK(wifi_cred_strnlen(NULL, 4) == 0, "strnlen(NULL) == 0");

	CHECK(wifi_cred_ssid_valid("a"), "SSID 1 byte accepted");
	CHECK(wifi_cred_ssid_valid(ssid_32), "SSID 32 bytes accepted");
	CHECK(!wifi_cred_ssid_valid(""), "empty SSID rejected");
	CHECK(!wifi_cred_ssid_valid(ssid_33), "SSID 33 bytes rejected");
	CHECK(!wifi_cred_ssid_valid(NULL), "NULL SSID rejected");

	CHECK(wifi_cred_password_valid(""), "empty password = open network accepted");
	CHECK(wifi_cred_password_valid(pass_8), "8 byte password accepted");
	CHECK(wifi_cred_password_valid(pass_64), "64 byte password accepted");
	CHECK(!wifi_cred_password_valid("1234567"), "7 byte password rejected");
	CHECK(!wifi_cred_password_valid(pass_65), "65 byte password rejected");
	CHECK(!wifi_cred_password_valid(NULL), "NULL password rejected");

	CHECK(wifi_cred_ap_password_valid(pass_8), "AP password 8 bytes accepted");
	CHECK(wifi_cred_ap_password_valid("123456789012345678901234567890123456789012345678901234567890123"),
	      "AP password 63 bytes accepted");
	CHECK(!wifi_cred_ap_password_valid("1234567"), "AP password 7 bytes rejected");
	CHECK(!wifi_cred_ap_password_valid(pass_64), "AP password 64 bytes rejected");
}

static void test_copy(void)
{
	char ssid[33];
	char password[65];
	char too_small[4] = "xyz";

	group("bounded copies (defect fix: no fixed-size memcpy over-read)");
	memset(ssid, 0xAA, sizeof(ssid));
	CHECK(wifi_cred_ssid_copy(ssid, "WiFi Dap-Lab") == 0, "ssid_copy accepts valid SSID");
	CHECK(strcmp(ssid, "WiFi Dap-Lab") == 0, "ssid_copy result matches");
	memset(password, 0xAA, sizeof(password));
	CHECK(wifi_cred_password_copy(password, "12345678") == 0, "password_copy accepts valid");
	CHECK(strcmp(password, "12345678") == 0, "password_copy result matches");
	CHECK(wifi_cred_ssid_copy(ssid, "") == -1 && ssid[0] == '\0',
	      "ssid_copy rejects empty and clears destination");
	CHECK(wifi_cred_password_copy(password, "1234567") == -1 && password[0] == '\0',
	      "password_copy rejects 7 bytes and clears destination");
	(void)too_small;
}

static void test_find(void)
{
	birdiebot_wifi_cred_t table[3];
	int idx;

	group("SSID lookup (exact bytes, case sensitive, contract §4.1)");
	table[0] = make_cred("net-a", "password1", 100, 10);
	table[1] = make_cred("net-b", "password2", 200, 100);
	table[2] = make_cred("net-c", "password3", 0, 100);
	idx = wifi_cred_find_ssid(table, 3, "net-b");
	CHECK(idx == 1, "find exact SSID returns its index");
	CHECK(wifi_cred_find_ssid(table, 3, "NET-B") == -1, "lookup is case sensitive");
	CHECK(wifi_cred_find_ssid(table, 3, "net") == -1, "lookup does not do prefix match");
	CHECK(wifi_cred_find_ssid(table, 2, "net-c") == -1, "honours the count argument");
	CHECK(wifi_cred_find_ssid(NULL, 3, "net-a") == -1, "NULL table handled");
	CHECK(wifi_cred_find_ssid(table, 3, NULL) == -1, "NULL ssid handled");
}

static void test_codec(void)
{
	birdiebot_wifi_cred_t in;
	birdiebot_wifi_cred_t out;
	uint8_t blob[103];

	group("NVS blob codec (contract §2.2/§2.4)");
	in = make_cred("WiFi Dap-Lab", "password123", 1750000000u, 7);
	memset(blob, 0, sizeof(blob));
	CHECK(wifi_cred_record_pack(&in, blob, sizeof(blob)) == 0, "pack valid record");
	CHECK(wifi_cred_record_unpack(blob, sizeof(blob), &out) == 0, "unpack valid record");
	CHECK(strcmp(out.ssid, in.ssid) == 0 && strcmp(out.password, in.password) == 0 &&
	              out.last_success_ts == in.last_success_ts &&
	              out.priority == in.priority,
	      "pack/unpack roundtrip preserves all fields");
	CHECK(wifi_cred_record_pack(&in, blob, sizeof(blob) - 1) == -1,
	      "pack rejects a wrong buffer size");
	CHECK(wifi_cred_record_unpack(blob, 100, &out) == -1,
	      "unpack rejects a blob shorter than 103 bytes");
	CHECK(wifi_cred_record_unpack(blob, 104, &out) == -1,
	      "unpack rejects a blob longer than 103 bytes");

	memset(blob, 0, sizeof(blob));
	CHECK(wifi_cred_record_unpack(blob, sizeof(blob), &out) == -1,
	      "unpack rejects an empty SSID record");
	memset(blob, 0, sizeof(blob));
	memcpy(blob + 33, "1234567", 8); /* 7 byte password */
	blob[0] = 'a';
	blob[1] = '\0';
	CHECK(wifi_cred_record_unpack(blob, sizeof(blob), &out) == -1,
	      "unpack rejects a 1..7 byte password");

	/* §2.4.1: out-of-range characters are truncated by forcing the NUL at the
	 * last position of each field instead of trusting foreign/stale blobs. */
	memset(blob, 'A', sizeof(blob));
	blob[32] = '\0';      /* ssid[32] */
	blob[33 + 64] = '\0'; /* password[64] */
	CHECK(wifi_cred_record_unpack(blob, sizeof(blob), &out) == 0,
	      "unpack accepts max-length SSID/password and repairs NULs");
	CHECK(strlen(out.ssid) == 32 && strlen(out.password) == 64,
	      "max-length fields survive the repair");
}

static void test_timestamp(void)
{
	group("last_success_ts normalization (contract §4: no SNTP => 0)");
	CHECK(wifi_cred_normalize_ts(0) == 0, "0 stays 0");
	CHECK(wifi_cred_normalize_ts(BIRDIEBOT_WIFI_CRED_TS_VALID_MIN - 1) == 0,
	      "unsynced time (< 2023) is stored as 0");
	CHECK(wifi_cred_normalize_ts(BIRDIEBOT_WIFI_CRED_TS_VALID_MIN) ==
	              BIRDIEBOT_WIFI_CRED_TS_VALID_MIN,
	      "threshold time is trusted");
	CHECK(wifi_cred_normalize_ts(1750000000u) == 1750000000u, "typical unix time kept");
	CHECK(wifi_cred_normalize_ts(UINT32_MAX + 1ull) == UINT32_MAX, "value clamped to u32");
}

static void test_default_ap_ssid(void)
{
	uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x12, 0xAB, 0xCD};
	uint8_t zero_mac[6] = {0};
	char ssid[33];

	group("default AP SSID (contract §6)");
	CHECK(wifi_cred_make_default_ap_ssid(mac, ssid) == 0, "default SSID generation succeeds");
	CHECK(strcmp(ssid, "WiFi-Dap-ABCD") == 0,
	      "STA MAC 24:0A:C4:12:AB:CD -> WiFi-Dap-ABCD");
	CHECK(strlen(ssid) == 18, "default AP SSID is 18 bytes");
	CHECK(wifi_cred_make_default_ap_ssid(zero_mac, ssid) == 0,
	      "default SSID works for a zero MAC (defensive)");
	CHECK(strcmp(ssid, "WiFi-Dap-0000") == 0, "zero MAC -> WiFi-Dap-0000");
	CHECK(wifi_cred_make_default_ap_ssid(NULL, ssid) == -1, "NULL MAC handled");
	CHECK(wifi_cred_make_default_ap_ssid(mac, NULL) == -1, "NULL output handled");
}

static void test_eviction(void)
{
	birdiebot_wifi_cred_t table[4];

	group("eviction policy (contract §4.4)");
	table[0] = make_cred("net-a", "password1", 100, 10);
	table[1] = make_cred("net-b", "password2", 200, 100);
	table[2] = make_cred("net-c", "password3", 0, 100);
	table[3] = make_cred("net-d", "password4", 300, 255);

	CHECK(wifi_cred_evict_index(table, 4, BIRDIEBOT_WIFI_CRED_NONE) == 3,
	      "highest priority number (255) is evicted first");
	CHECK(wifi_cred_evict_index(table, 3, BIRDIEBOT_WIFI_CRED_NONE) == 2,
	      "same priority: oldest last_success (0) evicted");
	CHECK(wifi_cred_evict_index(table, 4, 3) == 2,
	      "currently connecting entry (idx 3) is protected");
	CHECK(wifi_cred_evict_index(table, 1, 0) == -1,
	      "only the protected entry left: nothing to evict");
	CHECK(wifi_cred_evict_index(table, 0, BIRDIEBOT_WIFI_CRED_NONE) == -1,
	      "empty table: nothing to evict");
	CHECK(wifi_cred_evict_index(NULL, 4, 0) == -1, "NULL table handled");

	/* tie on priority AND last_success -> lowest index */
	{
		birdiebot_wifi_cred_t tie[2];
		tie[0] = make_cred("t0", "password1", 0, 50);
		tie[1] = make_cred("t1", "password2", 0, 50);
		CHECK(wifi_cred_evict_index(tie, 2, BIRDIEBOT_WIFI_CRED_NONE) == 0,
		      "full tie on priority/ts falls back to lowest idx");
	}
}

static void test_rotation(void)
{
	uint8_t order[BIRDIEBOT_WIFI_CRED_MAX];
	birdiebot_wifi_cred_t table[4];
	int count;

	group("rotation order (contract §5.1)");
	table[0] = make_cred("a", "password1", 0, 100);
	table[1] = make_cred("b", "password2", 500, 10);
	table[2] = make_cred("c", "password3", 600, 10);
	table[3] = make_cred("d", "password4", 0, 100);

	count = wifi_cred_rotate_order(table, 4, 3, NULL, order, 4);
	CHECK(count == 4, "all four entries returned");
	CHECK(count == 4 && order[0] == 3, "last successful credential is first (pinned)");
	CHECK(count == 4 && order[1] == 2, "priority asc: 10 before 100 (newer ts first)");
	CHECK(count == 4 && order[2] == 1, "same priority: newer last_success first");
	CHECK(count == 4 && order[3] == 0, "ts=0 tie falls back to table index order");

	count = wifi_cred_rotate_order(table, 4, BIRDIEBOT_WIFI_CRED_NONE, NULL, order, 4);
	CHECK(count == 4 && order[0] == 2 && order[1] == 1 && order[2] == 0 && order[3] == 3,
	      "no last_idx: priority/ts/idx ordering for all entries");

	count = wifi_cred_rotate_order(table, 4, 9, NULL, order, 4);
	CHECK(count == 4 && order[0] == 2 && order[1] == 1,
	      "out-of-range last_idx falls back to normal ordering");

	{
		uint8_t skip[BIRDIEBOT_WIFI_CRED_MAX] = {0, 1, 0, 0};
		count = wifi_cred_rotate_order(table, 4, 0, skip, order, 4);
		CHECK(count == 3 && order[0] == 0 && order[1] == 2 && order[2] == 3,
		      "skip mask removes failed credentials (last success pinned)");
	}
	{
		uint8_t skip_all[BIRDIEBOT_WIFI_CRED_MAX] = {1, 1, 1, 1};
		CHECK(wifi_cred_rotate_order(table, 4, 0, skip_all, order, 4) == 0,
		      "all candidates skipped -> empty rotation plan");
	}

	count = wifi_cred_rotate_order(table, 4, 1, NULL, order, 2);
	CHECK(count == 2 && order[0] == 1, "max output cap honoured (last success kept)");
	CHECK(wifi_cred_rotate_order(table, 0, BIRDIEBOT_WIFI_CRED_NONE, NULL, order, 4) == 0,
	      "empty table -> empty rotation plan");
	CHECK(wifi_cred_rotate_order(NULL, 4, 0, NULL, order, 4) == 0, "NULL table handled");
	CHECK(wifi_cred_rotate_order(table, 4, 0, NULL, NULL, 4) == 0, "NULL order handled");
}

int main(void)
{
	printf("TAP version 13\n");
	printf("# scripts/test_wifi_cred_logic.sh -- host tests for wifi_cred_logic.c\n");

	test_layout();
	test_validation();
	test_copy();
	test_find();
	test_codec();
	test_timestamp();
	test_default_ap_ssid();
	test_eviction();
	test_rotation();

	printf("1..%d\n", g_test_no);
	printf("# %d/%d checks passed", g_test_no - g_failed, g_test_no);
	if (g_failed != 0) {
		printf(" (%d FAILED)\n", g_failed);
		return 1;
	}
	printf("\n");
	return 0;
}
