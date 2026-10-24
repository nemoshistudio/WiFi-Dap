/**
 * @file birdiebot_wired_dap.h
 * @brief C1 wired-DAP API: USB-NCM (default ON for unified ESP32-S3 images).
 *
 * STATUS: default ESP32-S3 unified firmware enables
 *         CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM=y together with Wi-Fi; other
 *         targets keep NONE.
 *
 * Design and effort/risk analysis: docs/wired-dap-plan.md.
 * Build / flash / host test steps: docs/wired-dap-mvp.md.
 *
 * Wired variant data path (all existing DAP/USBIP code reused):
 *
 *   Host usbip attach_ude -r 192.168.7.1 -b 1-1
 *     -> USB-C (ESP32-S3 native USB-OTG, Full-Speed 12 Mbps)
 *     -> TinyUSB CDC-NCM device (host: USB Ethernet adapter)
 *     -> device esp_netif/lwIP netif (static 192.168.7.1/24)
 *     -> existing TCP 3240 DAP proxy -> components/DAP -> SWD/JTAG
 *
 * Console/conflict rules for the wired build:
 *  - USB-OTG and USB-Serial/JTAG cannot be active simultaneously, therefore the
 *    wired build disables CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG and keeps
 *    the console on UART0. Flashing/recovery: BOOT+RESET ROM download mode,
 *    UART0 download, or Web UI OTA (Wi-Fi is kept enabled).
 *
 * Lifecycle contract:
 *  - birdiebot_wired_dap_init() is called once from app_main() in the wired
 *    build only; it installs the TinyUSB driver, creates the NCM netif and
 *    brings it up (non-blocking; USB enumeration happens asynchronously).
 *  - It must not be called in the default (NONE) build; app_main() guards the
 *    call with the CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM compile-time flag.
 */
#ifndef BIRDIEBOT_WIRED_DAP_H
#define BIRDIEBOT_WIRED_DAP_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BIRDIEBOT_WIRED_TRANSPORT_NONE = 0,    /**< Wi-Fi only (default/production). */
    BIRDIEBOT_WIRED_TRANSPORT_NCM = 1,     /**< USB CDC-NCM Ethernet + USBIP (wired MVP). */
    BIRDIEBOT_WIRED_TRANSPORT_HID_V1 = 2,  /**< Reserved: CMSIS-DAP v1 over USB HID (not implemented). */
    BIRDIEBOT_WIRED_TRANSPORT_BULK_V2 = 3, /**< Reserved: CMSIS-DAP v2 over USB bulk/WinUSB (not implemented). */
} birdiebot_wired_transport_t;

/** @brief Transport selected by Kconfig at compile time. */
birdiebot_wired_transport_t birdiebot_wired_dap_compiled_transport(void);

/** @brief Short stable name of the compiled transport (for logs). */
const char *birdiebot_wired_dap_transport_name(void);

/**
 * @brief True when the compiled transport is supported by this target.
 *
 * True only for ESP32-S3 builds with CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM.
 */
bool birdiebot_wired_dap_is_supported(void);

/**
 * @brief Start the wired transport (USB-NCM MVP).
 *
 * Installs the TinyUSB driver, initialises the NCM class and creates/attaches
 * the esp_netif/lwIP netif with the static device address 192.168.7.1/24.
 * Returns ESP_ERR_NOT_SUPPORTED when the build has no wired transport.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t birdiebot_wired_dap_init(void);

/** @brief Stop the wired transport (idempotent; MVP destroys the NCM netif). */
void birdiebot_wired_dap_deinit(void);

/** @brief Device IPv4 address used by the USB-NCM netif (for logs/tests). */
const char *birdiebot_wired_dap_device_ip(void);

/** @brief True when the USB-NCM transport is initialized (read-only). */
bool birdiebot_wired_dap_ncm_active(void);

/** @brief True when the NCM DHCP server started successfully (read-only). */
bool birdiebot_wired_dap_dhcp_active(void);

/** Last esp_netif_dhcps_start() error code and DHCP status enum (diagnostics). */
int birdiebot_wired_dap_dhcp_error(void);
int birdiebot_wired_dap_dhcp_state(void);

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_WIRED_DAP_H */
