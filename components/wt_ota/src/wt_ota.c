/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wt_ota.c
 * @brief OTA core implementation (WP-205): session state machine, flash
 *        begin/write/end/set_boot wrappers, SHA-256 accounting, rollback.
 *
 * Contract: docs/ota-local-upload.md (§2 rollback, §3 upload session, §5 state
 * machine, §6 write/verify flow) and docs/protocol.md §2.5 (module 5).
 */

#include "wt_ota.h"

#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

#include <stdio.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include "global_module.h"

#define TAG "WT_OTA"

/* Kconfig defaults (components/wt_ota/Kconfig); the fallbacks keep the file
 * self-contained for review/host tooling. */
#ifndef CONFIG_BIRDIEBOT_OTA_IDLE_TIMEOUT_MS
#define CONFIG_BIRDIEBOT_OTA_IDLE_TIMEOUT_MS 10000
#endif
#ifndef CONFIG_BIRDIEBOT_OTA_MAX_DURATION_MS
#define CONFIG_BIRDIEBOT_OTA_MAX_DURATION_MS 300000
#endif
#ifndef CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID_DELAY_MS
#define CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID_DELAY_MS 10000
#endif

/* esp_ota_begin() image_size hint: "known length" erases at most this range.
 * The 8MB table slots are 3 MiB (wt_ota.h WT_OTA_SLOT_SIZE_MAX). */
#define WT_OTA_REBOOT_TASK_STACK 3072
#define WT_OTA_AUTO_MARK_TASK_STACK 3072

typedef struct {
	bool inited;
	SemaphoreHandle_t lock;

	wt_ota_status_t status;

	/* A begin() is performing the initial flash erase outside the lock. */
	bool beginning;
	/* Client abort latched while beginning == true. */
	bool abort_requested;
	bool reboot_scheduled;

	esp_ota_handle_t handle;           /* valid while RECEIVING */
	const esp_partition_t *target;     /* inactive slot for the session */
	uint32_t total;                    /* declared Content-Length */
	uint32_t offset;                   /* bytes written so far */
	TickType_t started;
	TickType_t last_write;

	mbedtls_sha256_context sha;
	bool sha_active;
	char sha_hex[WT_OTA_SHA256_STR_LEN];      /* current session digest */
	char last_sha_hex[WT_OTA_SHA256_STR_LEN]; /* last completed digest */
	char err[WT_OTA_ERR_MAX_LEN];
} wt_ota_ctx_t;

static wt_ota_ctx_t s_ota;

static uint32_t s_reboot_delay_ms;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void ota_lock(void)
{
	if (s_ota.lock) {
		xSemaphoreTake(s_ota.lock, portMAX_DELAY);
	}
}

static void ota_unlock(void)
{
	if (s_ota.lock) {
		xSemaphoreGive(s_ota.lock);
	}
}

static bool status_is_active(wt_ota_status_t status)
{
	return status == WT_OTA_STATUS_RECEIVING ||
	       status == WT_OTA_STATUS_VERIFYING ||
	       status == WT_OTA_STATUS_READY ||
	       status == WT_OTA_STATUS_APPLYING;
}

static void copy_label(char *dst, size_t dst_len, const esp_partition_t *part)
{
	if (dst_len == 0) {
		return;
	}
	if (part && part->label[0]) {
		/* esp_partition_t.label is ESP_PARTITION_LABEL_SIZE (17) bytes while the
		 * JSON label buffers are 16; copy bounded without snprintf so
		 * -Werror=format-truncation stays happy. */
		size_t n = strnlen(part->label, dst_len - 1);
		memcpy(dst, part->label, n);
		dst[n] = '\0';
	} else {
		dst[0] = '\0';
	}
}

static void reset_session_locked(void)
{
	s_ota.beginning = false;
	s_ota.abort_requested = false;
	s_ota.handle = 0;
	s_ota.target = NULL;
	s_ota.total = 0;
	s_ota.offset = 0;
	s_ota.sha_active = false;
	s_ota.sha_hex[0] = '\0';
	s_ota.err[0] = '\0';
}

static void set_error_locked(esp_err_t err, const char *msg)
{
	s_ota.status = WT_OTA_STATUS_ERROR;
	snprintf(s_ota.err, sizeof(s_ota.err), "%s", msg ? msg : wt_ota_err_to_str(err));
	ESP_LOGE(TAG, "session error: %s (%s)", s_ota.err, esp_err_to_name(err));
}

static void sha256_hex(const uint8_t digest[32], char out[WT_OTA_SHA256_STR_LEN])
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 32; ++i) {
		out[i * 2] = hex[(digest[i] >> 4) & 0x0F];
		out[i * 2 + 1] = hex[digest[i] & 0x0F];
	}
	out[WT_OTA_SHA256_HEX_LEN] = '\0';
}

static bool sha256_hex_valid(const char *hex)
{
	if (!hex || strlen(hex) != WT_OTA_SHA256_HEX_LEN) {
		return false;
	}
	for (int i = 0; i < WT_OTA_SHA256_HEX_LEN; ++i) {
		char c = hex[i];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!ok) {
			return false;
		}
	}
	return true;
}

static bool sha256_hex_equal(const char *a, const char *b)
{
	if (!sha256_hex_valid(a) || !sha256_hex_valid(b)) {
		return false;
	}
	for (int i = 0; i < WT_OTA_SHA256_HEX_LEN; ++i) {
		char ca = a[i];
		char cb = b[i];
		if (ca >= 'A' && ca <= 'F') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'F') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return false;
		}
	}
	return true;
}

static uint32_t elapsed_ms(TickType_t now, TickType_t since)
{
	return (uint32_t)((now - since) * portTICK_PERIOD_MS);
}

/* Enforce the upload timeouts; caller must hold the lock. */
static void check_timeouts_locked(void)
{
	if (s_ota.status != WT_OTA_STATUS_RECEIVING || s_ota.beginning) {
		return;
	}

	TickType_t now = xTaskGetTickCount();
	uint32_t idle = elapsed_ms(now, s_ota.last_write);
	uint32_t total = elapsed_ms(now, s_ota.started);

	if (idle <= (uint32_t)CONFIG_BIRDIEBOT_OTA_IDLE_TIMEOUT_MS &&
	    total <= (uint32_t)CONFIG_BIRDIEBOT_OTA_MAX_DURATION_MS) {
		return;
	}

	ESP_LOGW(TAG, "upload timeout (idle %u ms, total %u ms) -> abort",
	         (unsigned)idle, (unsigned)total);
	if (s_ota.handle) {
		esp_ota_abort(s_ota.handle);
		s_ota.handle = 0;
	}
	/* ERROR keeps offset/total for diagnosis (contract §3.3). */
	set_error_locked(ESP_ERR_TIMEOUT, "upload timeout");
}

static void reboot_task(void *arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(s_reboot_delay_ms));
	ESP_LOGW(TAG, "rebooting to apply the OTA update");
	esp_restart();
}

#if defined(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID)
static void auto_mark_task(void *arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID_DELAY_MS));
	ESP_LOGI(TAG, "auto-confirming the running image after boot");
	wt_ota_mark_app_valid();
}
#endif

/* ------------------------------------------------------------------------- */
/* lifecycle / queries                                                       */
/* ------------------------------------------------------------------------- */

esp_err_t wt_ota_init(void)
{
	if (s_ota.inited) {
		return ESP_OK;
	}

	s_ota.lock = xSemaphoreCreateMutex();
	if (!s_ota.lock) {
		ESP_LOGE(TAG, "cannot create OTA mutex");
		return ESP_ERR_NO_MEM;
	}

	s_ota.status = WT_OTA_STATUS_IDLE;
	s_ota.inited = true;

	const esp_partition_t *running = esp_ota_get_running_partition();
	const esp_app_desc_t *desc = esp_app_get_description();
	const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

	ESP_LOGI(TAG, "OTA core init: running=%s ver=%s next=%s rollback=%s",
	         running ? running->label : "?",
	         desc ? desc->version : "?",
	         target ? target->label : "none",
	         wt_ota_rollback_enabled() ? "on" : "off");
	if (!target) {
		ESP_LOGW(TAG, "no inactive OTA slot on this partition table (4MB single-app build?); "
		              "module 5 reports free/slot_size=0 and POST upload is unsupported");
	}

#if defined(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID)
	if (xTaskCreate(auto_mark_task, "ota_auto_mark", WT_OTA_AUTO_MARK_TASK_STACK,
	                NULL, 3, NULL) != pdPASS) {
		ESP_LOGW(TAG, "cannot create auto mark-valid task");
	}
#endif

	return ESP_OK;
}

static void ensure_init(void)
{
	if (!s_ota.inited) {
		wt_ota_init();
	}
}

void wt_ota_get_info(wt_ota_info_t *info)
{
	if (!info) {
		return;
	}
	memset(info, 0, sizeof(*info));
	ensure_init();

	ota_lock();
	check_timeouts_locked();

	const esp_partition_t *running = esp_ota_get_running_partition();
	const esp_app_desc_t *desc = esp_app_get_description();
	const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

	copy_label(info->running, sizeof(info->running), running);
	snprintf(info->running_ver, sizeof(info->running_ver), "%s", desc ? desc->version : "");
	copy_label(info->idle, sizeof(info->idle), target);
	info->free_bytes = target ? (uint32_t)target->size : 0;
	info->slot_size = target ? (uint32_t)target->size : 0;
	info->status = s_ota.status;
	info->rollback_enabled = wt_ota_rollback_enabled();
	snprintf(info->sha256, sizeof(info->sha256), "%s",
	         s_ota.sha_hex[0] ? s_ota.sha_hex : s_ota.last_sha_hex);
	snprintf(info->last_error, sizeof(info->last_error), "%s", s_ota.err);

	ota_unlock();
}

void wt_ota_get_progress(wt_ota_progress_t *progress)
{
	if (!progress) {
		return;
	}
	memset(progress, 0, sizeof(*progress));
	ensure_init();

	ota_lock();
	check_timeouts_locked();

	copy_label(progress->slot, sizeof(progress->slot), s_ota.target);
	progress->offset = s_ota.offset;
	progress->total = s_ota.total;
	progress->status = s_ota.status;
	snprintf(progress->err, sizeof(progress->err), "%s", s_ota.err);
	snprintf(progress->sha256, sizeof(progress->sha256), "%s",
	         s_ota.sha_hex[0] ? s_ota.sha_hex : s_ota.last_sha_hex);

	ota_unlock();
}

wt_ota_status_t wt_ota_get_status(void)
{
	ensure_init();
	ota_lock();
	check_timeouts_locked();
	wt_ota_status_t status = s_ota.status;
	ota_unlock();
	return status;
}

bool wt_ota_is_busy(void)
{
	return status_is_active(wt_ota_get_status());
}

const char *wt_ota_status_to_str(wt_ota_status_t status)
{
	switch (status) {
	case WT_OTA_STATUS_IDLE: return "IDLE";
	case WT_OTA_STATUS_RECEIVING: return "RECEIVING";
	case WT_OTA_STATUS_VERIFYING: return "VERIFYING";
	case WT_OTA_STATUS_READY: return "READY";
	case WT_OTA_STATUS_APPLYING: return "APPLYING";
	case WT_OTA_STATUS_ERROR: return "ERROR";
	default: return "ERROR";
	}
}

void wt_ota_poll(void)
{
	ensure_init();
	ota_lock();
	check_timeouts_locked();
	ota_unlock();
}

/* ------------------------------------------------------------------------- */
/* upload API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t wt_ota_upload_begin(uint32_t content_length)
{
	ensure_init();

	ota_lock();
	check_timeouts_locked();

	if (status_is_active(s_ota.status) || s_ota.beginning) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_begin rejected: session already active (status %s)",
		         wt_ota_status_to_str(s_ota.status));
		return ESP_ERR_INVALID_STATE;
	}
	if (content_length < WT_OTA_MIN_IMAGE_SIZE) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_begin rejected: content_length %u < %u",
		         (unsigned)content_length, (unsigned)WT_OTA_MIN_IMAGE_SIZE);
		return ESP_ERR_INVALID_ARG;
	}

	const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
	if (!target) {
		ota_unlock();
		ESP_LOGE(TAG, "upload_begin rejected: no inactive OTA partition");
		return ESP_ERR_NOT_SUPPORTED;
	}
	if (content_length > (uint32_t)target->size) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_begin rejected: %u > slot size %u",
		         (unsigned)content_length, (unsigned)target->size);
		return ESP_ERR_INVALID_SIZE;
	}

	reset_session_locked();
	s_ota.status = WT_OTA_STATUS_RECEIVING;
	s_ota.beginning = true;
	s_ota.target = target;
	s_ota.total = content_length;
	s_ota.offset = 0;
	s_ota.started = xTaskGetTickCount();
	s_ota.last_write = s_ota.started;
	ota_unlock();

	ESP_LOGI(TAG, "upload start: target=%s size=%u", target->label, (unsigned)content_length);

	/* Flash erase happens outside the state lock so GET_INFO/GET_PROGRESS stay
	 * responsive; start snapshot (RECEIVING, offset 0) is already published. */
	esp_ota_handle_t handle = 0;
	esp_err_t err = esp_ota_begin(target, content_length, &handle);

	ota_lock();
	s_ota.beginning = false;
	bool aborted = s_ota.abort_requested;
	s_ota.abort_requested = false;

	if (aborted) {
		if (err == ESP_OK && handle) {
			esp_ota_abort(handle);
		}
		reset_session_locked();
		s_ota.status = WT_OTA_STATUS_IDLE;
		ota_unlock();
		ESP_LOGW(TAG, "upload start aborted by client request");
		return ESP_ERR_INVALID_STATE;
	}

	if (err != ESP_OK) {
		s_ota.handle = 0;
		if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
			set_error_locked(err, "running image not confirmed (boot pending verify)");
		} else if (err == ESP_ERR_NOT_SUPPORTED) {
			set_error_locked(err, "ota not supported on this partition table");
		} else {
			set_error_locked(err, "ota begin failed");
		}
		ota_unlock();
		return err;
	}

	s_ota.handle = handle;
	mbedtls_sha256_init(&s_ota.sha);
	/* mbedtls requires an explicit starts() call after init(); without it the
	 * digest is computed over an uninitialised state (hardware-verified bug). */
	mbedtls_sha256_starts(&s_ota.sha, 0);
	s_ota.sha_active = true;
	s_ota.sha_hex[0] = '\0';
	ota_unlock();

	return ESP_OK;
}

esp_err_t wt_ota_upload_write(const void *data, size_t len)
{
	if (!data || len == 0) {
		return ESP_ERR_INVALID_ARG;
	}
	ensure_init();

	ota_lock();

	if (s_ota.status != WT_OTA_STATUS_RECEIVING || s_ota.beginning || s_ota.handle == 0) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_write rejected: no RECEIVING session (status %s)",
		         wt_ota_status_to_str(s_ota.status));
		return ESP_ERR_INVALID_STATE;
	}
	if ((uint64_t)s_ota.offset + len > (uint64_t)s_ota.total) {
		if (s_ota.handle) {
			esp_ota_abort(s_ota.handle);
			s_ota.handle = 0;
		}
		set_error_locked(ESP_ERR_INVALID_SIZE, "content length exceeded");
		ota_unlock();
		return ESP_ERR_INVALID_SIZE;
	}

	if (s_ota.sha_active) {
		mbedtls_sha256_update(&s_ota.sha, (const unsigned char *)data, len);
	}

	esp_err_t err = esp_ota_write(s_ota.handle, data, len);
	if (err != ESP_OK) {
		esp_ota_abort(s_ota.handle);
		s_ota.handle = 0;
		set_error_locked(err, "ota write failed");
		ota_unlock();
		return err;
	}

	s_ota.offset += (uint32_t)len;
	s_ota.last_write = xTaskGetTickCount();
	ota_unlock();

	return ESP_OK;
}

esp_err_t wt_ota_upload_finish(const char *expected_sha256_hex)
{
	ensure_init();

	ota_lock();

	if (s_ota.status != WT_OTA_STATUS_RECEIVING || s_ota.beginning || s_ota.handle == 0) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_finish rejected: no RECEIVING session (status %s)",
		         wt_ota_status_to_str(s_ota.status));
		return ESP_ERR_INVALID_STATE;
	}

	const char *expected = (expected_sha256_hex && expected_sha256_hex[0]) ? expected_sha256_hex : NULL;
	if (expected && !sha256_hex_valid(expected)) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_finish rejected: malformed X-SHA256 header");
		return ESP_ERR_INVALID_ARG;
	}

	s_ota.status = WT_OTA_STATUS_VERIFYING;
	esp_ota_handle_t handle = s_ota.handle;
	s_ota.handle = 0;

	/* esp_ota_end() consumes the handle even on failure (contract §6.1) */
	esp_err_t err = esp_ota_end(handle);

	/* Finish the SHA-256 regardless, so GET_PROGRESS/GET_INFO and the failed
	 * upload response can show the digest of what the device actually received. */
	uint8_t digest[32] = {0};
	if (s_ota.sha_active) {
		mbedtls_sha256_finish(&s_ota.sha, digest);
		s_ota.sha_active = false;
		sha256_hex(digest, s_ota.sha_hex);
		snprintf(s_ota.last_sha_hex, sizeof(s_ota.last_sha_hex), "%s", s_ota.sha_hex);
	}

	if (err != ESP_OK) {
		set_error_locked(err, "image verification failed");
		ota_unlock();
		return err;
	}

	if (expected && !sha256_hex_equal(expected, s_ota.sha_hex)) {
		/* Written slot is left as-is; boot slot is NOT switched (contract §6.3). */
		ESP_LOGE(TAG, "sha256 mismatch: expected=%s computed=%s", expected, s_ota.sha_hex);
		set_error_locked(ESP_ERR_INVALID_CRC, "sha256 mismatch");
		ota_unlock();
		return ESP_ERR_INVALID_CRC;
	}

	err = esp_ota_set_boot_partition(s_ota.target);
	if (err != ESP_OK) {
		set_error_locked(err, "set boot partition failed");
		ota_unlock();
		return err;
	}

	s_ota.status = WT_OTA_STATUS_READY;
	ESP_LOGI(TAG, "image verified (%s), boot slot switched to %s",
	         s_ota.sha_hex, s_ota.target ? s_ota.target->label : "?");
	ota_unlock();
	return ESP_OK;
}

esp_err_t wt_ota_upload_abort(void)
{
	ensure_init();

	ota_lock();

	if (s_ota.beginning) {
		/* The erase/flash-begin is in flight; latch and let begin() finish it. */
		s_ota.abort_requested = true;
		ota_unlock();
		ESP_LOGW(TAG, "upload abort latched while begin() is in progress");
		return ESP_OK;
	}

	if (s_ota.status != WT_OTA_STATUS_RECEIVING) {
		ota_unlock();
		return ESP_ERR_INVALID_STATE;
	}

	if (s_ota.handle) {
		esp_ota_abort(s_ota.handle);
		s_ota.handle = 0;
	}
	reset_session_locked();
	s_ota.status = WT_OTA_STATUS_IDLE;
	ota_unlock();

	ESP_LOGW(TAG, "upload aborted by client -> IDLE");
	return ESP_OK;
}

esp_err_t wt_ota_upload_fail(const char *reason)
{
	ensure_init();

	ota_lock();

	if (s_ota.status != WT_OTA_STATUS_RECEIVING) {
		ota_unlock();
		ESP_LOGW(TAG, "upload_fail ignored: not RECEIVING (status %s)",
		         wt_ota_status_to_str(s_ota.status));
		return ESP_ERR_INVALID_STATE;
	}

	if (s_ota.handle) {
		esp_ota_abort(s_ota.handle);
		s_ota.handle = 0;
	}
	s_ota.sha_active = false;
	set_error_locked(ESP_FAIL, reason ? reason : "upload failed");
	ota_unlock();

	return ESP_OK;
}

esp_err_t wt_ota_apply_and_reboot(uint32_t delay_ms)
{
	ensure_init();

	ota_lock();
	if (s_ota.status != WT_OTA_STATUS_READY || s_ota.reboot_scheduled) {
		ota_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	s_ota.status = WT_OTA_STATUS_APPLYING;
	s_ota.reboot_scheduled = true;
	s_reboot_delay_ms = delay_ms;
	ota_unlock();

	ESP_LOGW(TAG, "OTA update ready: rebooting in %u ms", (unsigned)delay_ms);
	if (xTaskCreate(reboot_task, "ota_reboot", WT_OTA_REBOOT_TASK_STACK, NULL, 5, NULL) != pdPASS) {
		ESP_LOGE(TAG, "cannot create reboot task; rebooting now");
		esp_restart();
	}
	return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* rollback                                                                  */
/* ------------------------------------------------------------------------- */

bool wt_ota_rollback_enabled(void)
{
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
	return true;
#else
	return false;
#endif
}

esp_err_t wt_ota_mark_app_valid(void)
{
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
	const esp_partition_t *running = esp_ota_get_running_partition();
	if (!running) {
		ESP_LOGE(TAG, "mark_app_valid: no running partition");
		return ESP_ERR_NOT_FOUND;
	}

	esp_ota_img_states_t state;
	esp_err_t err = esp_ota_get_state_partition(running, &state);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "mark_app_valid: cannot read OTA state: %s", esp_err_to_name(err));
		return err;
	}

	if (state != ESP_OTA_IMG_PENDING_VERIFY) {
		ESP_LOGI(TAG, "mark_app_valid: running image state=%d, nothing to confirm", (int)state);
		return ESP_OK;
	}

	err = esp_ota_mark_app_valid_cancel_rollback();
	if (err == ESP_OK) {
		ESP_LOGI(TAG, "running image confirmed (rollback cancelled)");
	} else {
		ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(err));
	}
	return err;
#else
	ESP_LOGI(TAG, "mark_app_valid: rollback support disabled, no-op");
	return ESP_OK;
#endif
}

esp_err_t wt_ota_mark_app_invalid(void)
{
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
	ESP_LOGW(TAG, "mark_app_invalid: rolling back to the previous slot and rebooting");
	return esp_ota_mark_app_invalid_rollback_and_reboot();
#else
	ESP_LOGW(TAG, "mark_app_invalid: rollback support disabled, no-op");
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ------------------------------------------------------------------------- */
/* error mapping                                                             */
/* ------------------------------------------------------------------------- */

const char *wt_ota_err_to_str(esp_err_t err)
{
	switch (err) {
	case ESP_OK: return "ok";
	case ESP_ERR_INVALID_STATE: return "ota session busy or invalid state";
	case ESP_ERR_INVALID_ARG: return "invalid content length";
	case ESP_ERR_INVALID_SIZE: return "image too large";
	case ESP_ERR_NOT_SUPPORTED: return "ota service unavailable";
	case ESP_ERR_TIMEOUT: return "upload timeout";
	case ESP_ERR_OTA_VALIDATE_FAILED: return "image verification failed";
	case ESP_ERR_INVALID_CRC: return "sha256 mismatch";
	case ESP_ERR_OTA_ROLLBACK_INVALID_STATE: return "running image not confirmed";
	case ESP_ERR_NO_MEM: return "out of memory";
	default: return "ota operation failed";
	}
}

int wt_ota_err_to_http_status(esp_err_t err)
{
	switch (err) {
	case ESP_ERR_INVALID_STATE: return 409;
	case ESP_ERR_OTA_ROLLBACK_INVALID_STATE: return 409;
	case ESP_ERR_INVALID_ARG: return 400;
	case ESP_ERR_INVALID_SIZE: return 413;
	case ESP_ERR_NOT_SUPPORTED: return 503;
	case ESP_ERR_NO_MEM: return 503;
	case ESP_ERR_TIMEOUT: return 408;
	case ESP_ERR_OTA_VALIDATE_FAILED: return 422;
	case ESP_ERR_INVALID_CRC: return 422;
	default: return 500;
	}
}

int wt_ota_err_to_json_code(esp_err_t err)
{
	switch (err) {
	case ESP_ERR_INVALID_ARG:
	case ESP_ERR_INVALID_SIZE:
	case ESP_ERR_OTA_VALIDATE_FAILED:
	case ESP_ERR_INVALID_CRC:
		return 5; /* API_JSON_PROPERTY_ERR */
	case ESP_ERR_INVALID_STATE:
	case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
	case ESP_ERR_NOT_SUPPORTED:
	case ESP_ERR_NO_MEM:
	case ESP_ERR_TIMEOUT:
		return 6; /* API_JSON_BUSY */
	default:
		return 3; /* API_JSON_INTERNAL_ERR */
	}
}

/* ------------------------------------------------------------------------- */
/* module registration                                                       */
/* ------------------------------------------------------------------------- */

static int wt_ota_global_module_init(void)
{
	return wt_ota_init() == ESP_OK ? 0 : 1;
}

static const global_module_t s_wt_ota_module = {
	.init = wt_ota_global_module_init,
	.module_id = WT_OTA_MODULE_ID,
};

GLOBAL_MODULE_REGISTER(WT_OTA, &s_wt_ota_module);
