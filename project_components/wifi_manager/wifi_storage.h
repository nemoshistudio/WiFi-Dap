#ifndef WIFI_STORAGE_H_GUARD
#define WIFI_STORAGE_H_GUARD

#include <stdint-gcc.h>
#include "wifi_api.h"

/* Legacy (namespace "wt_wifi") single network credential layout. New code must
 * use wifi_credential.h (namespace "wifi_cred"); this type is only kept for the
 * one-time migration read in wifi_credential.c. */
typedef struct nvs_wifi_credential_t {
	char ssid[32];
	char password[64];
} wifi_credential_t;

/* Legacy reads (migration / upgrade compatibility only). */
int wifi_data_get_sta_last_conn_cred(wifi_credential_t *ap_credential);

int wifi_data_get_ap_credential(wifi_credential_t *ap_credential);

/* WiFi mode persistence (unchanged behaviour). */
int wifi_data_save_wifi_mode(wifi_apsta_mode_e mode);

int wifi_data_get_wifi_mode(wifi_apsta_mode_e *mode);

/* STA static IP / DNS persistence (unchanged behaviour). */
int wifi_data_get_static(wifi_api_sta_ap_static_info_t *static_info);

int wifi_data_save_static(wifi_api_sta_ap_static_info_t *static_info);


#endif //WIFI_STORAGE_H_GUARD
