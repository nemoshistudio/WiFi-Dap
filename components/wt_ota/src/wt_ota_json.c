/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wt_ota_json.c
 * @brief JSON module 5 self-registration: OTA/GET_INFO(1), GET_PROGRESS(3),
 *        ABORT(4) - docs/protocol.md §2.5, docs/ota-local-upload.md §4.
 *
 * Registered through API_JSON_MODULE_REGISTER so the WS/API routers dispatch
 * {"module":5,...} to this module without touching web_server code. The
 * component sets WHOLE_ARCHIVE so the constructor runs and the module links.
 */

#include "wt_ota.h"

#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

#include <esp_err.h>
#include <esp_log.h>

#include <cJSON.h>

#include "api_json_module.h"

#define TAG "WT_OTA_JSON"

#if defined(CONFIG_BIRDIEBOT_OTA_ENABLE)

typedef enum {
	WT_OTA_CMD_GET_INFO = 1,
	WT_OTA_CMD_UPLOAD_RESPONSE = 2, /* HTTP response association only; WS -> code 4 */
	WT_OTA_CMD_GET_PROGRESS = 3,
	WT_OTA_CMD_ABORT = 4,
} wt_ota_cmd_t;

static cJSON *ota_json_info(void)
{
	wt_ota_info_t info;
	wt_ota_get_info(&info);

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return NULL;
	}

	cJSON_AddNumberToObject(root, "module", WT_OTA_MODULE_ID);
	cJSON_AddNumberToObject(root, "cmd", WT_OTA_CMD_GET_INFO);
	cJSON_AddStringToObject(root, "running", info.running);
	cJSON_AddStringToObject(root, "running_ver", info.running_ver);
	cJSON_AddNumberToObject(root, "free", (double)info.free_bytes);
	cJSON_AddNumberToObject(root, "slot_size", (double)info.slot_size);
	cJSON_AddStringToObject(root, "status", wt_ota_status_to_str(info.status));

	/* Optional increment fields allowed by docs/ota-local-upload.md §4.1. */
	if (info.idle[0]) {
		cJSON_AddStringToObject(root, "idle", info.idle);
	}
	cJSON_AddBoolToObject(root, "rollback", info.rollback_enabled);
	if (info.sha256[0]) {
		cJSON_AddStringToObject(root, "sha256", info.sha256);
	}
	if (info.last_error[0]) {
		cJSON_AddStringToObject(root, "last_error", info.last_error);
	}
	return root;
}

static cJSON *ota_json_progress(void)
{
	wt_ota_progress_t progress;
	wt_ota_get_progress(&progress);

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return NULL;
	}

	cJSON_AddNumberToObject(root, "module", WT_OTA_MODULE_ID);
	cJSON_AddNumberToObject(root, "cmd", WT_OTA_CMD_GET_PROGRESS);
	cJSON_AddNumberToObject(root, "offset", (double)progress.offset);
	cJSON_AddNumberToObject(root, "total", (double)progress.total);
	cJSON_AddStringToObject(root, "status", wt_ota_status_to_str(progress.status));
	cJSON_AddStringToObject(root, "err", progress.err);

	/* Optional increment fields (docs/ota-local-upload.md §4.2). */
	if (progress.slot[0]) {
		cJSON_AddStringToObject(root, "slot", progress.slot);
	}
	if (progress.sha256[0]) {
		cJSON_AddStringToObject(root, "sha256", progress.sha256);
	}
	return root;
}

static cJSON *ota_json_abort_echo(void)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return NULL;
	}
	cJSON_AddNumberToObject(root, "module", WT_OTA_MODULE_ID);
	cJSON_AddNumberToObject(root, "cmd", WT_OTA_CMD_ABORT);
	return root;
}

static int ota_api_json_abort(api_json_req_t *req)
{
	esp_err_t err = wt_ota_upload_abort();
	if (err == ESP_OK) {
		req->out = ota_json_abort_echo();
		return req->out ? API_JSON_OK : API_JSON_INTERNAL_ERR;
	}
	if (err == ESP_ERR_INVALID_STATE) {
		/* Contract §4.3: ABORT outside RECEIVING -> code 5 (invalid state). */
		ESP_LOGW(TAG, "ABORT ignored: no RECEIVING session");
		return API_JSON_PROPERTY_ERR;
	}
	ESP_LOGE(TAG, "ABORT failed: %s", wt_ota_err_to_str(err));
	return API_JSON_INTERNAL_ERR;
}

static int on_json_req(uint16_t cmd, api_json_req_t *req, api_json_module_async_t *async)
{
	(void)async;

	switch ((wt_ota_cmd_t)cmd) {
	case WT_OTA_CMD_GET_INFO:
		req->out = ota_json_info();
		return req->out ? API_JSON_OK : API_JSON_INTERNAL_ERR;
	case WT_OTA_CMD_GET_PROGRESS:
		req->out = ota_json_progress();
		return req->out ? API_JSON_OK : API_JSON_INTERNAL_ERR;
	case WT_OTA_CMD_ABORT:
		return ota_api_json_abort(req);
	case WT_OTA_CMD_UPLOAD_RESPONSE:
	default:
		/* cmd 2 is HTTP-upload response correlation only; on WS it is
		 * unsupported (contract §3.4). */
		return API_JSON_UNSUPPORTED_CMD;
	}
}

static int wt_ota_json_init(api_json_module_cfg_t *cfg)
{
	cfg->on_req = on_json_req;
	cfg->module_id = WT_OTA_MODULE_ID;
	return 0;
}

API_JSON_MODULE_REGISTER(wt_ota_json_init)

#endif /* CONFIG_BIRDIEBOT_OTA_ENABLE */
