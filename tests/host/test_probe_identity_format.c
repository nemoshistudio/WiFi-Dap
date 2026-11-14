/*
 * Host unit tests for the ESP-IDF-free probe serial formatting.
 *
 * Target under test: components/probe_identity/birdiebot_probe_identity_format.c
 * Contract:          "WiFi-Dap-" + 12 uppercase hex (WiFi STA MAC), see
 *                    components/probe_identity/include/birdiebot_probe_identity.h
 *
 * Output is TAP-ish ("TAP version 13", one line per check, plan at the end) so
 * scripts/test_host.sh can aggregate results.  It only uses the C standard
 * library and never touches ESP-IDF.
 *
 * Build/run:
 *   cc -std=c11 -Wall -Wextra -Werror -Icomponents/probe_identity/include \
 *      tests/host/test_probe_identity_format.c \
 *      components/probe_identity/birdiebot_probe_identity_format.c -o /tmp/t && /tmp/t
 */

#include "birdiebot_probe_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_total;
static int g_fail;

static void tap_result(bool pass, const char *name, const char *detail)
{
	++g_total;
	if (pass) {
		printf("ok %d - %s\n", g_total, name);
		return;
	}
	++g_fail;
	printf("not ok %d - %s\n", g_total, name);
	if (detail != NULL) {
		printf("#   %s\n", detail);
	}
}

#define CHECK_TRUE(cond, name)                                                    \
	do {                                                                      \
		bool _ok = (cond) ? true : false;                                 \
		tap_result(_ok, name, _ok ? NULL : "expected true, got false");   \
	} while (0)

#define CHECK_STR_EQ(actual, expected, name)                                      \
	do {                                                                      \
		const char *_a = (actual);                                        \
		const char *_e = (expected);                                      \
		bool _ok = (_a != NULL) && (strcmp(_a, _e) == 0);                 \
		char _detail[160];                                                \
		snprintf(_detail, sizeof(_detail), "actual='%s' expected='%s'",   \
		         (_a != NULL) ? _a : "(null)", _e);                       \
		tap_result(_ok, name, _ok ? NULL : _detail);                      \
	} while (0)

static void test_known_mac(void)
{
	const uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
	char serial[BIRDIEBOT_PROBE_SERIAL_CAPACITY] = {0};

	const size_t len = birdiebot_probe_format_serial(mac, serial, sizeof(serial));

	CHECK_TRUE(len == BIRDIEBOT_PROBE_SERIAL_LEN,
	           "length matches BIRDIEBOT_PROBE_SERIAL_LEN");
	CHECK_STR_EQ(serial, "WiFi-Dap-001122334455", "known MAC formatting");
	CHECK_TRUE(strlen(serial) == len, "return value equals strlen");
}

static void test_uppercase_hex(void)
{
	const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x0F};
	char serial[BIRDIEBOT_PROBE_SERIAL_CAPACITY] = {0};

	(void)birdiebot_probe_format_serial(mac, serial, sizeof(serial));

	CHECK_STR_EQ(serial, "WiFi-Dap-AABBCCDDEE0F", "hex digits are uppercase");
}

static void test_all_zero_mac(void)
{
	const uint8_t mac[6] = {0};
	char serial[BIRDIEBOT_PROBE_SERIAL_CAPACITY] = {0};

	(void)birdiebot_probe_format_serial(mac, serial, sizeof(serial));

	CHECK_STR_EQ(serial, "WiFi-Dap-000000000000", "zero MAC fallback format");
}

static void test_capacity_edges(void)
{
	const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
	char exact[BIRDIEBOT_PROBE_SERIAL_CAPACITY] = {0};
	char too_small[BIRDIEBOT_PROBE_SERIAL_CAPACITY] = {0};
	const size_t ok_len =
	    birdiebot_probe_format_serial(mac, exact, sizeof(exact));
	const size_t fail_len =
	    birdiebot_probe_format_serial(mac, too_small, sizeof(too_small) - 1U);

	CHECK_TRUE(ok_len == BIRDIEBOT_PROBE_SERIAL_LEN,
	           "exact capacity is accepted");
	CHECK_TRUE(fail_len == 0U, "one-byte-short capacity is rejected");
	CHECK_TRUE(exact[BIRDIEBOT_PROBE_SERIAL_LEN] == '\0',
	           "exact-capacity result is NUL terminated");
}

static void test_null_arguments(void)
{
	const uint8_t mac[6] = {0};
	char serial[BIRDIEBOT_PROBE_SERIAL_CAPACITY] = {0};

	CHECK_TRUE(birdiebot_probe_format_serial(NULL, serial, sizeof(serial)) == 0U,
	           "NULL mac is rejected");
	CHECK_TRUE(birdiebot_probe_format_serial(mac, NULL, sizeof(serial)) == 0U,
	           "NULL output is rejected");
	CHECK_TRUE(birdiebot_probe_format_serial(mac, serial, 0U) == 0U,
	           "zero capacity is rejected");
}

int main(void)
{
	printf("TAP version 13\n");
	printf("# probe identity serial formatting\n");

	test_known_mac();
	test_uppercase_hex();
	test_all_zero_mac();
	test_capacity_edges();
	test_null_arguments();

	printf("1..%d\n", g_total);
	if (g_fail == 0) {
		printf("# summary: checks=%d failed=0 status=PASS\n", g_total);
		return 0;
	}
	printf("# summary: checks=%d failed=%d status=FAIL\n", g_total, g_fail);
	return 1;
}
