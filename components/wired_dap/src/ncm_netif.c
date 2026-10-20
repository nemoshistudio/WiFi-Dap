/**
 * @file ncm_netif.c
 * @brief USB-NCM wired DAP transport (C1 MVP).
 *
 * Data path:
 *   host USB-NCM Ethernet <-> TinyUSB CDC-NCM <-> esp_netif/lwIP (static
 *   192.168.7.1/24) <-> existing TCP 3240 DAP proxy (components/dap_proxy)
 *   -> USBIP/DAP -> SWD/JTAG.
 *
 * Design follows the IDF 5.2.1 reference implementation
 * examples/network/sta2eth/main/usb_ncm_iface.c (custom esp_netif driver over
 * TinyUSB NCM + lwIP ethernetif glue). Differences for this MVP:
 *   - device-local interface with a static IPv4 address (192.168.7.1/24);
 *   - no WiFi<->USB bridge (the DAP TCP server listens on INADDR_ANY, so it is
 *     reachable through both the NCM netif and the Wi-Fi netifs);
 *   - no DHCP server yet (the host configures 192.168.7.2/24 manually) to keep
 *     the MVP small and to avoid the host installing a default route through
 *     the probe. DHCP is a P2 item (docs/wired-dap-mvp.md).
 *
 * Console conflict: USB-OTG and USB-Serial/JTAG cannot be active at the same
 * time. The wired build must disable CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
 * (sdkconfig.wired.defaults); the console stays on UART0.
 */
#include "sdkconfig.h"

#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_net.h"

/* lwIP <-> esp_netif glue: ethernetif_init / ethernetif_input from the
 * IDF esp_netif lwIP port (same header used by the IDF usb_ncm_iface example). */
#include "lwip/esp_netif_net_stack.h"

#include "ncm_netif.h"

static const char *TAG = "wired_ncm";

#define NCM_IF_KEY         "usb_ncm"
#define NCM_IF_DESC        "USB NCM wired DAP"
#define NCM_DAP_PROXY_PORT 3240
#define NCM_TX_TIMEOUT_TICKS pdMS_TO_TICKS(100)

/* Static device address for the USB-NCM management link: 192.168.7.1/24.
 * No default gateway is advertised; the host uses it only for the DAP proxy. */
#define NCM_IPV4_ADDR   ESP_IP4TOADDR(192, 168, 7, 1)
#define NCM_IPV4_MASK   ESP_IP4TOADDR(255, 255, 255, 0)

static const esp_netif_ip_info_t s_ncm_ip_info = {
    .ip = { .addr = NCM_IPV4_ADDR },
    .netmask = { .addr = NCM_IPV4_MASK },
    .gw = { .addr = 0 },
};

static esp_netif_t *s_ncm_netif = NULL;
static uint8_t s_ncm_mac[6];

/* ------------------------------------------------------------------------- */
/* esp_netif driver callbacks (called from lwIP/TinyUSB contexts)             */
/* ------------------------------------------------------------------------- */

static esp_err_t ncm_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (buffer == NULL || len == 0 || len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, NCM_TX_TIMEOUT_TICKS);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "USB-NCM TX failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void ncm_netif_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* TinyUSB NCM receive callback (TinyUSB task context). */
static esp_err_t ncm_netif_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_ncm_netif == NULL || buffer == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* esp_netif_receive takes ownership of the buffer; copy it so TinyUSB can
     * recycle its NTB buffer immediately (same approach as the IDF reference). */
    void *copy = malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    esp_err_t err = esp_netif_receive(s_ncm_netif, copy, len, NULL);
    if (err != ESP_OK) {
        free(copy);
        return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* USB-NCM bring-up                                                           */
/* ------------------------------------------------------------------------- */

static esp_err_t ncm_usb_init(void)
{
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .self_powered = false,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_read_mac(s_ncm_mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed (%s); using fixed locally-administered MAC",
                 esp_err_to_name(err));
        const uint8_t fallback[6] = { 0x02, 0x42, 0x49, 0x52, 0x44, 0x01 };
        memcpy(s_ncm_mac, fallback, sizeof(s_ncm_mac));
    }

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = ncm_netif_recv_cb,
        .free_tx_buffer = NULL,
        .on_init_callback = NULL,
        .user_context = NULL,
    };
    memcpy(net_cfg.mac_addr, s_ncm_mac, sizeof(net_cfg.mac_addr));

    err = tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TinyUSB CDC-NCM device installed (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
             s_ncm_mac[0], s_ncm_mac[1], s_ncm_mac[2], s_ncm_mac[3], s_ncm_mac[4], s_ncm_mac[5]);
    return ESP_OK;
}

static esp_err_t ncm_esp_netif_init(void)
{
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &s_ncm_ip_info,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = NCM_IF_KEY,
        .if_desc = NCM_IF_DESC,
        .route_prio = 10,
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        /* Static singleton (TinyUSB NCM is a single device); must be non-NULL. */
        .handle = (void *)1,
        .transmit = ncm_netif_transmit,
        .transmit_wrap = NULL,
        .driver_free_rx_buffer = ncm_netif_free_rx,
    };

    /* USB-NCM presents an Ethernet L2 interface to lwIP; reuse the IDF
     * ethernetif glue (IDF 5.2.1 esp_netif lwIP port). */
    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config,
    };

    s_ncm_netif = esp_netif_new(&cfg);
    if (s_ncm_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_set_mac(s_ncm_netif, s_ncm_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_mac failed: %s", esp_err_to_name(err));
    }

    /* Driver is already started (USB-NCM class initialised); bring the netif up
     * with its static IP. No DHCP client/server in the MVP. */
    esp_netif_action_start(s_ncm_netif, NULL, 0, NULL);

    esp_netif_ip_info_t ip_info = { 0 };
    err = esp_netif_get_ip_info(s_ncm_netif, &ip_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NCM netif up: ip=" IPSTR " netmask=" IPSTR,
                 IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask));
    }
    return ESP_OK;
}

esp_err_t birdiebot_wired_ncm_init(void)
{
    if (s_ncm_netif != NULL) {
        ESP_LOGW(TAG, "already initialised");
        return ESP_OK;
    }

    /* Create the netif first so the USB RX callback always has a valid target,
     * then install TinyUSB/NCM (host enumeration starts after this). */
    esp_err_t err = ncm_esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = ncm_usb_init();
    if (err != ESP_OK) {
        esp_netif_destroy(s_ncm_netif);
        s_ncm_netif = NULL;
        return err;
    }

    ESP_LOGI(TAG, "wired DAP ready: host network 192.168.7.0/24 (host .2), "
                  "device 192.168.7.1, DAP proxy TCP %d",
             NCM_DAP_PROXY_PORT);
    return ESP_OK;
}

void birdiebot_wired_ncm_deinit(void)
{
    if (s_ncm_netif != NULL) {
        esp_netif_destroy(s_ncm_netif);
        s_ncm_netif = NULL;
    }
}

#endif /* CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM && CONFIG_IDF_TARGET_ESP32S3 */
