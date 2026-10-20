#ifndef WIFI_CREDENTIAL_H_GUARD
#define WIFI_CREDENTIAL_H_GUARD

/*
 * Multi WiFi credential memory (WP-201 / WP-202).
 *
 * Frozen contract: docs/wifi-credential-memory.md
 *   - NVS partition "wt_nvs", namespace "wifi_cred"
 *   - BIRDIEBOT_WIFI_CRED_MAX records, each packed into a 103-byte blob
 *   - rotation order: last success -> priority asc / last_success desc -> idx
 *   - AP SSID default: "WiFi-Dap-XXXX" (XXXX = last 2 bytes of the STA
 *     MAC, upper case hex)
 *
 * This header contains the storage types / public API and the declarations of
 * the *pure* logic helpers (validation, blob pack/unpack, eviction and rotation
 * selection). The pure helpers live in wifi_cred_logic.c which only depends on
 * the C standard library, so they can be compiled and unit-tested on the host
 * without ESP-IDF (Wave 3 / WP-210). wifi_credential.c is the ESP/NVS backend.
 *
 * Return value convention for the ESP backend: 0 == success, otherwise an
 * esp_err_t value (ESP_ERR_INVALID_ARG -> JSON code 5, ESP_ERR_INVALID_STATE ->
 * JSON code 6, everything else -> JSON code 3; see wifi_api_json.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Compile time configuration (Kconfig: project_components/wifi_manager/Kconfig) */
/* ------------------------------------------------------------------------- */

#ifdef CONFIG_BIRDIEBOT_WIFI_CRED_MAX
#define BIRDIEBOT_WIFI_CRED_MAX CONFIG_BIRDIEBOT_WIFI_CRED_MAX
#else
/* Host builds / Kconfig missing: frozen contract default. */
#define BIRDIEBOT_WIFI_CRED_MAX 8
#endif

#if BIRDIEBOT_WIFI_CRED_MAX < 1 || BIRDIEBOT_WIFI_CRED_MAX > 8
#error "BIRDIEBOT_WIFI_CRED_MAX must be within [1, 8] (NVS schema, docs/wifi-credential-memory.md §1)"
#endif

#define BIRDIEBOT_WIFI_SSID_MAX 32
#define BIRDIEBOT_WIFI_PASS_MAX 64
#define BIRDIEBOT_WIFI_AP_PASS_MIN 8
#define BIRDIEBOT_WIFI_AP_PASS_MAX 63

#define BIRDIEBOT_WIFI_AP_SSID_PREFIX "WiFi-Dap-"

#define BIRDIEBOT_WIFI_CRED_SCHEMA_VER 1
#define BIRDIEBOT_WIFI_CRED_NONE 0xFF
#define BIRDIEBOT_WIFI_CRED_PRIORITY_DEFAULT 100

/* time(NULL) is only trusted after this point (contract §4: no SNTP this
 * release, unsynced devices store 0 = "never/unknown"). */
#define BIRDIEBOT_WIFI_CRED_TS_VALID_MIN 1700000000u

/* last_success_ts-only changes are debounced by 300 s to limit flash wear. */
#define BIRDIEBOT_WIFI_CRED_TS_DEBOUNCE_MS 300000u

/* ------------------------------------------------------------------------- */
/* On-flash record (NVS blob, fixed 103 bytes) -- contract §2.2               */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) birdiebot_wifi_cred_t {
	char ssid[33];        /* UTF-8, NUL terminated, 1..32 valid bytes */
	char password[65];    /* UTF-8, NUL terminated, 0 or 8..64 valid bytes */
	uint32_t last_success_ts; /* Unix seconds; 0 = never / clock not synced */
	uint8_t priority;     /* 0 = highest, 255 = lowest, default 100 */
} birdiebot_wifi_cred_t;

_Static_assert(sizeof(birdiebot_wifi_cred_t) == 103,
               "birdiebot_wifi_cred_t must stay 103 bytes (docs/wifi-credential-memory.md §2.2)");

/* cmd 11 (GET_SAVED_LIST) data source -- contract §8 */
typedef struct wifi_cred_list_item_t {
	uint8_t idx;
	char ssid[33];
	uint32_t last_success_ts;
	uint8_t priority;
} wifi_cred_list_item_t;

typedef struct wifi_cred_list_t {
	bool auto_save;
	uint8_t count;
	uint8_t connected_idx; /* BIRDIEBOT_WIFI_CRED_NONE == not connected */
	wifi_cred_list_item_t item[BIRDIEBOT_WIFI_CRED_MAX];
} wifi_cred_list_t;

/* One rotation candidate (persisted record or RAM-only credential, §3/§5). */
typedef struct wifi_cred_candidate_t {
	bool persistent;      /* false: RAM-only, never written to NVS / cmd 11 */
	uint8_t idx;          /* table index for persistent entries */
	char ssid[33];
	char password[65];
	uint32_t last_success_ts;
	uint8_t priority;
} wifi_cred_candidate_t;

/* ------------------------------------------------------------------------- */
/* Pure logic helpers (wifi_cred_logic.c, no ESP-IDF dependency)              */
/* ------------------------------------------------------------------------- */

/** Bounded strlen (never reads more than max bytes). */
size_t wifi_cred_strnlen(const char *str, size_t max);

/** SSID: 1..32 bytes, NUL terminated within 33 bytes. */
bool wifi_cred_ssid_valid(const char *ssid);

/** STA password: empty (open network) or 8..64 bytes. */
bool wifi_cred_password_valid(const char *password);

/** SoftAP password: 8..63 bytes (WPA2/WPA3-PSK). */
bool wifi_cred_ap_password_valid(const char *password);

/** Copy an SSID into a char[33] buffer. Returns 0 on success. */
int wifi_cred_ssid_copy(char dst[BIRDIEBOT_WIFI_SSID_MAX + 1], const char *src);

/** Copy a password into a char[65] buffer. Returns 0 on success. */
int wifi_cred_password_copy(char dst[BIRDIEBOT_WIFI_PASS_MAX + 1], const char *src);

/** Exact-byte SSID lookup (case sensitive, no trimming, contract §4.1). */
int wifi_cred_find_ssid(const birdiebot_wifi_cred_t *table, uint8_t count, const char *ssid);

/**
 * Choose the record to evict (contract §4.4): highest priority value, then
 * oldest last_success_ts (0 = oldest), then lowest index. The currently
 * connecting entry (@p protect_idx) is never evicted unless it is the only one.
 * @return index, or -1 when nothing can be evicted.
 */
int wifi_cred_evict_index(const birdiebot_wifi_cred_t *table, uint8_t count, uint8_t protect_idx);

/**
 * Build the persisted rotation order (contract §5.1 step 1+2):
 * last success first, then priority ascending, last_success_ts descending
 * (0 last), index ascending. Entries whose skip_mask[idx] != 0 are skipped
 * (runtime "credential failed this power cycle" marks, §4.5).
 * @param skip_mask optional array of BIRDIEBOT_WIFI_CRED_MAX bytes, or NULL.
 * @return number of indices written (<= max).
 */
int wifi_cred_rotate_order(const birdiebot_wifi_cred_t *table, uint8_t count, uint8_t last_idx,
                           const uint8_t *skip_mask, uint8_t *order, uint8_t max);

/** Clamp a unix timestamp to the contract "trusted time" rules. */
uint32_t wifi_cred_normalize_ts(uint64_t unix_seconds);

/** Default SoftAP SSID from a 6-byte MAC ("WiFi-Dap-XXXX", contract §6). */
int wifi_cred_make_default_ap_ssid(const uint8_t mac[6], char out[BIRDIEBOT_WIFI_SSID_MAX + 1]);

/** Fill a record from strings (also used by migration). Returns 0 on success. */
int wifi_cred_record_set(birdiebot_wifi_cred_t *record, const char *ssid, const char *password,
                         uint32_t last_success_ts, uint8_t priority);

/** Serialize a record into its fixed 103-byte NVS blob. Returns 0 on success. */
int wifi_cred_record_pack(const birdiebot_wifi_cred_t *record, uint8_t *buf, size_t buf_len);

/**
 * Validate/deserialize a 103-byte NVS blob (§2.4): wrong length, bad SSID or
 * bad password -> -1 and the record is skipped by the loader.
 */
int wifi_cred_record_unpack(const uint8_t *buf, size_t buf_len, birdiebot_wifi_cred_t *out);

/* ------------------------------------------------------------------------- */
/* ESP-IDF backend (wifi_credential.c)                                       */
/* ------------------------------------------------------------------------- */

/** Load/migrate the credential table from NVS (idempotent, call early in init). */
int wifi_cred_mem_init(void);

/** cmd 11 data source: persisted records only, optional connected_idx. */
int wifi_cred_mem_get_list(wifi_cred_list_t *out);

/** cmd 12: upsert a record. On success sets idx/updated and optional evicted idx. */
int wifi_cred_mem_add(const char *ssid, const char *password, int priority,
                      uint8_t *out_idx, bool *updated, int *evicted_idx);

/** cmd 13: delete a persisted record and compact the table. */
int wifi_cred_mem_del(uint8_t idx);

/** cmd 14: enable/disable automatic memory. */
int wifi_cred_mem_set_auto_save(bool enable);
bool wifi_cred_mem_get_auto_save(void);

/** Note a successfully established connection (contract §4). */
int wifi_cred_mem_note_success(const char *ssid, const char *password, bool may_persist);

/** Rotation candidates: persisted order first, then RAM-only entries. */
int wifi_cred_mem_rotation_plan(wifi_cred_candidate_t *out, uint8_t max);

/** Index order of persisted records only (contract §8 helper). */
int wifi_cred_mem_rotate_order(uint8_t *order, uint8_t max);

/** cmd 8: store custom AP SSID/password. Empty ssid restores the MAC default. */
int wifi_cred_mem_set_ap_cred(const char *ssid, const char *password);

/** Effective command 5/8 AP credential (custom value wins, else dynamic default). */
int wifi_cred_mem_get_ap_cred(char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1],
                              char password[BIRDIEBOT_WIFI_PASS_MAX + 1]);

/** Compute the MAC based default AP SSID ("WiFi-Dap-XXXX"). */
bool wifi_cred_mem_ap_ssid_default(char out[BIRDIEBOT_WIFI_SSID_MAX + 1]);

/** Track the currently connected SSID for GET_SAVED_LIST.connected_idx. */
void wifi_cred_mem_set_connected_ssid(const char *ssid);

/** Mark a credential as "do not retry this power cycle" after auth failure (§4.5). */
void wifi_cred_mem_note_connect_failure(const char *ssid, bool credential_failure);

/** Manual CONNECT / ADD_SAVED gives credentials another chance. */
void wifi_cred_mem_clear_failures(void);

/** Pick the boot credential (last success first). Returns 0 when one exists. */
int wifi_cred_mem_initial_cred(char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1],
                               char password[BIRDIEBOT_WIFI_PASS_MAX + 1]);

/** OTA/diagnostics hook: temporarily pause rotation (contract §5.8/§9.3). */
void wifi_cred_mem_pause_rotation(bool pause);
bool wifi_cred_mem_rotation_paused(void);

#endif /* WIFI_CREDENTIAL_H_GUARD */
