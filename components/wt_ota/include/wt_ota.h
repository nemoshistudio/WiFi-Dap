/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wt_ota.h
 * @brief WiFi Dap OTA core (WP-205).
 *
 * Implements the firmware half of docs/ota-local-upload.md and the JSON
 * module 5 commands of docs/protocol.md §2.5:
 *
 *   GET_INFO(1)      running / running_ver / free / slot_size / status (+optional
 *                    idle / rollback / sha256 / last_error)
 *   GET_PROGRESS(3)  offset / total / status / err (+optional slot / sha256)
 *   ABORT(4)         client abort of a RECEIVING session (echo on success)
 *
 * The HTTP endpoint POST /api/ota/upload is implemented by WP-206 in
 * project_components/web_server/uri_modules/uri_ota.c; it uses the upload API
 * below (`wt_ota_upload_begin/write/finish/abort/fail`) plus
 * `wt_ota_apply_and_reboot()`. The JSON module is registered by this component
 * through API_JSON_MODULE_REGISTER, so no web_server changes are needed for
 * module 5 dispatch.
 *
 * State machine (docs/ota-local-upload.md §5):
 *
 *   IDLE -> RECEIVING -> VERIFYING -> READY -> APPLYING -> (reboot) IDLE
 *     ^         |            |          |
 *     |         +--abort-----+          +-- timeouts / verification errors
 *     +---------------- ERROR <-----------+
 *
 * Threading: every public call is serialized by an internal mutex, so the
 * upload API may be called from a dedicated OTA task while another task polls
 * GET_PROGRESS/GET_INFO. `esp_ota_begin()` erases the target slot and can block
 * for a while (hundreds of ms up to seconds; see the note in wt_ota_begin());
 * it is executed without holding the state lock so GET_INFO/GET_PROGRESS stay
 * responsive. The upload API is single-session by contract: the HTTP layer must
 * reject a second POST while a session is active.
 *
 * Rollback (docs/ota-local-upload.md §2.3):
 *    - CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (project default for all targets)
 *      makes the bootloader boot an OTA-updated image with
 *      ESP_OTA_IMG_PENDING_VERIFY. The application must confirm it with
 *      wt_ota_mark_app_valid() after basic services are up (contract: 10 s
 *      single-shot timer), otherwise the next reset rolls back to the previous
 *      slot. Optional Kconfig BIRDIEBOT_OTA_AUTO_MARK_VALID can do that from the
 *      component itself (default n; integration in main/ is the preferred hook).
 *    - With rollback disabled the same API is a safe no-op and
 *      GET_INFO.rollback reports false.
 */

#ifndef WT_OTA_H_GUARD
#define WT_OTA_H_GUARD

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** JSON module id of the OTA module (docs/protocol.md §2.5). */
#define WT_OTA_MODULE_ID 5

/** Largest image the 8MB dual-OTA partition table accepts (slot 0x300000). */
#define WT_OTA_SLOT_SIZE_MAX (3u * 1024u * 1024u)

/** docs/ota-local-upload.md §3.1: smaller uploads are rejected with HTTP 400. */
#define WT_OTA_MIN_IMAGE_SIZE 4096u

#define WT_OTA_LABEL_MAX_LEN 16
#define WT_OTA_VERSION_MAX_LEN 40
#define WT_OTA_ERR_MAX_LEN 96
#define WT_OTA_SHA256_HEX_LEN 64
#define WT_OTA_SHA256_STR_LEN (WT_OTA_SHA256_HEX_LEN + 1)

/** OTA session state (exact strings are part of the JSON contract). */
typedef enum {
	WT_OTA_STATUS_IDLE = 0,
	WT_OTA_STATUS_RECEIVING,
	WT_OTA_STATUS_VERIFYING,
	WT_OTA_STATUS_READY,
	WT_OTA_STATUS_APPLYING,
	WT_OTA_STATUS_ERROR,
} wt_ota_status_t;

/** Snapshot for OTA/GET_INFO (cmd 1) and boot diagnostics. */
typedef struct {
	char running[WT_OTA_LABEL_MAX_LEN];       //!< current slot: ota_0 / ota_1 ("" if none)
	char running_ver[WT_OTA_VERSION_MAX_LEN]; //!< running esp_app_desc.version
	char idle[WT_OTA_LABEL_MAX_LEN];          //!< next update target slot ("" if no OTA slots)
	uint32_t free_bytes;                      //!< target slot size in bytes ("free")
	uint32_t slot_size;                       //!< target slot size in bytes ("slot_size")
	wt_ota_status_t status;                   //!< session state
	bool rollback_enabled;                    //!< CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
	char sha256[WT_OTA_SHA256_STR_LEN];       //!< last/current image sha256 hex ("" if none)
	char last_error[WT_OTA_ERR_MAX_LEN];      //!< last error text (RAM only; "" if none)
} wt_ota_info_t;

/** Snapshot for OTA/GET_PROGRESS (cmd 3). */
typedef struct {
	char slot[WT_OTA_LABEL_MAX_LEN];          //!< target slot label ("" if none)
	uint32_t offset;                          //!< bytes written by the device so far
	uint32_t total;                           //!< total bytes of this upload (0 when idle)
	wt_ota_status_t status;                   //!< session state
	char err[WT_OTA_ERR_MAX_LEN];             //!< error text ("" unless status == ERROR)
	char sha256[WT_OTA_SHA256_STR_LEN];       //!< sha256 of the received image ("" if none)
} wt_ota_progress_t;

/* ------------------------------------------------------------------------- */
/* Lifecycle / queries                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Initialize the OTA core (mutex, logging, optional auto-confirm task).
 *
 * Called automatically once from app_main() through GLOBAL_MODULE_REGISTER;
 * safe to call again. Never reboots or writes flash.
 */
esp_err_t wt_ota_init(void);

/** Fill a consistent snapshot for GET_INFO. Never fails; zeroes on error. */
void wt_ota_get_info(wt_ota_info_t *info);

/** Fill a consistent snapshot for GET_PROGRESS. Never fails; zeroes on error. */
void wt_ota_get_progress(wt_ota_progress_t *progress);

/** Current session state (lock-protected snapshot). */
wt_ota_status_t wt_ota_get_status(void);

/** True while a session is RECEIVING/VERIFYING/READY/APPLYING (HTTP -> 409). */
bool wt_ota_is_busy(void);

/** Contract string for a state: "IDLE"/"RECEIVING"/.../"ERROR". */
const char *wt_ota_status_to_str(wt_ota_status_t status);

/**
 * @brief Enforce upload timeouts (idle 10 s / total 300 s by Kconfig defaults).
 *
 * GET_INFO / GET_PROGRESS already call this; the HTTP layer may also call it
 * from its OTA task. A timed-out RECEIVING session is aborted and moves to
 * ERROR ("upload timeout") exactly like docs/ota-local-upload.md §3.3.
 */
void wt_ota_poll(void);

/* ------------------------------------------------------------------------- */
/* Upload API (used by uri_ota.c / WP-206)                                   */
/* ------------------------------------------------------------------------- */
/*
 * Error -> HTTP/JSON mapping used by the HTTP layer (contract §3.4); the
 * helpers wt_ota_err_to_http_status() / wt_ota_err_to_json_code() below return
 * these values so uri_ota.c does not have to duplicate the table:
 *
 *   ESP_ERR_INVALID_STATE                -> 409 "ota session busy" / code 6
 *   ESP_ERR_INVALID_ARG                  -> 400 "invalid content length" / code 5
 *   ESP_ERR_INVALID_SIZE                 -> 413 "image too large"    / code 5
 *   ESP_ERR_NOT_SUPPORTED                -> 503 "ota service unavailable" / code 6
 *   ESP_ERR_TIMEOUT                      -> 408 "upload timeout"     / code 6
 *   ESP_ERR_OTA_VALIDATE_FAILED          -> 422 "image verification failed" / code 5
 *   ESP_ERR_INVALID_CRC                  -> 422 "sha256 mismatch"    / code 5
 *   ESP_ERR_OTA_ROLLBACK_INVALID_STATE   -> 409 "confirm running image first" / code 6
 *   ESP_ERR_NO_MEM                       -> 503 / code 6
 *   other failures                       -> 500 / code 3
 */

/**
 * @brief Start an upload session into the inactive OTA slot.
 *
 * Validates the declared size (4096 <= content_length <= target slot size),
 * rejects a busy session (ESP_ERR_INVALID_STATE -> HTTP 409) and rejects boards
 * without OTA partitions (single-app 4MB table: ESP_ERR_NOT_SUPPORTED -> 503).
 * Passing the known Content-Length lets esp_ota_begin() erase only the needed
 * range. Blocks during the initial flash erase; call from the OTA task.
 */
esp_err_t wt_ota_upload_begin(uint32_t content_length);

/**
 * @brief Append one received chunk (any size, typically 4096 B from uri_ota).
 *
 * Updates the running SHA-256 and GET_PROGRESS.offset. Returns
 * ESP_ERR_INVALID_STATE when no RECEIVING session exists and
 * ESP_ERR_INVALID_SIZE when the device receives more than Content-Length
 * (the session then moves to ERROR and the slot is aborted).
 */
esp_err_t wt_ota_upload_write(const void *data, size_t len);

/**
 * @brief Finish the session: esp_ota_end() validation + optional SHA-256 check
 *        + esp_ota_set_boot_partition() -> READY.
 *
 * @param expected_sha256_hex optional X-SHA256 header value (64 hex chars,
 *                            case-insensitive); NULL/"" skips the comparison.
 *                            A mismatch moves to ERROR("sha256 mismatch") and
 *                            does NOT switch the boot slot.
 */
esp_err_t wt_ota_upload_finish(const char *expected_sha256_hex);

/**
 * @brief Client-requested abort (WS ABORT=4 / xhr.abort()).
 *
 * Only valid while RECEIVING: esp_ota_abort() and back to IDLE (echo response).
 * Returns ESP_ERR_INVALID_STATE (JSON code 5) in every other state.
 * If a begin/erase is still in flight the abort is latched and applied by
 * wt_ota_upload_begin() before it returns.
 */
esp_err_t wt_ota_upload_abort(void);

/**
 * @brief Report a failed session (HTTP timeout / client disconnect / internal).
 *
 * Only valid while RECEIVING: aborts the slot and moves to ERROR(reason),
 * keeping offset/total for diagnosis (contract §3.3/§5). Does not reboot.
 */
esp_err_t wt_ota_upload_fail(const char *reason);

/**
 * @brief READY -> APPLYING and esp_restart() after @p delay_ms.
 *
 * uri_ota.c must send the HTTP 200 response first (contract §3.4: reboot after
 * 1000 ms so the browser receives the JSON result).
 */
esp_err_t wt_ota_apply_and_reboot(uint32_t delay_ms);

/* ------------------------------------------------------------------------- */
/* Rollback confirmation                                                     */
/* ------------------------------------------------------------------------- */

/** True when the build has CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y. */
bool wt_ota_rollback_enabled(void);

/**
 * @brief Confirm the running image after a successful OTA boot.
 *
 * Integration (main/app_main, WP-209): after WiFi + webserver + DAP/UART are up,
 * start a single 10 s timer and call this function (contract §2.3). It only acts
 * when the running image is in ESP_OTA_IMG_PENDING_VERIFY; otherwise it logs and
 * returns ESP_OK. No-op (ESP_OK) when rollback support is disabled.
 */
esp_err_t wt_ota_mark_app_valid(void);

/**
 * @brief Explicitly roll back a freshly booted image and reboot (fatal init).
 *
 * Calls esp_ota_mark_app_invalid_rollback_and_reboot(); on success it reboots
 * and does not return. No-op (ESP_ERR_NOT_SUPPORTED) when rollback is disabled.
 */
esp_err_t wt_ota_mark_app_invalid(void);

/* ------------------------------------------------------------------------- */
/* Error helpers (contract mapping tables)                                   */
/* ------------------------------------------------------------------------- */

/** Human/HTTP error text for a failed OTA call. never NULL. */
const char *wt_ota_err_to_str(esp_err_t err);

/** HTTP status code mapping for uri_ota.c (400/408/409/413/422/500/503). */
int wt_ota_err_to_http_status(esp_err_t err);

/** docs/protocol.md §1.1 JSON code mapping (3/5/6). */
int wt_ota_err_to_json_code(esp_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* WT_OTA_H_GUARD */
