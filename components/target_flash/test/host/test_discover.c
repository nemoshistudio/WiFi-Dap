/*
 * Host regression test for the P1-19 /api/discover JSON formatter
 * (components/target_flash/src/tf_discover_core.c), no ESP32/mDNS needed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wt_target_flash.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

static void test_empty(void)
{
    char buf[512];
    int n = wt_tf_discover_format_json(NULL, 0U, buf, sizeof(buf));

    CHECK(n > 0);
    CHECK(strcmp(buf, "{\"ok\":true,\"devices\":[]}") == 0);
    CHECK(n == (int)strlen(buf));

    /* A non-NULL array with count 0 is the same as no result. */
    wt_tf_discover_device_t dev;
    memset(&dev, 0, sizeof(dev));
    n = wt_tf_discover_format_json(&dev, 0U, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"devices\":[]") != NULL);
}

static void test_one_device(void)
{
    char buf[1024];
    wt_tf_discover_device_t dev;
    int n;

    memset(&dev, 0, sizeof(dev));
    (void)snprintf(dev.name, sizeof(dev.name), "WiFi Dap");
    (void)snprintf(dev.hostname, sizeof(dev.hostname), "wifi-dap-1a2b.local");
    (void)snprintf(dev.ip, sizeof(dev.ip), "<device-ip>");
    dev.port = 80;

    n = wt_tf_discover_format_json(&dev, 1U, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(n == (int)strlen(buf));
    CHECK(strstr(buf, "\"ok\":true") != NULL);
    CHECK(strstr(buf, "\"name\":\"WiFi Dap\"") != NULL);
    CHECK(strstr(buf, "\"hostname\":\"wifi-dap-1a2b.local\"") != NULL);
    CHECK(strstr(buf, "\"ip\":\"<device-ip>\"") != NULL);
    CHECK(strstr(buf, "\"port\":80") != NULL);
    /* mDNS has no RSSI -> must be omitted, never fabricated as 0/null. */
    CHECK(strstr(buf, "rssi") == NULL);
    CHECK(strstr(buf, "\"target\"") == NULL);
    CHECK(strstr(buf, "\"version\"") == NULL);

    /* Measured RSSI and optional TXT enrichment are included when present. */
    dev.has_rssi = true;
    dev.rssi = -52;
    (void)snprintf(dev.target, sizeof(dev.target), "STM32H7");
    (void)snprintf(dev.version, sizeof(dev.version), "v1.2.0");
    n = wt_tf_discover_format_json(&dev, 1U, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"rssi\":-52") != NULL);
    CHECK(strstr(buf, "\"target\":\"STM32H7\"") != NULL);
    CHECK(strstr(buf, "\"version\":\"v1.2.0\"") != NULL);
}

static void test_escaping_and_utf8(void)
{
    char buf[1024];
    wt_tf_discover_device_t dev;

    memset(&dev, 0, sizeof(dev));
    /* Quotes/backslash/newline/control char must be escaped; a valid UTF-8
     * mDNS TXT name (the project advertises Chinese text) passes through. */
    (void)snprintf(dev.name, sizeof(dev.name), "a\"b\\c\nd\x01");
    (void)snprintf(dev.hostname, sizeof(dev.hostname), "dev.local");
    (void)snprintf(dev.ip, sizeof(dev.ip), "10.0.0.2");
    dev.port = 80;

    int n = wt_tf_discover_format_json(&dev, 1U, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "a\\\"b\\\\c\\nd\\u0001") != NULL);
    CHECK(strstr(buf, "\"ip\":\"10.0.0.2\"") != NULL);

    memset(&dev, 0, sizeof(dev));
    (void)snprintf(dev.name, sizeof(dev.name), "\u5141\u65af\u8c03\u8bd5\u5668-1A-2B");
    (void)snprintf(dev.hostname, sizeof(dev.hostname), "d.local");
    (void)snprintf(dev.ip, sizeof(dev.ip), "10.0.0.3");
    dev.port = 8080;
    n = wt_tf_discover_format_json(&dev, 1U, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\u5141\u65af\u8c03\u8bd5\u5668-1A-2B") != NULL);
    CHECK(strstr(buf, "\"port\":8080") != NULL);
}

static void test_multiple_and_truncation(void)
{
    char buf[2048];
    char small[40];
    wt_tf_discover_device_t devs[2];

    memset(devs, 0, sizeof(devs));
    (void)snprintf(devs[0].name, sizeof(devs[0].name), "one");
    (void)snprintf(devs[0].hostname, sizeof(devs[0].hostname), "one.local");
    (void)snprintf(devs[0].ip, sizeof(devs[0].ip), "10.0.0.4");
    devs[0].port = 80;
    (void)snprintf(devs[1].name, sizeof(devs[1].name), "two");
    (void)snprintf(devs[1].hostname, sizeof(devs[1].hostname), "two.local");
    (void)snprintf(devs[1].ip, sizeof(devs[1].ip), "10.0.0.5");
    devs[1].port = 8080;

    int n = wt_tf_discover_format_json(devs, 2U, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"name\":\"one\"") != NULL);
    CHECK(strstr(buf, "\"name\":\"two\"") != NULL);
    CHECK(strstr(buf, "},{") != NULL); /* comma-separated rows */
    CHECK(buf[n - 1] == '}' && buf[n - 2] == ']');

    /* Too small buffer -> -1, never a truncated (invalid) JSON body. */
    CHECK(wt_tf_discover_format_json(devs, 2U, small, sizeof(small)) == -1);
    CHECK(wt_tf_discover_format_json(devs, 2U, NULL, 0U) == -1);
    CHECK(wt_tf_discover_format_json(devs, 1U, small, 0U) == -1);
    /* Exactly the empty response size must fit. */
    {
        char tiny[sizeof("{\"ok\":true,\"devices\":[]}")];
        int tn = wt_tf_discover_format_json(NULL, 0U, tiny, sizeof(tiny));

        CHECK(tn == (int)sizeof("{\"ok\":true,\"devices\":[]}") - 1);
        CHECK(strcmp(tiny, "{\"ok\":true,\"devices\":[]}") == 0);
    }
}

int main(void)
{
    test_empty();
    test_one_device();
    test_escaping_and_utf8();
    test_multiple_and_truncation();

    if (g_failures == 0) {
        printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    printf("FAILED: %d/%d checks\n", g_failures, g_checks);
    return 1;
}
