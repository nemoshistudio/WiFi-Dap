/**
 * @file wired_dap.c
 * @brief C1 wired-DAP entry point: Kconfig dispatch and USB-NCM bring-up.
 *
 * Default build (CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NONE): this file only provides
 * the stub API; it installs nothing and app_main() never calls it.
 *
 * Wired build (CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM=y on ESP32-S3): dispatches
 * to the USB-NCM implementation in ncm_netif.c (TinyUSB CDC-NCM + esp_netif).
 */
#include "sdkconfig.h"

#include <stddef.h>

#include "esp_log.h"

#include "birdiebot_wired_dap.h"

#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM)
#include "ncm_netif.h"
#endif

static const char *TAG = "wired_dap";

#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM)
#define BIRDIEBOT_WIRED_COMPILED_TRANSPORT BIRDIEBOT_WIRED_TRANSPORT_NCM
#elif defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_HID_V1)
#define BIRDIEBOT_WIRED_COMPILED_TRANSPORT BIRDIEBOT_WIRED_TRANSPORT_HID_V1
#elif defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_BULK_V2)
#define BIRDIEBOT_WIRED_COMPILED_TRANSPORT BIRDIEBOT_WIRED_TRANSPORT_BULK_V2
#else
#define BIRDIEBOT_WIRED_COMPILED_TRANSPORT BIRDIEBOT_WIRED_TRANSPORT_NONE
#endif

birdiebot_wired_transport_t birdiebot_wired_dap_compiled_transport(void)
{
    return BIRDIEBOT_WIRED_COMPILED_TRANSPORT;
}

const char *birdiebot_wired_dap_transport_name(void)
{
    switch (BIRDIEBOT_WIRED_COMPILED_TRANSPORT) {
    case BIRDIEBOT_WIRED_TRANSPORT_NCM:
        return "ncm";
    case BIRDIEBOT_WIRED_TRANSPORT_HID_V1:
        return "hid_v1";
    case BIRDIEBOT_WIRED_TRANSPORT_BULK_V2:
        return "bulk_v2";
    case BIRDIEBOT_WIRED_TRANSPORT_NONE:
    default:
        return "none";
    }
}

bool birdiebot_wired_dap_is_supported(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3) && (BIRDIEBOT_WIRED_COMPILED_TRANSPORT != BIRDIEBOT_WIRED_TRANSPORT_NONE)
    return true;
#else
    return false;
#endif
}

const char *birdiebot_wired_dap_device_ip(void)
{
    return "192.168.7.1";
}

esp_err_t birdiebot_wired_dap_init(void)
{
#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)
    esp_err_t err = birdiebot_wired_ncm_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-NCM init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB-NCM wired DAP ready: device=%s/24, DAP proxy on TCP 3240, host 192.168.7.2/24",
             birdiebot_wired_dap_device_ip());
    return ESP_OK;
#else
    ESP_LOGW(TAG, "wired transport not compiled in (CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NONE)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void birdiebot_wired_dap_deinit(void)
{
#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)
    birdiebot_wired_ncm_deinit();
#endif
}
