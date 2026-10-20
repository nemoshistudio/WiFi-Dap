#include "wifi_api.h"
#include "wifi_manager.h"
#include "wifi_configuration.h"
#include "wifi_credential.h"
#include "wifi_storage.h"
#include <esp_wifi.h>

void wifi_api_sta_get_ap_info(wifi_api_ap_info_t *ap_info)
{
	wifi_ap_record_t ap_record = {0};
	int err;
	esp_wifi_sta_get_ap_info(&ap_record);
	strncpy(ap_info->ssid, (char *)ap_record.ssid, sizeof(ap_info->ssid) - 1);
	ap_info->ssid[sizeof(ap_info->ssid) - 1] = '\0';
	strncpy(ap_info->mac, (char *)ap_record.bssid, sizeof(ap_info->mac));
	ap_info->rssi = ap_record.rssi;
	ap_info->password[0] = '\0';

	esp_netif_t *sta_netif = wifi_manager_get_sta_netif();
	esp_netif_ip_info_t ip_info;
	esp_netif_get_ip_info(sta_netif, &ip_info);
	ip4_addr_set(&ap_info->ip, &ip_info.ip);
	ip4_addr_set(&ap_info->gateway, &ip_info.gw);
	ip4_addr_set(&ap_info->netmask, &ip_info.netmask);

	esp_netif_dns_info_t dns_info;
	err = esp_netif_get_dns_info(wifi_manager_get_sta_netif(),
	                             ESP_NETIF_DNS_MAIN, &dns_info);
	if (err) {
		ap_info->dns_main.addr = ap_info->gateway.addr;
	} else {
		ap_info->dns_main.addr = dns_info.ip.u_addr.ip4.addr;
	}

	err = esp_netif_get_dns_info(wifi_manager_get_sta_netif(),
	                             ESP_NETIF_DNS_BACKUP, &dns_info);
	if (err) {
		ap_info->dns_backup.addr = ap_info->gateway.addr;
	} else {
		ap_info->dns_backup.addr = dns_info.ip.u_addr.ip4.addr;
	}
}

void wifi_api_ap_get_info(wifi_api_ap_info_t *ap_info)
{
	char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1] = {0};
	char password[BIRDIEBOT_WIFI_PASS_MAX + 1] = {0};
	int err;

	/* Effective AP credential: user defined NVS value wins over the dynamic
	 * "WiFi-Dap-XXXX" default (docs/wifi-credential-memory.md §6). */
	err = wifi_cred_mem_get_ap_cred(ssid, password);
	if (err) {
		ap_info->ssid[0] = '\0';
		ap_info->password[0] = '\0';
	} else {
		strncpy(ap_info->ssid, ssid, sizeof(ap_info->ssid) - 1);
		ap_info->ssid[sizeof(ap_info->ssid) - 1] = '\0';
		strncpy(ap_info->password, password, sizeof(ap_info->password) - 1);
		ap_info->password[sizeof(ap_info->password) - 1] = '\0';
	}
	esp_wifi_get_mac(WIFI_IF_AP, (uint8_t *) &ap_info->mac);
	IP4_ADDR_EXPAND(&ap_info->ip, WIFI_DEFAULT_AP_IP);
	IP4_ADDR_EXPAND(&ap_info->gateway, WIFI_DEFAULT_AP_GATEWAY);
	IP4_ADDR_EXPAND(&ap_info->netmask, WIFI_DEFAULT_AP_NETMASK);
	ap_info->rssi = 0;

	esp_netif_dns_info_t dns_info;
	err = esp_netif_get_dns_info(wifi_manager_get_ap_netif(),
	                             ESP_NETIF_DNS_MAIN, &dns_info);
	if (err) {
		ap_info->dns_main.addr = ap_info->gateway.addr;
	} else {
		ap_info->dns_main.addr = dns_info.ip.u_addr.ip4.addr;
	}

	err = esp_netif_get_dns_info(wifi_manager_get_ap_netif(),
	                             ESP_NETIF_DNS_BACKUP, &dns_info);
	if (err) {
		ap_info->dns_backup.addr = ap_info->gateway.addr;
	} else {
		ap_info->dns_backup.addr = dns_info.ip.u_addr.ip4.addr;
	}
}

static int rssi_comp(const void *a, const void *b)
{
	const wifi_api_ap_scan_info_t *r1 = a;
	const wifi_api_ap_scan_info_t *r2 = b;
	return r2->rssi - r1->rssi;
}

/**
 * @brief blocking function
 */
int wifi_api_get_scan_list(uint16_t *number, wifi_api_ap_scan_info_t *ap_info)
{
	wifi_ap_record_t *records;
	int err;

	records = malloc(*number * sizeof(wifi_ap_record_t));
	if (records == NULL) {
		return ESP_ERR_NO_MEM;
	}

	err = wifi_manager_get_scan_list(number, records);
	if (err) {
		printf("get scan list err\n");
		free(records);
		return err;
	}

	for (int i = 0; i < *number; ++i) {
		printf("%d %d %s\n", i, records[i].rssi, records[i].ssid);
		strncpy(ap_info[i].ssid, (char *) records[i].ssid, sizeof(ap_info[i].ssid));
		strncpy(ap_info[i].mac, (char *) records[i].bssid, sizeof(ap_info[i].mac));
		ap_info[i].rssi = records[i].rssi;
	}
	free(records);

	qsort(ap_info, *number, sizeof(wifi_api_ap_scan_info_t), rssi_comp);
	return 0;
}

int wifi_api_connect(const char *ssid, const char *password)
{
	return wifi_manager_connect(ssid, password, WIFI_API_CONNECT_SAVE_AUTO);
}

int wifi_api_connect_save(const char *ssid, const char *password, int save)
{
	if (save != WIFI_API_CONNECT_SAVE_AUTO && save != WIFI_API_CONNECT_SAVE_NO &&
	    save != WIFI_API_CONNECT_SAVE_YES) {
		return 1;
	}
	return wifi_manager_connect(ssid, password, save);
}

int wifi_api_disconnect(void)
{
	return wifi_manager_disconnect();
}

int wifi_api_sta_set_static_conf(wifi_api_sta_ap_static_info_t *static_info)
{
	int err = wifi_manager_sta_set_static_conf(static_info);
	if (err) {
		return err;
	}

	return wifi_data_save_static(static_info);
}
