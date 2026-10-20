/*
 * Multi WiFi credential memory -- NVS backend (WP-201).
 *
 * Frozen contract: docs/wifi-credential-memory.md
 *
 * Storage layout (`wt_nvs` partition, namespace "wifi_cred"):
 *   schema_ver (u8) | auto_save (u8) | count (u8) | last_idx (u8)
 *   ap_ssid (str) | ap_pass (str) | cred_0..cred_7 (blob, 103 bytes)
 *
 * Everything below assumes the `wt_nvs` partition has been initialised by
 * wt_storage_init()/wt_nvs_init() (main.c does this before wifi_manager_init).
 * All table/NVS mutations are serialized with this module's own semaphore; the
 * ESP layer never writes NVS from an ISR.
 *
 * Pure helpers (validation, blob codec, eviction/rotation selection) live in
 * wifi_cred_logic.c so they can be host-tested (Wave 3 / WP-210).
 */

#include "wifi_credential.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include "wt_nvs.h"
#include "wifi_configuration.h"
#include "wifi_storage.h" /* legacy "wt_wifi" namespace, read-only migration */

#define TAG __FILENAME__

#define WIFI_CRED_NVS_NAMESPACE "wifi_cred"
#define WIFI_CRED_NVS_PARTITION "wt_nvs"

#define WIFI_CRED_KEY_SCHEMA "schema_ver"
#define WIFI_CRED_KEY_AUTO_SAVE "auto_save"
#define WIFI_CRED_KEY_COUNT "count"
#define WIFI_CRED_KEY_LAST_IDX "last_idx"
#define WIFI_CRED_KEY_AP_SSID "ap_ssid"
#define WIFI_CRED_KEY_AP_PASS "ap_pass"
#define WIFI_CRED_KEY_PREFIX "cred_"

#ifdef CONFIG_BIRDIEBOT_AP_PASS
#define WIFI_CRED_DEFAULT_AP_PASS CONFIG_BIRDIEBOT_AP_PASS
#else
#define WIFI_CRED_DEFAULT_AP_PASS WIFI_DEFAULT_AP_PASS
#endif

typedef struct wifi_cred_ctx_t {
	SemaphoreHandle_t lock;
	bool initialized;
	/* schema_ver > 1 from a newer firmware: run empty and refuse writes (§2.4.4) */
	bool future_schema;
	bool auto_save;
	uint8_t count;
	uint8_t last_idx; /* BIRDIEBOT_WIFI_CRED_NONE == no recent success */
	birdiebot_wifi_cred_t table[BIRDIEBOT_WIFI_CRED_MAX];

	/* RAM-only credentials of this power cycle (§3 RAM-only exception) */
	birdiebot_wifi_cred_t volatile_table[BIRDIEBOT_WIFI_CRED_MAX];
	uint8_t volatile_count;
	uint8_t volatile_last_idx;

	/* runtime "credential failed this power cycle" marks (§4.5) */
	uint8_t no_retry_mask[BIRDIEBOT_WIFI_CRED_MAX];
	uint8_t volatile_no_retry_mask[BIRDIEBOT_WIFI_CRED_MAX];

	bool rotation_paused;
	TickType_t last_flush_tick; /* ticks of the last successful NVS flush */
	bool dirty;                 /* ts-only change pending (300 s debounce) */

	bool connected_ssid_set;
	char connected_ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];

	char ap_ssid[BIRDIEBOT_WIFI_SSID_MAX + 1]; /* empty => MAC based default */
	char ap_pass[BIRDIEBOT_WIFI_PASS_MAX + 1];
} wifi_cred_ctx_t;

static wifi_cred_ctx_t cred;

/* ------------------------------------------------------------------------- */
/* small helpers                                                            */
/* ------------------------------------------------------------------------- */

static bool cred_ready(void)
{
	return cred.initialized && cred.lock != NULL;
}

static void cred_lock(void)
{
	if (cred.lock != NULL) {
		xSemaphoreTake(cred.lock, portMAX_DELAY);
	}
}

static void cred_unlock(void)
{
	if (cred.lock != NULL) {
		xSemaphoreGive(cred.lock);
	}
}

static void cred_key(uint8_t idx, char key[16])
{
	snprintf(key, 16, WIFI_CRED_KEY_PREFIX "%u", (unsigned)idx);
}

static esp_err_t cred_nvs_open(nvs_handle_t *out_handle)
{
	return nvs_open_from_partition(WIFI_CRED_NVS_PARTITION, WIFI_CRED_NVS_NAMESPACE,
	                               NVS_READWRITE, out_handle);
}

/* Reset the RAM table to the "empty NVS" defaults. Caller holds the lock. */
static void cred_reset_locked(void)
{
	memset(cred.table, 0, sizeof(cred.table));
	memset(cred.volatile_table, 0, sizeof(cred.volatile_table));
	memset(cred.no_retry_mask, 0, sizeof(cred.no_retry_mask));
	memset(cred.volatile_no_retry_mask, 0, sizeof(cred.volatile_no_retry_mask));
	cred.count = 0;
	cred.last_idx = BIRDIEBOT_WIFI_CRED_NONE;
	cred.volatile_count = 0;
	cred.volatile_last_idx = BIRDIEBOT_WIFI_CRED_NONE;
	cred.auto_save = true;
	cred.future_schema = false;
	cred.rotation_paused = false;
	cred.last_flush_tick = 0;
	cred.dirty = false;
	cred.connected_ssid_set = false;
	memset(cred.connected_ssid, 0, sizeof(cred.connected_ssid));
	memset(cred.ap_ssid, 0, sizeof(cred.ap_ssid));
	memset(cred.ap_pass, 0, sizeof(cred.ap_pass));
	if (wifi_cred_password_copy(cred.ap_pass, WIFI_CRED_DEFAULT_AP_PASS) != 0) {
		/* Defensive: a malformed Kconfig value falls back to the contract default. */
		wifi_cred_password_copy(cred.ap_pass, WIFI_DEFAULT_AP_PASS);
	}
}

/* Volume credential keys 0..count-1 and erase stale trailing blobs. */
static esp_err_t cred_write_records_locked(nvs_handle_t handle)
{
	char key[16];
	esp_err_t err = ESP_OK;

	for (uint8_t i = 0; i < cred.count; ++i) {
		cred_key(i, key);
		err = nvs_set_blob(handle, key, &cred.table[i], sizeof(cred.table[i]));
		if (err != ESP_OK) {
			return err;
		}
	}
	for (uint8_t i = cred.count; i < BIRDIEBOT_WIFI_CRED_MAX; ++i) {
		cred_key(i, key);
		esp_err_t del = nvs_erase_key(handle, key);
		if (del != ESP_OK && del != ESP_ERR_NVS_NOT_FOUND) {
			return del;
		}
	}
	/* contract §2.1: records first, metadata afterwards */
	return nvs_commit(handle);
}

static esp_err_t cred_write_meta_locked(nvs_handle_t handle)
{
	esp_err_t err = nvs_set_u8(handle, WIFI_CRED_KEY_COUNT, cred.count);
	if (err == ESP_OK) {
		err = nvs_set_u8(handle, WIFI_CRED_KEY_LAST_IDX, cred.last_idx);
	}
	if (err == ESP_OK) {
		err = nvs_commit(handle);
	}
	return err;
}

/* Full flush of the RAM table to NVS (records then metadata). Caller holds lock. */
static esp_err_t cred_flush_locked(bool all_records, int record_idx)
{
	nvs_handle_t handle;
	esp_err_t err;

	err = cred_nvs_open(&handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
		return err;
	}

	if (all_records) {
		err = cred_write_records_locked(handle);
	} else if (record_idx >= 0 && record_idx < cred.count) {
		char key[16];
		cred_key((uint8_t)record_idx, key);
		err = nvs_set_blob(handle, key, &cred.table[record_idx], sizeof(cred.table[record_idx]));
		if (err == ESP_OK) {
			err = nvs_commit(handle);
		}
	} else {
		err = ESP_OK;
	}

	if (err == ESP_OK) {
		err = cred_write_meta_locked(handle);
	}

	nvs_close(handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "credential flush failed: %s", esp_err_to_name(err));
	}
	return err;
}

/* ------------------------------------------------------------------------- */
/* load / migration / repairs (contract §2.4, §7.1 migration item)           */
/* ------------------------------------------------------------------------- */

static void cred_load_table_locked(nvs_handle_t handle)
{
	uint8_t stored_count = 0;
	uint8_t valid = 0;
	uint8_t stored_last = BIRDIEBOT_WIFI_CRED_NONE;
	bool repaired = false;
	bool last_idx_untrusted = false;

	if (nvs_get_u8(handle, WIFI_CRED_KEY_COUNT, &stored_count) != ESP_OK) {
		stored_count = 0;
	}
	if (stored_count > BIRDIEBOT_WIFI_CRED_MAX) {
		ESP_LOGW(TAG, "stored count %u > %u, clamping", stored_count, BIRDIEBOT_WIFI_CRED_MAX);
		stored_count = BIRDIEBOT_WIFI_CRED_MAX;
		repaired = true;
	}

	/* Contract §2.4.2: rebuild count from the blobs that are actually readable
	 * instead of trusting a count that may be stale after a power loss. Valid
	 * records are compacted preserving their order; a hole makes last_idx
	 * untrustworthy and resets it. */
	for (uint8_t i = 0; i < BIRDIEBOT_WIFI_CRED_MAX; ++i) {
		char key[16];
		size_t len = 0;
		birdiebot_wifi_cred_t raw;
		esp_err_t err;

		cred_key(i, key);
		err = nvs_get_blob(handle, key, NULL, &len);
		if (err == ESP_ERR_NVS_NOT_FOUND) {
			continue;
		}
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "cred_%u unreadable (%s), skipped", i, esp_err_to_name(err));
			repaired = true;
			continue;
		}
		if (len != sizeof(raw)) {
			ESP_LOGW(TAG, "cred_%u bad blob size %u != %u, skipped",
			         i, (unsigned)len, (unsigned)sizeof(raw));
			repaired = true;
			continue;
		}
		err = nvs_get_blob(handle, key, &raw, &len);
		if (err != ESP_OK) {
			repaired = true;
			continue;
		}
		if (wifi_cred_record_unpack((const uint8_t *)&raw, len, &cred.table[valid]) != 0) {
			ESP_LOGW(TAG, "cred_%u invalid record, skipped", i);
			repaired = true;
			continue;
		}
		if (i != valid) {
			ESP_LOGW(TAG, "cred_%u moved to slot %u while repairing", i, valid);
			repaired = true;
			last_idx_untrusted = true;
		}
		++valid;
	}

	if (nvs_get_u8(handle, WIFI_CRED_KEY_LAST_IDX, &stored_last) != ESP_OK) {
		stored_last = BIRDIEBOT_WIFI_CRED_NONE;
	}

	cred.count = valid;
	if (last_idx_untrusted) {
		cred.last_idx = BIRDIEBOT_WIFI_CRED_NONE;
		repaired = true;
	} else if (stored_last < cred.count) {
		cred.last_idx = stored_last;
	} else {
		cred.last_idx = BIRDIEBOT_WIFI_CRED_NONE;
		if (stored_last != BIRDIEBOT_WIFI_CRED_NONE) {
			ESP_LOGW(TAG, "last_idx %u out of range (%u), reset", stored_last, cred.count);
			repaired = true;
		}
	}
	if (stored_count != valid) {
		ESP_LOGW(TAG, "count rebuilt: %u -> %u", stored_count, valid);
		repaired = true;
	}

	if (repaired) {
		/* Best effort repair; a failure here is not fatal -- the table stays
		 * usable in RAM and the next write retries. */
		esp_err_t err = cred_write_records_locked(handle);
		if (err == ESP_OK) {
			err = cred_write_meta_locked(handle);
		}
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "table repair write failed: %s", esp_err_to_name(err));
		}
	}
}

/*
 * Import the last successful credential of the old "wt_wifi" namespace into
 * index 0 (§10 migration item). The legacy keys are intentionally *not*
 * deleted so a firmware rollback can still read them.
 * @return 0 migrated, 1 nothing to migrate.
 */
static int cred_migrate_legacy_locked(void)
{
	wifi_credential_t legacy;
	char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];
	char password[BIRDIEBOT_WIFI_PASS_MAX + 1];
	size_t len;

	if (wifi_data_get_sta_last_conn_cred(&legacy) != 0) {
		return 1;
	}

	/* Legacy buffers are fixed size and may not be NUL terminated. */
	len = wifi_cred_strnlen(legacy.ssid, sizeof(legacy.ssid));
	if (len == 0 || len > BIRDIEBOT_WIFI_SSID_MAX) {
		return 1;
	}
	memcpy(ssid, legacy.ssid, len);
	ssid[len] = '\0';

	len = wifi_cred_strnlen(legacy.password, sizeof(legacy.password));
	if (len > BIRDIEBOT_WIFI_PASS_MAX) {
		len = 0;
	}
	memcpy(password, legacy.password, len);
	password[len] = '\0';
	if (!wifi_cred_password_valid(password)) {
		ESP_LOGW(TAG, "legacy STA password invalid, skipping migration");
		return 1;
	}

	if (wifi_cred_record_set(&cred.table[0], ssid, password, 0,
	                         BIRDIEBOT_WIFI_CRED_PRIORITY_DEFAULT) != 0) {
		return 1;
	}
	cred.count = 1;
	cred.last_idx = 0;
	ESP_LOGI(TAG, "migrated legacy STA credential (ssid=\"%s\") to cred_0", ssid);
	return 0;
}

static void cred_load_ap_cred_locked(nvs_handle_t handle)
{
	size_t len;
	bool pass_from_nvs = false;
	wifi_credential_t legacy_ap;

	cred.ap_ssid[0] = '\0';
	len = sizeof(cred.ap_ssid);
	if (nvs_get_str(handle, WIFI_CRED_KEY_AP_SSID, cred.ap_ssid, &len) != ESP_OK) {
		cred.ap_ssid[0] = '\0';
	}
	cred.ap_ssid[BIRDIEBOT_WIFI_SSID_MAX] = '\0';
	if (cred.ap_ssid[0] != '\0' && !wifi_cred_ssid_valid(cred.ap_ssid)) {
		ESP_LOGW(TAG, "stored ap_ssid invalid, falling back to default");
		cred.ap_ssid[0] = '\0';
	}

	len = sizeof(cred.ap_pass);
	if (nvs_get_str(handle, WIFI_CRED_KEY_AP_PASS, cred.ap_pass, &len) == ESP_OK) {
		cred.ap_pass[BIRDIEBOT_WIFI_PASS_MAX] = '\0';
		pass_from_nvs = true;
	}
	if (!wifi_cred_ap_password_valid(cred.ap_pass)) {
		wifi_cred_password_copy(cred.ap_pass, WIFI_CRED_DEFAULT_AP_PASS);
		pass_from_nvs = false;
	}

	/* Upgrade path: honour AP credentials written by the old firmware
	 * (namespace "wt_wifi") until the user saves new ones. Nothing is written
	 * here, so a rollback keeps the old values. */
	if ((cred.ap_ssid[0] == '\0' || !pass_from_nvs) &&
	    wifi_data_get_ap_credential(&legacy_ap) == 0) {
		if (cred.ap_ssid[0] == '\0') {
			len = wifi_cred_strnlen(legacy_ap.ssid, sizeof(legacy_ap.ssid));
			if (len > 0 && len <= BIRDIEBOT_WIFI_SSID_MAX) {
				memcpy(cred.ap_ssid, legacy_ap.ssid, len);
				cred.ap_ssid[len] = '\0';
				ESP_LOGI(TAG, "using legacy custom AP SSID");
			}
		}
		if (!pass_from_nvs) {
			len = wifi_cred_strnlen(legacy_ap.password, sizeof(legacy_ap.password));
			if (len >= BIRDIEBOT_WIFI_AP_PASS_MIN && len <= BIRDIEBOT_WIFI_AP_PASS_MAX) {
				memcpy(cred.ap_pass, legacy_ap.password, len);
				cred.ap_pass[len] = '\0';
				ESP_LOGI(TAG, "using legacy custom AP password");
			}
		}
	}
}

static esp_err_t cred_seed_locked(nvs_handle_t handle, bool migrated)
{
	esp_err_t err;

	ESP_LOGI(TAG, "seeding wifi_cred namespace (migrated=%d)", migrated ? 1 : 0);
	err = nvs_set_u8(handle, WIFI_CRED_KEY_SCHEMA, BIRDIEBOT_WIFI_CRED_SCHEMA_VER);
	if (err != ESP_OK) {
		return err;
	}
	err = nvs_set_u8(handle, WIFI_CRED_KEY_AUTO_SAVE, cred.auto_save ? 1 : 0);
	if (err != ESP_OK) {
		return err;
	}
	if (migrated) {
		err = cred_write_records_locked(handle);
		if (err != ESP_OK) {
			return err;
		}
	}
	err = cred_write_meta_locked(handle);
	if (err != ESP_OK) {
		return err;
	}
	return nvs_commit(handle);
}

/* ------------------------------------------------------------------------- */
/* RAM-only credential helpers (contract §3 RAM-only exception)              */
/* ------------------------------------------------------------------------- */

static void cred_volatile_remove_locked(const char *ssid)
{
	for (uint8_t i = 0; i < cred.volatile_count; ++i) {
		if (strncmp(cred.volatile_table[i].ssid, ssid, BIRDIEBOT_WIFI_SSID_MAX + 1) != 0) {
			continue;
		}
		for (uint8_t j = i; j + 1 < cred.volatile_count; ++j) {
			cred.volatile_table[j] = cred.volatile_table[j + 1];
			cred.volatile_no_retry_mask[j] = cred.volatile_no_retry_mask[j + 1];
		}
		--cred.volatile_count;
		memset(&cred.volatile_table[cred.volatile_count], 0,
		       sizeof(cred.volatile_table[0]));
		cred.volatile_no_retry_mask[cred.volatile_count] = 0;
		if (cred.volatile_last_idx == i) {
			cred.volatile_last_idx = BIRDIEBOT_WIFI_CRED_NONE;
		} else if (cred.volatile_last_idx != BIRDIEBOT_WIFI_CRED_NONE &&
		           cred.volatile_last_idx > i) {
			--cred.volatile_last_idx;
		}
		return;
	}
}

static void cred_volatile_upsert_locked(const char *ssid, const char *password, uint32_t ts)
{
	int idx = wifi_cred_find_ssid(cred.volatile_table, cred.volatile_count, ssid);

	if (idx < 0) {
		if (cred.volatile_count < BIRDIEBOT_WIFI_CRED_MAX) {
			idx = cred.volatile_count;
			++cred.volatile_count;
		} else {
			/* Replace the oldest entry (min last_success_ts, then min idx). */
			uint8_t oldest = 0;
			for (uint8_t i = 1; i < cred.volatile_count; ++i) {
				if (cred.volatile_table[i].last_success_ts <
				    cred.volatile_table[oldest].last_success_ts) {
					oldest = i;
				}
			}
			idx = oldest;
			cred.volatile_no_retry_mask[idx] = 0;
		}
	}
	wifi_cred_record_set(&cred.volatile_table[idx], ssid, password, ts,
	                     BIRDIEBOT_WIFI_CRED_PRIORITY_DEFAULT);
	cred.volatile_last_idx = (uint8_t)idx;
	cred.volatile_no_retry_mask[idx] = 0;
}

/* ------------------------------------------------------------------------- */
/* upsert (shared by cmd 12 / connection success / migration)               */
/* ------------------------------------------------------------------------- */

/*
 * @param priority -1 keeps the existing priority on update (default 100 on insert)
 * @param ts        last_success_ts to store; ignored on update when keep_ts
 * @param keep_ts   true => an existing record keeps its last_success_ts
 * @param set_last  true => last_idx follows the record (connection success)
 */
static int cred_upsert_locked(const char *ssid, const char *password, uint32_t ts,
                              int priority, bool keep_ts, bool set_last,
                              bool *updated, int *evicted_idx, uint8_t *out_idx)
{
	int idx = wifi_cred_find_ssid(cred.table, cred.count, ssid);
	esp_err_t err;

	if (updated != NULL) {
		*updated = false;
	}
	if (evicted_idx != NULL) {
		*evicted_idx = -1;
	}

	if (idx >= 0) {
		birdiebot_wifi_cred_t backup = cred.table[idx];
		uint8_t backup_last = cred.last_idx;

		if (wifi_cred_password_copy(cred.table[idx].password, password) != 0) {
			return ESP_ERR_INVALID_ARG;
		}
		if (priority >= 0) {
			cred.table[idx].priority = (uint8_t)priority;
		}
		if (!keep_ts) {
			cred.table[idx].last_success_ts = ts;
		}
		if (set_last) {
			cred.last_idx = (uint8_t)idx;
		}
		cred.no_retry_mask[idx] = 0;
		err = cred_flush_locked(true, -1);
		if (err != ESP_OK) {
			cred.table[idx] = backup;
			cred.last_idx = backup_last;
			return err;
		}
		if (updated != NULL) {
			*updated = true;
		}
		if (out_idx != NULL) {
			*out_idx = (uint8_t)idx;
		}
		return ESP_OK;
	}

	{
		uint8_t new_idx;
		birdiebot_wifi_cred_t backup;
		uint8_t backup_count = cred.count;
		uint8_t backup_last = cred.last_idx;
		bool had_eviction = false;
		int evicted = -1;

		if (cred.count < BIRDIEBOT_WIFI_CRED_MAX) {
			new_idx = cred.count;
			++cred.count;
		} else {
			evicted = wifi_cred_evict_index(cred.table, cred.count, cred.last_idx);
			if (evicted < 0) {
				/* Only the currently connecting entry exists: contract §4.4
				 * allows replacing it as a last resort. */
				evicted = (cred.last_idx < cred.count) ? (int)cred.last_idx : 0;
			}
			backup = cred.table[evicted];
			had_eviction = true;
			new_idx = (uint8_t)evicted;
		}
		if (wifi_cred_record_set(&cred.table[new_idx], ssid, password, ts,
		                         priority >= 0 ? (uint8_t)priority
		                                       : BIRDIEBOT_WIFI_CRED_PRIORITY_DEFAULT) != 0) {
			cred.count = backup_count;
			return ESP_ERR_INVALID_ARG;
		}
		cred.no_retry_mask[new_idx] = 0;
		if (set_last) {
			cred.last_idx = new_idx;
		}
		err = cred_flush_locked(true, -1);
		if (err != ESP_OK) {
			if (had_eviction) {
				cred.table[new_idx] = backup;
			} else {
				memset(&cred.table[new_idx], 0, sizeof(cred.table[new_idx]));
			}
			cred.count = backup_count;
			cred.last_idx = backup_last;
			return err;
		}
		if (evicted_idx != NULL) {
			*evicted_idx = evicted;
		}
		if (out_idx != NULL) {
			*out_idx = new_idx;
		}
		return ESP_OK;
	}
}

/* ------------------------------------------------------------------------- */
/* public API                                                               */
/* ------------------------------------------------------------------------- */

int wifi_cred_mem_init(void)
{
	nvs_handle_t handle;
	esp_err_t err;
	uint8_t schema = 0;

	if (cred.initialized) {
		return ESP_OK;
	}

	cred.lock = xSemaphoreCreateBinary();
	if (cred.lock == NULL) {
		ESP_LOGE(TAG, "cannot create credential lock");
		return ESP_ERR_NO_MEM;
	}
	xSemaphoreGive(cred.lock);
	cred_reset_locked();

	err = cred_nvs_open(&handle);
	if (err != ESP_OK) {
		/* Run with an empty in-RAM table; JSON writes will report code 3. */
		ESP_LOGE(TAG, "NVS %s/%s unavailable: %s", WIFI_CRED_NVS_PARTITION,
		         WIFI_CRED_NVS_NAMESPACE, esp_err_to_name(err));
		cred.initialized = true;
		return err;
	}

	esp_err_t schema_err = nvs_get_u8(handle, WIFI_CRED_KEY_SCHEMA, &schema);
	if (schema_err == ESP_ERR_NVS_NOT_FOUND || (schema_err == ESP_OK && schema == 0)) {
		int migrated = cred_migrate_legacy_locked();
		err = cred_seed_locked(handle, migrated == 0);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "seeding credentials failed: %s", esp_err_to_name(err));
		}
	} else if (schema_err != ESP_OK) {
		ESP_LOGE(TAG, "reading schema_ver failed: %s", esp_err_to_name(schema_err));
	} else if (schema == BIRDIEBOT_WIFI_CRED_SCHEMA_VER) {
		cred_load_table_locked(handle);
	} else {
		/* §2.4.4: unknown future schema: run empty, never write back. */
		cred.future_schema = true;
		ESP_LOGW(TAG, "unknown credential schema %u (newer firmware?), running empty",
		         schema);
	}

	/* auto_save defaults to true when missing (§2.3). */
	{
		uint8_t auto_save_u8 = 1;
		if (nvs_get_u8(handle, WIFI_CRED_KEY_AUTO_SAVE, &auto_save_u8) != ESP_OK) {
			auto_save_u8 = 1;
		}
		cred.auto_save = auto_save_u8 != 0;
	}
	cred_load_ap_cred_locked(handle);

	nvs_close(handle);
	cred.initialized = true;
	ESP_LOGI(TAG, "credential memory ready: count=%u auto_save=%d last_idx=%u",
	         cred.count, cred.auto_save ? 1 : 0, cred.last_idx);
	return ESP_OK;
}

int wifi_cred_mem_get_list(wifi_cred_list_t *out)
{
	if (out == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	memset(out, 0, sizeof(*out));
	out->connected_idx = BIRDIEBOT_WIFI_CRED_NONE;
	/* cmd 11 incremental field (docs/wifi-credential-memory.md §7.1, b015a0c):
	 * the device side limit; clients must not hard-code 8. */
	out->max = BIRDIEBOT_WIFI_CRED_MAX;
	if (!cred_ready()) {
		out->auto_save = true;
		return ESP_ERR_INVALID_STATE;
	}

	cred_lock();
	out->auto_save = cred.auto_save;
	out->count = cred.count;
	for (uint8_t i = 0; i < cred.count; ++i) {
		out->item[i].idx = i;
		wifi_cred_ssid_copy(out->item[i].ssid, cred.table[i].ssid);
		out->item[i].last_success_ts = cred.table[i].last_success_ts;
		out->item[i].priority = cred.table[i].priority;
	}
	if (cred.connected_ssid_set) {
		int idx = wifi_cred_find_ssid(cred.table, cred.count, cred.connected_ssid);
		if (idx >= 0) {
			out->connected_idx = (uint8_t)idx;
		}
	}
	cred_unlock();
	return ESP_OK;
}

int wifi_cred_mem_add(const char *ssid, const char *password, int priority,
                      uint8_t *out_idx, bool *updated, int *evicted_idx)
{
	int ret;

	if (!wifi_cred_ssid_valid(ssid) || !wifi_cred_password_valid(password)) {
		return ESP_ERR_INVALID_ARG;
	}
	if (priority < 0 || priority > 255) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!cred_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	cred_lock();
	if (cred.future_schema) {
		cred_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	ret = cred_upsert_locked(ssid, password, 0, priority, true, false,
	                         updated, evicted_idx, out_idx);
	if (ret == ESP_OK) {
		cred_volatile_remove_locked(ssid);
	}
	cred_unlock();
	return ret;
}

int wifi_cred_mem_del(uint8_t idx)
{
	birdiebot_wifi_cred_t removed;
	uint8_t old_last;
	uint8_t old_mask[BIRDIEBOT_WIFI_CRED_MAX];
	esp_err_t err;

	if (!cred_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	cred_lock();
	if (cred.future_schema) {
		cred_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	if (idx >= cred.count) {
		cred_unlock();
		return ESP_ERR_INVALID_ARG;
	}

	removed = cred.table[idx];
	old_last = cred.last_idx;
	memcpy(old_mask, cred.no_retry_mask, sizeof(old_mask));

	for (uint8_t i = idx; i + 1 < cred.count; ++i) {
		cred.table[i] = cred.table[i + 1];
	}
	memset(&cred.table[cred.count - 1], 0, sizeof(cred.table[0]));
	--cred.count;
	for (uint8_t i = idx; i + 1 < BIRDIEBOT_WIFI_CRED_MAX; ++i) {
		cred.no_retry_mask[i] = cred.no_retry_mask[i + 1];
	}
	cred.no_retry_mask[BIRDIEBOT_WIFI_CRED_MAX - 1] = 0;

	if (old_last == idx) {
		cred.last_idx = BIRDIEBOT_WIFI_CRED_NONE;
	} else if (old_last != BIRDIEBOT_WIFI_CRED_NONE && old_last > idx) {
		cred.last_idx = (uint8_t)(old_last - 1);
	} else {
		cred.last_idx = old_last;
	}

	err = cred_flush_locked(true, -1);
	if (err != ESP_OK) {
		/* Roll back the compaction so RAM stays consistent with NVS. */
		for (uint8_t i = cred.count; i > idx; --i) {
			cred.table[i] = cred.table[i - 1];
		}
		cred.table[idx] = removed;
		++cred.count;
		cred.last_idx = old_last;
		memcpy(cred.no_retry_mask, old_mask, sizeof(old_mask));
		cred_unlock();
		return err;
	}
	cred_unlock();
	return ESP_OK;
}

int wifi_cred_mem_set_auto_save(bool enable)
{
	nvs_handle_t handle;
	esp_err_t err;

	if (!cred_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	cred_lock();
	if (cred.future_schema) {
		cred_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	if (cred.auto_save == enable) {
		cred_unlock();
		return ESP_OK;
	}

	err = cred_nvs_open(&handle);
	if (err != ESP_OK) {
		cred_unlock();
		return err;
	}
	err = nvs_set_u8(handle, WIFI_CRED_KEY_AUTO_SAVE, enable ? 1 : 0);
	if (err == ESP_OK) {
		err = nvs_commit(handle);
	}
	nvs_close(handle);
	if (err != ESP_OK) {
		cred_unlock();
		return err;
	}
	cred.auto_save = enable;
	cred_unlock();
	return ESP_OK;
}

bool wifi_cred_mem_get_auto_save(void)
{
	bool auto_save;

	if (!cred_ready()) {
		return true;
	}
	cred_lock();
	auto_save = cred.auto_save;
	cred_unlock();
	return auto_save;
}

int wifi_cred_mem_note_success(const char *ssid, const char *password, bool may_persist)
{
	uint32_t ts;
	esp_err_t err = ESP_OK;
	TickType_t now;
	int idx;

	if (!wifi_cred_ssid_valid(ssid) || !wifi_cred_password_valid(password)) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!cred_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	{
		time_t unix_now = time(NULL);
		ts = (unix_now > 0) ? wifi_cred_normalize_ts((uint64_t)unix_now) : 0;
	}
	now = xTaskGetTickCount();

	cred_lock();
	if (cred.future_schema) {
		cred_unlock();
		return ESP_ERR_INVALID_STATE;
	}

	idx = wifi_cred_find_ssid(cred.table, cred.count, ssid);
	if (idx >= 0) {
		bool pass_changed = strncmp(cred.table[idx].password, password,
		                            BIRDIEBOT_WIFI_PASS_MAX + 1) != 0;
		bool idx_changed = cred.last_idx != (uint8_t)idx;
		bool flush_now;

		if (pass_changed) {
			wifi_cred_password_copy(cred.table[idx].password, password);
		}
		cred.table[idx].last_success_ts = ts;
		cred.last_idx = (uint8_t)idx;
		cred.no_retry_mask[idx] = 0;
		cred_volatile_remove_locked(ssid);
		if (!may_persist) {
			/* CONNECT(save=0) / auto_save=false: RAM only, never write the
			 * table (contract §7.5). */
			cred_unlock();
			return ESP_OK;
		}

		/* contract §3: new record / password change / last_idx change are
		 * written immediately, a ts-only update is debounced by 300 s.
		 * If an earlier write failed (dirty), flush the whole table so no
		 * pending change is lost. */
		flush_now = pass_changed || idx_changed ||
		            cred.last_flush_tick == 0 ||
		            (now - cred.last_flush_tick) >= pdMS_TO_TICKS(BIRDIEBOT_WIFI_CRED_TS_DEBOUNCE_MS);
		if (flush_now) {
			bool flush_all = cred.dirty;
			err = cred_flush_locked(flush_all, flush_all ? -1 : idx);
			if (err == ESP_OK) {
				cred.last_flush_tick = now;
				cred.dirty = false;
			} else {
				cred.dirty = true;
			}
		} else {
			cred.dirty = true;
		}
	} else if (may_persist) {
		bool updated = false;
		int evicted = -1;
		uint8_t new_idx = BIRDIEBOT_WIFI_CRED_NONE;

		err = cred_upsert_locked(ssid, password, ts, -1, false, true,
		                         &updated, &evicted, &new_idx);
		if (err == ESP_OK) {
			cred.last_flush_tick = now;
			cred.dirty = false;
			cred_volatile_remove_locked(ssid);
		} else {
			/* Keep the RAM table, retry on the next flush (§3). */
			cred.dirty = true;
		}
	} else {
		/* RAM-only credential: participates in rotation of this power cycle
		 * but never shows up in GET_SAVED_LIST / NVS (§3). */
		cred_volatile_upsert_locked(ssid, password, ts);
	}
	cred_unlock();
	return err;
}

int wifi_cred_mem_rotation_plan(wifi_cred_candidate_t *out, uint8_t max)
{
	uint8_t order[BIRDIEBOT_WIFI_CRED_MAX];
	int count;
	uint8_t written = 0;

	if (out == NULL || max == 0 || !cred_ready()) {
		return 0;
	}

	cred_lock();
	count = wifi_cred_rotate_order(cred.table, cred.count, cred.last_idx,
	                               cred.no_retry_mask, order, sizeof(order));
	for (int i = 0; i < count && written < max; ++i) {
		wifi_cred_candidate_t *cand = &out[written];
		memset(cand, 0, sizeof(*cand));
		cand->persistent = true;
		cand->idx = order[i];
		wifi_cred_ssid_copy(cand->ssid, cred.table[order[i]].ssid);
		wifi_cred_password_copy(cand->password, cred.table[order[i]].password);
		cand->last_success_ts = cred.table[order[i]].last_success_ts;
		cand->priority = cred.table[order[i]].priority;
		++written;
	}
	count = wifi_cred_rotate_order(cred.volatile_table, cred.volatile_count,
	                               cred.volatile_last_idx, cred.volatile_no_retry_mask,
	                               order, sizeof(order));
	for (int i = 0; i < count && written < max; ++i) {
		wifi_cred_candidate_t *cand = &out[written];
		memset(cand, 0, sizeof(*cand));
		cand->persistent = false;
		cand->idx = order[i];
		wifi_cred_ssid_copy(cand->ssid, cred.volatile_table[order[i]].ssid);
		wifi_cred_password_copy(cand->password, cred.volatile_table[order[i]].password);
		cand->last_success_ts = cred.volatile_table[order[i]].last_success_ts;
		cand->priority = cred.volatile_table[order[i]].priority;
		++written;
	}
	cred_unlock();
	return (int)written;
}

int wifi_cred_mem_rotate_order(uint8_t *order, uint8_t max)
{
	int count;

	if (order == NULL || max == 0 || !cred_ready()) {
		return 0;
	}
	cred_lock();
	count = wifi_cred_rotate_order(cred.table, cred.count, cred.last_idx,
	                               cred.no_retry_mask, order, max);
	cred_unlock();
	return count;
}

int wifi_cred_mem_set_ap_cred(const char *ssid, const char *password)
{
	char new_ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];
	bool restore_default;
	nvs_handle_t handle;
	esp_err_t err;

	if ((ssid != NULL && ssid[0] != '\0' && !wifi_cred_ssid_valid(ssid)) ||
	    !wifi_cred_ap_password_valid(password)) {
		return ESP_ERR_INVALID_ARG;
	}
	restore_default = (ssid == NULL || ssid[0] == '\0');
	memset(new_ssid, 0, sizeof(new_ssid));
	if (!restore_default) {
		wifi_cred_ssid_copy(new_ssid, ssid);
	}
	if (!cred_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	cred_lock();
	if (cred.future_schema) {
		cred_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	err = cred_nvs_open(&handle);
	if (err != ESP_OK) {
		cred_unlock();
		return err;
	}
	if (restore_default) {
		esp_err_t del = nvs_erase_key(handle, WIFI_CRED_KEY_AP_SSID);
		if (del != ESP_OK && del != ESP_ERR_NVS_NOT_FOUND) {
			err = del;
		}
	} else {
		err = nvs_set_str(handle, WIFI_CRED_KEY_AP_SSID, new_ssid);
	}
	if (err == ESP_OK) {
		err = nvs_set_str(handle, WIFI_CRED_KEY_AP_PASS, password);
	}
	if (err == ESP_OK) {
		err = nvs_commit(handle);
	}
	nvs_close(handle);
	if (err != ESP_OK) {
		cred_unlock();
		return err;
	}

	if (restore_default) {
		cred.ap_ssid[0] = '\0';
	} else {
		wifi_cred_ssid_copy(cred.ap_ssid, new_ssid);
	}
	wifi_cred_password_copy(cred.ap_pass, password);
	cred_unlock();
	return ESP_OK;
}

int wifi_cred_mem_get_ap_cred(char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1],
                              char password[BIRDIEBOT_WIFI_PASS_MAX + 1])
{
	if (ssid == NULL || password == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	cred_lock();
	if (cred.ap_ssid[0] != '\0') {
		wifi_cred_ssid_copy(ssid, cred.ap_ssid);
	} else if (!wifi_cred_mem_ap_ssid_default(ssid)) {
		/* Defensive: the MAC read cannot realistically fail on supported
		 * targets; keep a deterministic value instead of an empty AP SSID. */
		snprintf(ssid, BIRDIEBOT_WIFI_SSID_MAX + 1, "%s0000",
		         BIRDIEBOT_WIFI_AP_SSID_PREFIX);
	}
	if (cred.ap_pass[0] != '\0') {
		wifi_cred_password_copy(password, cred.ap_pass);
	} else {
		wifi_cred_password_copy(password, WIFI_CRED_DEFAULT_AP_PASS);
	}
	cred_unlock();
	return ESP_OK;
}

bool wifi_cred_mem_ap_ssid_default(char out[BIRDIEBOT_WIFI_SSID_MAX + 1])
{
	uint8_t mac[6] = {0};

	if (out == NULL) {
		return false;
	}
	if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
		return false;
	}
	return wifi_cred_make_default_ap_ssid(mac, out) == 0;
}

void wifi_cred_mem_set_connected_ssid(const char *ssid)
{
	if (!cred_ready()) {
		return;
	}
	cred_lock();
	if (ssid == NULL || ssid[0] == '\0') {
		cred.connected_ssid_set = false;
		memset(cred.connected_ssid, 0, sizeof(cred.connected_ssid));
	} else if (wifi_cred_ssid_copy(cred.connected_ssid, ssid) == 0) {
		cred.connected_ssid_set = true;
	}
	cred_unlock();
}

void wifi_cred_mem_note_connect_failure(const char *ssid, bool credential_failure)
{
	int idx;

	if (!ssid || !credential_failure || !cred_ready()) {
		return;
	}
	cred_lock();
	idx = wifi_cred_find_ssid(cred.table, cred.count, ssid);
	if (idx >= 0) {
		cred.no_retry_mask[idx] = 1;
	}
	idx = wifi_cred_find_ssid(cred.volatile_table, cred.volatile_count, ssid);
	if (idx >= 0) {
		cred.volatile_no_retry_mask[idx] = 1;
	}
	cred_unlock();
}

void wifi_cred_mem_clear_failures(void)
{
	if (!cred_ready()) {
		return;
	}
	cred_lock();
	memset(cred.no_retry_mask, 0, sizeof(cred.no_retry_mask));
	memset(cred.volatile_no_retry_mask, 0, sizeof(cred.volatile_no_retry_mask));
	cred_unlock();
}

int wifi_cred_mem_initial_cred(char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1],
                               char password[BIRDIEBOT_WIFI_PASS_MAX + 1])
{
	uint8_t order[BIRDIEBOT_WIFI_CRED_MAX];
	int count;

	if (ssid == NULL || password == NULL || !cred_ready()) {
		return ESP_ERR_INVALID_STATE;
	}
	cred_lock();
	count = wifi_cred_rotate_order(cred.table, cred.count, cred.last_idx, NULL,
	                               order, sizeof(order));
	if (count <= 0) {
		cred_unlock();
		return ESP_ERR_NOT_FOUND;
	}
	wifi_cred_ssid_copy(ssid, cred.table[order[0]].ssid);
	wifi_cred_password_copy(password, cred.table[order[0]].password);
	cred_unlock();
	return ESP_OK;
}

void wifi_cred_mem_pause_rotation(bool pause)
{
	if (!cred_ready()) {
		return;
	}
	cred_lock();
	cred.rotation_paused = pause;
	cred_unlock();
}

bool wifi_cred_mem_rotation_paused(void)
{
	bool paused;

	if (!cred_ready()) {
		return false;
	}
	cred_lock();
	paused = cred.rotation_paused;
	cred_unlock();
	return paused;
}
