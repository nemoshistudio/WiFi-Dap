#include "wt_mdns_config.h"

#include <mdns.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <stdio.h>

#define MDNS_TAG "wt_mdns"

void wt_mdns_init()
{
	ESP_ERROR_CHECK(mdns_init());

	/* Hostname must be unique per device: wifi-dap-XXXX.local where XXXX
	 * is the last two bytes (four hex digits) of the STA MAC, same suffix as
	 * the default AP SSID.  Use the WiFi STA MAC so the name is stable even
	 * when the NCM netif is also present. */
	uint8_t mac[6] = {0};
	esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
	char hostname[32];
	if (mac_err == ESP_OK) {
		snprintf(hostname, sizeof(hostname), "%s-%02x%02x", MDSN_HOSTNAME_PREFIX,
		         mac[4], mac[5]);
	} else {
		ESP_LOGW(MDNS_TAG, "esp_read_mac failed (%s); using prefix-only hostname",
		         esp_err_to_name(mac_err));
		snprintf(hostname, sizeof(hostname), "%s", MDSN_HOSTNAME_PREFIX);
	}
	ESP_LOGI(MDNS_TAG, "mDNS hostname: %s.local", hostname);
	ESP_ERROR_CHECK(mdns_hostname_set(hostname));

	/* TODO: read instance description from NVS */
	ESP_ERROR_CHECK(mdns_instance_name_set(MDSN_INSTANCE_DESC));

	/* mac already read above (STA MAC); keep TXT suffix identical to hostname. */
	char value[64];
	snprintf(value, 63, "允斯调试器-%02X-%02X", mac[4], mac[5]);
	value[63] = '\0';

	//structure with TXT records
	mdns_txt_item_t serviceTxtData[] = {
		{"name", value},
	};

	//initialize service
	ESP_ERROR_CHECK(mdns_service_add("WebServer", "_http", "_tcp", 80,
									  serviceTxtData, sizeof(serviceTxtData)/sizeof(serviceTxtData[0])));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host("WebServer", "_http", "_tcp", NULL, "_server") );
}

int wt_mdns_set_hostname(const char *hostname)
{
	mdns_hostname_set(hostname);

	/* TODO save to NVS */
	return 0;
}
