#include <cJSON.h>

#include <stdio.h>
#include <string.h>

#include "wifi_json_utils.h"
#include "wifi_api.h"
#include "wifi_manager.h"

/* Bounded copy into a fixed buffer (the old code used strncpy with a wrong
 * size and a hardcoded out-of-bounds NUL write, docs §8.1 defect). */
static void wifi_json_copy_str(char *dst, size_t dst_size, const char *src)
{
	size_t len;

	if (dst == NULL || dst_size == 0) {
		return;
	}
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	len = wifi_cred_strnlen(src, dst_size);
	if (len >= dst_size) {
		len = dst_size - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void wifi_api_json_set_header(cJSON *root, uint16_t cmd)
{
	cJSON_AddNumberToObject(root, "cmd", cmd);
	cJSON_AddNumberToObject(root, "module", WIFI_MODULE_ID);
}

cJSON *wifi_api_json_serialize_ap_info(wifi_api_ap_info_t *ap_info, wifi_api_json_cmd_t cmd)
{
	cJSON *root;
	char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];
	char password[BIRDIEBOT_WIFI_PASS_MAX + 1];

	if (ap_info == NULL) {
		return NULL;
	}
	wifi_json_copy_str(ssid, sizeof(ssid), ap_info->ssid);
	wifi_json_copy_str(password, sizeof(password), ap_info->password);

	root = cJSON_CreateObject();
	wifi_api_json_set_header(root, cmd);
	cJSON_AddStringToObject(root, "ip", ip4addr_ntoa(&ap_info->ip));
	cJSON_AddStringToObject(root, "gateway", ip4addr_ntoa(&ap_info->gateway));
	cJSON_AddStringToObject(root, "netmask", ip4addr_ntoa(&ap_info->netmask));
	cJSON_AddStringToObject(root, "dns_main", ip4addr_ntoa(&ap_info->dns_main));
	cJSON_AddStringToObject(root, "dns_backup", ip4addr_ntoa(&ap_info->dns_backup));
	cJSON_AddNumberToObject(root, "rssi", ap_info->rssi);
	cJSON_AddStringToObject(root, "ssid", ssid);
	if (password[0] != '\0') {
		cJSON_AddStringToObject(root, "password", password);
	}

	char mac_str[18];
	char *m = ap_info->mac;
	snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
	         m[0], m[1], m[2], m[3], m[4], m[5]);
	cJSON_AddStringToObject(root, "mac", mac_str);

	/* DAP latency contract (docs/wifi-no-power-save.md): expose the effective
	 * station power-save mode with the STA info so a Web/host client can verify
	 * that modem sleep really is off (the IDF default is min_modem). The AP
	 * info response (cmd 5) is unchanged; GET_MODE stays byte-compatible. */
	if (cmd == WIFI_API_JSON_STA_GET_AP_INFO) {
		wifi_ps_type_t ps = wifi_manager_get_power_save();
		cJSON_AddStringToObject(root, "ps", wifi_manager_power_save_name(ps));
		cJSON_AddBoolToObject(root, "ps_none", ps == WIFI_PS_NONE);
	}

	return root;
}

cJSON *wifi_api_json_serialize_scan_list(wifi_api_ap_scan_info_t *aps_info, uint16_t count)
{
	cJSON *root;
	char mac_str[18];

	root = cJSON_CreateObject();
	wifi_api_json_set_header(root, WIFI_API_JSON_GET_SCAN);
	cJSON *scan_list = cJSON_AddArrayToObject(root, "scan_list");

	for (int i = 0; i < count; ++i) {
		cJSON *ap = cJSON_CreateObject();
		char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];

		wifi_json_copy_str(ssid, sizeof(ssid), aps_info[i].ssid);
		cJSON_AddNumberToObject(ap, "rssi", aps_info[i].rssi);
		cJSON_AddStringToObject(ap, "ssid", ssid);
		char *m = aps_info[i].mac;
		snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
		         m[0], m[1], m[2], m[3], m[4], m[5]);
		cJSON_AddStringToObject(ap, "mac", mac_str);
		cJSON_AddItemToArray(scan_list, ap);
	}

	return root;
}

cJSON *wifi_api_json_serialize_ap_auto(wifi_apsta_mode_e mode, int ap_on_delay, int ap_off_delay)
{
	cJSON *root;

	root = cJSON_CreateObject();
	/* The old code labelled this response with cmd=GET_SCAN (docs §8.1). */
	wifi_api_json_set_header(root, WIFI_API_JSON_SET_MODE);
	cJSON_AddNumberToObject(root, "mode", mode);
	cJSON_AddNumberToObject(root, "ap_on_delay", ap_on_delay);
	cJSON_AddNumberToObject(root, "ap_off_delay", ap_off_delay);

	return root;
}

cJSON *wifi_api_json_create_err_rsp(cJSON *req, const char *msg, int code)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *module = (req != NULL) ? cJSON_GetObjectItem(req, "module") : NULL;
	cJSON *cmd = (req != NULL) ? cJSON_GetObjectItem(req, "cmd") : NULL;

	cJSON_AddNumberToObject(root, "module",
	                        cJSON_IsNumber(module) ? module->valueint : WIFI_MODULE_ID);
	cJSON_AddNumberToObject(root, "cmd",
	                        cJSON_IsNumber(cmd) ? cmd->valueint : UNKNOWN);
	cJSON_AddStringToObject(root, "err", msg != NULL ? msg : "Internal error");
	cJSON_AddNumberToObject(root, "code", code);
	return root;
}

bool wifi_api_json_get_bool(cJSON *req, const char *name, bool *out_value)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, name);

	/* cJSON_IsBool() is only true for JSON true/false, not for numbers or
	 * strings (contract §7.4: numbers/strings must be rejected with code 5). */
	if (!cJSON_IsBool(item) || out_value == NULL) {
		return false;
	}
	*out_value = cJSON_IsTrue(item) ? true : false;
	return true;
}

int wifi_api_json_get_string(cJSON *req, const char *name, const char **out_value)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, name);

	if (!cJSON_IsString(item) || item->valuestring == NULL || out_value == NULL) {
		return 1;
	}
	*out_value = item->valuestring;
	return 0;
}

int wifi_api_json_get_int_range(cJSON *req, const char *name, int min, int max, int *out_value)
{
	cJSON *item;

	if (out_value == NULL) {
		return 1;
	}
	item = cJSON_GetObjectItemCaseSensitive(req, name);
	if (!cJSON_IsNumber(item)) {
		return 1;
	}
	/* Reject non integral JSON numbers instead of silently truncating them. */
	if (item->valuedouble != (double)item->valueint) {
		return 1;
	}
	if (item->valueint < min || item->valueint > max) {
		return 1;
	}
	*out_value = item->valueint;
	return 0;
}

int wifi_api_json_utils_get_int(cJSON *req, const char *name, int *out_value)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(req, name);

	if (!cJSON_IsNumber(item)) {
		return 1;
	}

	*out_value = item->valueint;
	return 0;
}

cJSON *wifi_api_json_serialize_get_mode(wifi_apsta_mode_e mode, int status, int ap_on_delay, int ap_off_delay)
{
	cJSON *root;

	root = cJSON_CreateObject();
	wifi_api_json_set_header(root, WIFI_API_JSON_GET_MODE);
	cJSON_AddNumberToObject(root, "mode", mode);
	cJSON_AddNumberToObject(root, "status", status);
	if (ap_on_delay >= 0 && ap_off_delay >= 0) {
		cJSON_AddNumberToObject(root, "ap_on_delay", ap_on_delay);
		cJSON_AddNumberToObject(root, "ap_off_delay", ap_off_delay);
	}

	return root;
}

cJSON *wifi_api_json_ser_static_info(wifi_api_sta_ap_static_info_t *info)
{
	cJSON *root;

	root = cJSON_CreateObject();
	wifi_api_json_set_header(root, WIFI_API_JSON_STA_GET_STATIC_INFO);
	cJSON_AddNumberToObject(root, "static_ip_en", info->static_ip_en);
	cJSON_AddNumberToObject(root, "static_dns_en", info->static_dns_en);

	cJSON_AddStringToObject(root, "ip", ip4addr_ntoa(&info->ip));
	cJSON_AddStringToObject(root, "gateway", ip4addr_ntoa(&info->gateway));
	cJSON_AddStringToObject(root, "netmask", ip4addr_ntoa(&info->netmask));
	cJSON_AddStringToObject(root, "dns_main", ip4addr_ntoa(&info->dns_main));
	cJSON_AddStringToObject(root, "dns_backup", ip4addr_ntoa(&info->dns_backup));

	return root;
}

int wifi_api_json_deser_static_conf(cJSON *root, wifi_api_sta_ap_static_info_t *static_info)
{
	cJSON *static_ip_en = cJSON_GetObjectItemCaseSensitive(root, "static_ip_en");
	cJSON *static_dns_en = cJSON_GetObjectItemCaseSensitive(root, "static_dns_en");

	if (!cJSON_IsNumber(static_ip_en) || !cJSON_IsNumber(static_dns_en)) {
		return 1;
	}

	static_info->static_dns_en = static_dns_en->valueint;
	static_info->static_ip_en = static_ip_en->valueint;

	cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "ip");
	cJSON *gateway = cJSON_GetObjectItemCaseSensitive(root, "gateway");
	cJSON *netmask = cJSON_GetObjectItemCaseSensitive(root, "netmask");
	cJSON *dns_main = cJSON_GetObjectItemCaseSensitive(root, "dns_main");
	cJSON *dns_backup = cJSON_GetObjectItemCaseSensitive(root, "dns_backup");

	if (cJSON_IsString(ip)) {
		ip4addr_aton(ip->valuestring, &static_info->ip);
	}
	if (cJSON_IsString(gateway)) {
		ip4addr_aton(gateway->valuestring, &static_info->gateway);
	}
	if (cJSON_IsString(netmask)) {
		ip4addr_aton(netmask->valuestring, &static_info->netmask);
	}
	if (cJSON_IsString(dns_main)) {
		ip4addr_aton(dns_main->valuestring, &static_info->dns_main);
	}
	if (cJSON_IsString(dns_backup)) {
		ip4addr_aton(dns_backup->valuestring, &static_info->dns_backup);
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* cmd 11-14 serializers (docs/wifi-credential-memory.md §7)                 */
/* ------------------------------------------------------------------------- */

cJSON *wifi_api_json_serialize_saved_list(const wifi_cred_list_t *list)
{
	cJSON *root;
	cJSON *array;

	root = cJSON_CreateObject();
	wifi_api_json_set_header(root, WIFI_API_JSON_GET_SAVED_LIST);
	cJSON_AddBoolToObject(root, "auto_save", list->auto_save);
	cJSON_AddNumberToObject(root, "count", list->count);
	/* Incremental field from contract b015a0c (optional for clients; the UI
	 * falls back to 8 when missing). */
	cJSON_AddNumberToObject(root, "max", list->max);
	array = cJSON_AddArrayToObject(root, "list");
	for (uint8_t i = 0; i < list->count && i < BIRDIEBOT_WIFI_CRED_MAX; ++i) {
		cJSON *item = cJSON_CreateObject();
		char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];

		wifi_json_copy_str(ssid, sizeof(ssid), list->item[i].ssid);
		cJSON_AddNumberToObject(item, "idx", list->item[i].idx);
		cJSON_AddStringToObject(item, "ssid", ssid);
		cJSON_AddNumberToObject(item, "last_success", list->item[i].last_success_ts);
		cJSON_AddNumberToObject(item, "priority", list->item[i].priority);
		cJSON_AddItemToArray(array, item);
	}
	if (list->connected_idx != BIRDIEBOT_WIFI_CRED_NONE) {
		cJSON_AddNumberToObject(root, "connected_idx", list->connected_idx);
	}
	return root;
}

cJSON *wifi_api_json_serialize_saved_add(const char *ssid, int priority, uint8_t idx,
                                         bool updated, int evicted_idx)
{
	cJSON *root;
	char ssid_buf[BIRDIEBOT_WIFI_SSID_MAX + 1];

	root = cJSON_CreateObject();
	wifi_api_json_set_header(root, WIFI_API_JSON_ADD_SAVED);
	wifi_json_copy_str(ssid_buf, sizeof(ssid_buf), ssid);
	cJSON_AddStringToObject(root, "ssid", ssid_buf);
	cJSON_AddNumberToObject(root, "priority", priority);
	cJSON_AddNumberToObject(root, "idx", idx);
	cJSON_AddBoolToObject(root, "updated", updated);
	if (evicted_idx >= 0) {
		cJSON_AddNumberToObject(root, "evicted_idx", evicted_idx);
	}
	/* Security exception (§7.2): the password is never echoed. */
	return root;
}

cJSON *wifi_api_json_serialize_saved_del(uint8_t idx)
{
	cJSON *root = cJSON_CreateObject();

	wifi_api_json_set_header(root, WIFI_API_JSON_DEL_SAVED);
	cJSON_AddNumberToObject(root, "idx", idx);
	return root;
}

cJSON *wifi_api_json_serialize_auto_save(bool enable, bool auto_save)
{
	cJSON *root = cJSON_CreateObject();

	wifi_api_json_set_header(root, WIFI_API_JSON_SET_AUTO_SAVE);
	cJSON_AddBoolToObject(root, "enable", enable);
	cJSON_AddBoolToObject(root, "auto_save", auto_save);
	return root;
}
