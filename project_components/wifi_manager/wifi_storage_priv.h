#ifndef WIFI_STORAGE_PRIV_H_GUARD
#define WIFI_STORAGE_PRIV_H_GUARD

/* NVS keys of the legacy "wt_wifi" namespace (wifi_storage.c).
 *
 * The multi credential memory (docs/wifi-credential-memory.md) uses its own
 * "wifi_cred" namespace; the keys below are kept so a firmware upgrade can
 * migrate the last successful STA credential and the legacy AP credential.
 * The legacy keys are never deleted (§10 migration item: rollback possible).
 */
typedef enum wt_wifi_key_enum {
	KEY_WIFI_AP_CRED = 0x1,

	/* STA information */
	KEY_WIFI_STA_LAST_AP_CRED = 0x08, /* ssid[32] + password[64] */
	KEY_WIFI_STA_AP_BITMAP = 0x09,    /* 32 bit */

	KEY_WIFI_STA_STATIC_BASE = 0x10, /* wifi_api_sta_ap_static_info_t */

	KEY_WIFI_APSTA_MODE = 0xFF, /* 1B */
} wt_wifi_key;

#endif //WIFI_STORAGE_PRIV_H_GUARD
