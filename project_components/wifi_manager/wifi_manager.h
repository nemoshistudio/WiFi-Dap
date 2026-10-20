#ifndef WIFI_MANAGER_H_GUARD
#define WIFI_MANAGER_H_GUARD

#include <esp_wifi_types.h>
#include "wifi_api.h"
#include "wifi_storage.h"

void wifi_manager_init(void);


void *wifi_manager_get_ap_netif(void);
void *wifi_manager_get_sta_netif(void);

typedef void (*wifi_manager_scan_done_cb)(uint16_t ap_found, wifi_ap_record_t *record, void *arg);
int wifi_manager_get_scan_list(uint16_t *number, wifi_ap_record_t *aps);

/**
 * Connect to an AP (JSON WIFI cmd 2).
 *
 * @param save WIFI_API_CONNECT_SAVE_AUTO: follow the auto_save setting;
 *             WIFI_API_CONNECT_SAVE_NO: never persist; WIFI_API_CONNECT_SAVE_YES:
 *             always persist (docs/wifi-credential-memory.md §7.5).
 * @return 0 on success, non-zero on failure.
 */
int wifi_manager_connect(const char *ssid, const char *password, int save);
int wifi_manager_disconnect(void);
int wifi_manager_change_mode(wifi_apsta_mode_e mode);
int wifi_manager_get_mode(wifi_apsta_mode_e *mode, wifi_mode_t *status);
int wifi_manager_get_ap_auto_delay(int *ap_on_delay, int *ap_off_delay);
int wifi_manager_set_ap_auto_delay(int *ap_on_delay, int *ap_off_delay);
int wifi_manager_set_ap_credential(wifi_credential_t *cred);
int wifi_manager_sta_set_static_conf(wifi_api_sta_ap_static_info_t *static_info);

/** Apply the effective (custom or MAC based default) AP credential from NVS. */
int wifi_manager_refresh_ap_credential(void);


#endif //WIFI_MANAGER_H_GUARD
