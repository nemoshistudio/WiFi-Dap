#ifndef WIFI_CONFIGURATION_H_GUARD
#define WIFI_CONFIGURATION_H_GUARD

#define WIFI_DEFAULT_HOSTNAME "无线DAP"

/*
 * SoftAP defaults. The default SSID is *dynamic* (docs/wifi-credential-memory.md
 * §6): "WiFi-Dap-XXXX" with XXXX = last two bytes of the STA MAC, upper
 * case hex. The user defined SSID stored in NVS wins. See wifi_credential.c
 * (wifi_cred_mem_get_ap_cred / wifi_cred_mem_ap_ssid_default).
 */
#define WIFI_DEFAULT_AP_PASS "12345678" /* fallback when CONFIG_BIRDIEBOT_AP_PASS is unset */
#define WIFI_DEFAULT_AP_IP      192, 168, 1, 1
#define WIFI_DEFAULT_AP_GATEWAY 192, 168, 1, 1
#define WIFI_DEFAULT_AP_NETMASK 255, 255, 255, 0

#if defined CONFIG_IDF_TARGET_ESP32
	#define WIFI_LED_ENABLE 0
#elif defined CONFIG_IDF_TARGET_ESP32C3
	#define WIFI_LED_ENABLE 0
#elif defined CONFIG_IDF_TARGET_ESP32S3
	#define WIFI_LED_ENABLE 0
#else
	#define WIFI_LED_ENABLE 0
#endif

#define WIFI_LED_PIN 9

#define IP4_ADDR_EXPAND(...) IP4_ADDR(__VA_ARGS__)

#endif //WIFI_CONFIGURATION_H_GUARD
