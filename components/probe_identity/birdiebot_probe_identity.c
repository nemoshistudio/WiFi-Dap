/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi Dap probe identity implementation (ESP-IDF part).
 *
 * The factory WiFi STA MAC is read once (esp_read_mac); the serial is
 * "WiFi-Dap-<12 uppercase hex>".  Formatting is delegated to the pure
 * helper in birdiebot_probe_identity_format.c.
 */

#include "birdiebot_probe_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "birdiebot_board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "probe_identity";

static bool s_initialized = false;
static char s_serial[BIRDIEBOT_PROBE_SERIAL_CAPACITY] =
    "WiFi-Dap-000000000000";
static char s_fw_version[32] = "unknown";

static const char *probe_chip_name(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  return "ESP32-S2";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  return "ESP32-C6";
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  return "ESP32-C5";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return "ESP32-C3";
#elif defined(CONFIG_IDF_TARGET_ESP32)
  return "ESP32";
#else
  return CONFIG_IDF_TARGET;
#endif
}

static const char *probe_board_name(void)
{
  /* components/board selects one board file per build; the header defines the
   * board id string (e.g. "wifi_dap_s3"). */
  return BIRDIEBOT_BOARD_NAME;
}

void birdiebot_probe_identity_init(void)
{
  if (s_initialized) {
    return;
  }

  uint8_t mac[6] = {0};
  const esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err == ESP_OK) {
    if (birdiebot_probe_format_serial(mac, s_serial, sizeof(s_serial)) == 0U) {
      ESP_LOGW(TAG, "serial formatting failed; keeping %s", s_serial);
    }
  } else {
    ESP_LOGW(TAG,
             "esp_read_mac(ESP_MAC_WIFI_STA) failed (%s); using fallback "
             "serial %s",
             esp_err_to_name(err), s_serial);
  }

  const esp_app_desc_t *app = esp_app_get_description();
  if ((app != NULL) && (app->version[0] != '\0')) {
    snprintf(s_fw_version, sizeof(s_fw_version), "%s", app->version);
  }

  s_initialized = true;
  ESP_LOGI(TAG, "serial=%s chip=%s board=%s fw=%s", s_serial,
           probe_chip_name(), probe_board_name(), s_fw_version);
}

const char *birdiebot_probe_serial(void)
{
  if (!s_initialized) {
    birdiebot_probe_identity_init();
  }
  return s_serial;
}

const char *birdiebot_probe_chip(void)
{
  if (!s_initialized) {
    birdiebot_probe_identity_init();
  }
  return probe_chip_name();
}

const char *birdiebot_probe_board(void)
{
  if (!s_initialized) {
    birdiebot_probe_identity_init();
  }
  return probe_board_name();
}

const char *birdiebot_probe_fw_version(void)
{
  if (!s_initialized) {
    birdiebot_probe_identity_init();
  }
  return s_fw_version;
}
