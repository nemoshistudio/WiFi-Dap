/**
 * @file ncm_netif.h
 * @brief Private interface of the USB-NCM wired-DAP transport (C1 MVP).
 *
 * Only compiled when CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM=y on ESP32-S3.
 * Implementation: ncm_netif.c (TinyUSB CDC-NCM + minimal esp_netif/lwIP driver).
 */
#ifndef BIRDIEBOT_WIRED_NCM_NETIF_H
#define BIRDIEBOT_WIRED_NCM_NETIF_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install TinyUSB NCM, create the static-IP esp_netif and bring it up. */
esp_err_t birdiebot_wired_ncm_init(void);

/** Destroy the USB-NCM netif / driver (best effort; used by deinit only). */
void birdiebot_wired_ncm_deinit(void);

/** Read-only diagnostics for the Web target/status endpoint. */
bool birdiebot_wired_ncm_is_active(void);
bool birdiebot_wired_ncm_dhcp_is_active(void);
const char *birdiebot_wired_ncm_device_ip(void);
int birdiebot_wired_ncm_dhcp_error(void);
int birdiebot_wired_ncm_dhcp_state(void);

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_WIRED_NCM_NETIF_H */
