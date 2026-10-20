#ifndef WIFI_JSON_UTILS_H_GUARD
#define WIFI_JSON_UTILS_H_GUARD

#include <stdbool.h>

#include <cJSON.h>

#include "wifi_api.h"
#include "wifi_credential.h"

/* existing cmd 1-10 serializers */
cJSON *wifi_api_json_serialize_ap_info(wifi_api_ap_info_t *ap_info, wifi_api_json_cmd_t cmd);
cJSON *wifi_api_json_serialize_scan_list(wifi_api_ap_scan_info_t *aps_info, uint16_t count);
cJSON *wifi_api_json_serialize_ap_auto(wifi_apsta_mode_e mode, int ap_on_delay, int ap_off_delay);
cJSON *wifi_api_json_serialize_get_mode(wifi_apsta_mode_e mode, int status, int ap_on_delay, int ap_off_delay);
cJSON *wifi_api_json_ser_static_info(wifi_api_sta_ap_static_info_t *info);
int wifi_api_json_deser_static_conf(cJSON *root, wifi_api_sta_ap_static_info_t *static_info);

/**
 * Full error response (docs/protocol.md §1.1):
 * {"module":<m>,"cmd":<c>,"err":"<msg>","code":<code>}
 * The request body is NOT duplicated on purpose: requests may carry plaintext
 * WiFi passwords and responses must never echo them.
 */
cJSON *wifi_api_json_create_err_rsp(cJSON *req, const char *msg, int code);

/* cmd 11-14 serializers (docs/wifi-credential-memory.md §7) */
cJSON *wifi_api_json_serialize_saved_list(const wifi_cred_list_t *list);
cJSON *wifi_api_json_serialize_saved_add(const char *ssid, int priority, uint8_t idx,
                                         bool updated, int evicted_idx);
cJSON *wifi_api_json_serialize_saved_del(uint8_t idx);
cJSON *wifi_api_json_serialize_auto_save(bool enable, bool auto_save);

/* strict request field helpers */
bool wifi_api_json_get_bool(cJSON *req, const char *name, bool *out_value);
int wifi_api_json_get_string(cJSON *req, const char *name, const char **out_value);
int wifi_api_json_get_int_range(cJSON *req, const char *name, int min, int max, int *out_value);
int wifi_api_json_utils_get_int(cJSON *req, const char *name, int *out_value);


#endif //WIFI_JSON_UTILS_H_GUARD
