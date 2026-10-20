#include "api_json_module.h"
#include "wifi_api.h"
#include "wifi_credential.h"
#include "wifi_json_utils.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <stdio.h>
#include <esp_err.h>
#include <esp_log.h>

#include "wifi_storage.h"

#define TAG __FILENAME__

static int wifi_api_json_sta_get_ap_info(api_json_req_t *req);

static int wifi_api_json_get_scan(api_json_req_t *req);

static int wifi_api_json_connect(api_json_req_t *req);

static int wifi_api_json_disconnect(api_json_req_t *req);

static int wifi_api_json_ap_get_info(api_json_req_t *req);

static int wifi_api_json_set_mode(api_json_req_t *req);

static int wifi_api_json_get_mode(api_json_req_t *req);
static int wifi_api_json_set_ap_cred(api_json_req_t *req);
static int wifi_api_json_sta_get_static_info(api_json_req_t *req);
static int wifi_api_json_sta_set_static_conf(api_json_req_t *req);
static int wifi_api_json_get_saved_list(api_json_req_t *req);
static int wifi_api_json_add_saved(api_json_req_t *req);
static int wifi_api_json_del_saved(api_json_req_t *req);
static int wifi_api_json_set_auto_save(api_json_req_t *req);

/* the upper caller call cb() with void *, this let us use custom function arg */
static int async_helper_cb(void *arg)
{
	api_json_module_req_t *req = arg;
	return req->func(req->arg);
}

static inline int set_async(api_json_req_t *req, api_json_module_async_t *async, int (*func)(api_json_req_t *))
{
	async->module.func = func;
	async->module.arg = req;
	async->req_task.module.cb = async_helper_cb;
	async->req_task.module.arg = &async->module;
	return API_JSON_ASYNC;
}

/* Map credential-memory/ESP errors to docs/protocol.md §1.1 error codes. */
static int wifi_cred_err_to_code(int err)
{
	switch (err) {
	case ESP_ERR_INVALID_ARG:
		return API_JSON_PROPERTY_ERR;
	case ESP_ERR_INVALID_STATE:
	case ESP_ERR_NO_MEM:
		return API_JSON_BUSY;
	default:
		return API_JSON_INTERNAL_ERR;
	}
}

static int on_json_req(uint16_t cmd, api_json_req_t *req, api_json_module_async_t *async)
{
	wifi_api_json_cmd_t wifi_cmd = cmd;
	switch (wifi_cmd) {
	case UNKNOWN:
	default:
		break;
	case WIFI_API_JSON_STA_GET_AP_INFO:
		return wifi_api_json_sta_get_ap_info(req);
	case WIFI_API_JSON_CONNECT:
		return set_async(req, async, wifi_api_json_connect);
	case WIFI_API_JSON_GET_SCAN:
		return set_async(req, async, wifi_api_json_get_scan);
	case WIFI_API_JSON_DISCONNECT:
		return wifi_api_json_disconnect(req);
	case WIFI_API_JSON_AP_GET_INFO:
		return wifi_api_json_ap_get_info(req);
	case WIFI_API_JSON_GET_MODE:
		return wifi_api_json_get_mode(req);
	case WIFI_API_JSON_SET_MODE:
		return wifi_api_json_set_mode(req);
	case WIFI_API_JSON_SET_AP_CRED:
		return wifi_api_json_set_ap_cred(req);
	case WIFI_API_JSON_STA_GET_STATIC_INFO:
		return wifi_api_json_sta_get_static_info(req);
	case WIFI_API_JSON_STA_SET_STATIC_CONF:
		return wifi_api_json_sta_set_static_conf(req);
	case WIFI_API_JSON_GET_SAVED_LIST:
		return wifi_api_json_get_saved_list(req);
	case WIFI_API_JSON_ADD_SAVED:
		return wifi_api_json_add_saved(req);
	case WIFI_API_JSON_DEL_SAVED:
		return wifi_api_json_del_saved(req);
	case WIFI_API_JSON_SET_AUTO_SAVE:
		return wifi_api_json_set_auto_save(req);
	}

	ESP_LOGI(TAG, "cmd %d not executed\n", cmd);
	return API_JSON_UNSUPPORTED_CMD;
}

/* ****
 *  register module
 * */
static int wifi_api_json_init(api_json_module_cfg_t *cfg)
{
	cfg->on_req = on_json_req;
	cfg->module_id = WIFI_MODULE_ID;
	return 0;
}

API_JSON_MODULE_REGISTER(wifi_api_json_init)


static int wifi_api_json_sta_get_ap_info(api_json_req_t *req)
{
	wifi_api_ap_info_t ap_info;
	wifi_api_sta_get_ap_info(&ap_info);
	req->out = wifi_api_json_serialize_ap_info(&ap_info, WIFI_API_JSON_STA_GET_AP_INFO);
	return API_JSON_OK;
}

static int wifi_api_json_ap_get_info(api_json_req_t *req)
{
	wifi_api_ap_info_t ap_info;
	wifi_api_ap_get_info(&ap_info);
	req->out = wifi_api_json_serialize_ap_info(&ap_info, WIFI_API_JSON_AP_GET_INFO);
	return API_JSON_OK;
}

static int wifi_api_json_get_scan(api_json_req_t *req)
{
	wifi_api_ap_scan_info_t ap_info[20];
	uint16_t max_count = 20;
	int err;

	ESP_LOGI(TAG, "get scan\n");

	err = wifi_api_get_scan_list(&max_count, ap_info);
	if (err != 0) {
		/* The old code ignored every error except a magic ESP_ERR_NOT_FINISHED
		 * and then serialised a stale/uninitialised list. */
		if (err == ESP_ERR_NOT_FINISHED) {
			req->out = wifi_api_json_create_err_rsp(req->in, "Wi-Fi scan busy", API_JSON_BUSY);
		} else {
			req->out = wifi_api_json_create_err_rsp(req->in, "Wi-Fi scan failed",
			                                        API_JSON_INTERNAL_ERR);
		}
		/* Frontend (Wifi.vue) iterates scan_list without checking err. */
		cJSON_AddArrayToObject(req->out, "scan_list");
		return API_JSON_OK;
	}

	ESP_LOGI(TAG, "scan ok\n");

	req->out = wifi_api_json_serialize_scan_list(ap_info, max_count);
	return API_JSON_OK;
}


static int wifi_api_json_connect(api_json_req_t *req)
{
	const char *ssid = NULL;
	const char *password = NULL;
	cJSON *save_item;
	int save = WIFI_API_CONNECT_SAVE_AUTO;
	int err;

	if (wifi_api_json_get_string(req->in, "ssid", &ssid) != 0 ||
	    wifi_api_json_get_string(req->in, "password", &password) != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'ssid'/'password' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	if (!wifi_cred_ssid_valid(ssid)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'ssid' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	if (!wifi_cred_password_valid(password)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'password' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}

	/* Optional `save` (contract §7.5): 0 = do not persist, 1 = force persist,
	 * absent = follow the auto_save setting. */
	save_item = cJSON_GetObjectItemCaseSensitive(req->in, "save");
	if (save_item != NULL) {
		if (cJSON_IsBool(save_item)) {
			save = cJSON_IsTrue(save_item) ? WIFI_API_CONNECT_SAVE_YES
			                               : WIFI_API_CONNECT_SAVE_NO;
		} else if (cJSON_IsNumber(save_item) &&
		           save_item->valuedouble == (double)save_item->valueint &&
		           (save_item->valueint == 0 || save_item->valueint == 1)) {
			save = save_item->valueint;
		} else {
			req->out = wifi_api_json_create_err_rsp(req->in, "'save' invalid",
			                                        API_JSON_PROPERTY_ERR);
			return API_JSON_OK;
		}
	}

	err = wifi_api_connect_save(ssid, password, save);
	if (err) {
		/* Async path: return a full error JSON (ws_on_text_data drops the bare
		 * error frame because it has no module/cmd). */
		req->out = wifi_api_json_create_err_rsp(req->in, "connect failed",
		                                        API_JSON_INTERNAL_ERR);
		return API_JSON_OK;
	}

	return wifi_api_json_sta_get_ap_info(req);
}

int wifi_api_json_set_mode(api_json_req_t *req)
{
	int value;
	int err;

	err = wifi_api_json_utils_get_int(req->in, "mode", &value);
	if (err) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'mode' attribute missing",
		                                        API_JSON_PROPERTY_ERR);
		ESP_LOGE(TAG, "ap stop: %s", esp_err_to_name(err));
		return API_JSON_OK;
	}
	wifi_apsta_mode_e mode = value;

	err = wifi_manager_change_mode(mode);
	if (err) {
		req->out = wifi_api_json_create_err_rsp(req->in, "Change mode Failed",
		                                        API_JSON_INTERNAL_ERR);
		ESP_LOGE(TAG, "ap stop: %s", esp_err_to_name(err));
		return API_JSON_OK;
	}

	if (mode == WIFI_AP_AUTO_STA_ON) {
		int ap_on_delay;
		int ap_off_delay;
		err = wifi_api_json_utils_get_int(req->in, "ap_on_delay", &ap_on_delay);
		err |= wifi_api_json_utils_get_int(req->in, "ap_off_delay", &ap_off_delay);
		if (err == 0) {
			wifi_manager_set_ap_auto_delay(&ap_on_delay, &ap_off_delay);
			req->out = wifi_api_json_serialize_ap_auto(mode, ap_on_delay, ap_off_delay);
			return API_JSON_OK;
		}
	}

	return API_JSON_OK;
}

int wifi_api_json_get_mode(api_json_req_t *req)
{
	wifi_apsta_mode_e mode;
	wifi_mode_t status;
	int ap_on_delay, ap_off_delay;

	wifi_manager_get_mode(&mode, &status);
	if (mode == WIFI_AP_AUTO_STA_ON) {
		wifi_manager_get_ap_auto_delay(&ap_on_delay, &ap_off_delay);
	} else {
		ap_on_delay = -1;
		ap_off_delay = -1;
	}

	req->out = wifi_api_json_serialize_get_mode(mode, status, ap_on_delay, ap_off_delay);
	return API_JSON_OK;
}


int wifi_api_json_disconnect(api_json_req_t *req)
{
	(void)req;
	/* cmd 4: stop automatic reconnection, keep the AP up. The sync success path
	 * echoes the request frame (protocol §1.1 implicit ACK). */
	wifi_api_disconnect();
	return API_JSON_OK;
}


int wifi_api_json_set_ap_cred(api_json_req_t *req)
{
	const char *ssid = NULL;
	const char *password = NULL;
	int err;

	if (wifi_api_json_get_string(req->in, "ssid", &ssid) != 0 ||
	    wifi_api_json_get_string(req->in, "password", &password) != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'ssid'/'password' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	/* Empty ssid restores the dynamic WiFi-Dap-XXXX default (§6). */
	if (ssid[0] != '\0' && !wifi_cred_ssid_valid(ssid)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'ssid' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	if (!wifi_cred_ap_password_valid(password)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'password' must be 8..63 bytes",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}

	err = wifi_cred_mem_set_ap_cred(ssid, password);
	if (err != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "nvs write failed",
		                                        wifi_cred_err_to_code(err));
		return API_JSON_OK;
	}
	wifi_manager_refresh_ap_credential();
	return API_JSON_OK;
}

int wifi_api_json_sta_get_static_info(api_json_req_t *req)
{
	wifi_api_sta_ap_static_info_t static_info = {0};
	int err = wifi_data_get_static(&static_info);
	if (err) {
		req->out = wifi_api_json_create_err_rsp(req->in, "nvs read failed",
		                                        API_JSON_INTERNAL_ERR);
		return API_JSON_OK;
	}

	req->out = wifi_api_json_ser_static_info(&static_info);
	return API_JSON_OK;
}

int wifi_api_json_sta_set_static_conf(api_json_req_t *req)
{
	wifi_api_sta_ap_static_info_t static_info = {0};
	int err;
	wifi_data_get_static(&static_info);

	err = wifi_api_json_deser_static_conf(req->in, &static_info);
	if (err) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'static_ip_en'/'static_dns_en' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}

	wifi_api_sta_set_static_conf(&static_info);
	return API_JSON_OK;
}

/* ------------------------------------------------------------------------- */
/* cmd 11-14: multi WiFi credential memory (§7)                              */
/* ------------------------------------------------------------------------- */

static int wifi_api_json_get_saved_list(api_json_req_t *req)
{
	wifi_cred_list_t list;
	int err;

	err = wifi_cred_mem_get_list(&list);
	if (err != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "nvs read failed",
		                                        API_JSON_INTERNAL_ERR);
		return API_JSON_OK;
	}
	req->out = wifi_api_json_serialize_saved_list(&list);
	return API_JSON_OK;
}

static int wifi_api_json_add_saved(api_json_req_t *req)
{
	const char *ssid = NULL;
	const char *password = NULL;
	int priority = BIRDIEBOT_WIFI_CRED_PRIORITY_DEFAULT;
	uint8_t idx = BIRDIEBOT_WIFI_CRED_NONE;
	bool updated = false;
	int evicted_idx = -1;
	int err;

	if (wifi_api_json_get_string(req->in, "ssid", &ssid) != 0 ||
	    wifi_api_json_get_string(req->in, "password", &password) != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'ssid'/'password' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	if (!wifi_cred_ssid_valid(ssid)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'ssid' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	if (!wifi_cred_password_valid(password)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'password' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	if (cJSON_GetObjectItemCaseSensitive(req->in, "priority") != NULL &&
	    wifi_api_json_get_int_range(req->in, "priority", 0, 255, &priority) != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'priority' invalid",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}

	err = wifi_cred_mem_add(ssid, password, priority, &idx, &updated, &evicted_idx);
	if (err != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "nvs write failed",
		                                        wifi_cred_err_to_code(err));
		return API_JSON_OK;
	}
	req->out = wifi_api_json_serialize_saved_add(ssid, priority, idx, updated, evicted_idx);
	return API_JSON_OK;
}

static int wifi_api_json_del_saved(api_json_req_t *req)
{
	int idx = -1;
	int err;

	if (wifi_api_json_get_int_range(req->in, "idx", 0, BIRDIEBOT_WIFI_CRED_MAX - 1,
	                                &idx) != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'idx' out of range",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	err = wifi_cred_mem_del((uint8_t)idx);
	if (err != 0) {
		if (err == ESP_ERR_INVALID_ARG) {
			req->out = wifi_api_json_create_err_rsp(req->in, "'idx' out of range",
			                                        API_JSON_PROPERTY_ERR);
		} else {
			req->out = wifi_api_json_create_err_rsp(req->in, "nvs write failed",
			                                        wifi_cred_err_to_code(err));
		}
		return API_JSON_OK;
	}
	req->out = wifi_api_json_serialize_saved_del((uint8_t)idx);
	return API_JSON_OK;
}

static int wifi_api_json_set_auto_save(api_json_req_t *req)
{
	bool enable = false;
	int err;

	if (!wifi_api_json_get_bool(req->in, "enable", &enable)) {
		req->out = wifi_api_json_create_err_rsp(req->in, "'enable' must be a bool",
		                                        API_JSON_PROPERTY_ERR);
		return API_JSON_OK;
	}
	err = wifi_cred_mem_set_auto_save(enable);
	if (err != 0) {
		req->out = wifi_api_json_create_err_rsp(req->in, "nvs write failed",
		                                        wifi_cred_err_to_code(err));
		return API_JSON_OK;
	}
	req->out = wifi_api_json_serialize_auto_save(enable, wifi_cred_mem_get_auto_save());
	return API_JSON_OK;
}
