/**
 * @file dataflow_api_json.c
 * @brief DATA_FLOW JSON module (module 2) — docs/protocol.md §2.3, WP-204
 *
 * Commands 1-10, minimal implementation:
 *   - one instance: the UART service instance (mod_type 130 = UART,
 *     port_info.periph_num = board UART number)
 *   - attach commands always succeed (the Web UI's UART page depends on
 *     ATTACH_CUR_TO_SENDER returning success before it streams data)
 *   - the real binary data plane is uri_bin_router.c (module 4) on top of
 *     components/uart_service
 *
 * Self-registers with API_JSON_MODULE_REGISTER (whole-archive component).
 */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_log.h"

#include "api_json_module.h"
#include "dataflow.h"

#define TAG "dataflow_json"

/* ------------------------------------------------------------------ */
/* JSON helpers                                                       */
/* ------------------------------------------------------------------ */
static cJSON *dataflow_json_new_response(uint16_t cmd)
{
    cJSON *out = cJSON_CreateObject();

    if (out == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(out, "module", DATA_FLOW_MODULE_ID);
    cJSON_AddNumberToObject(out, "cmd", cmd);
    return out;
}

/** Returns 0 when the field is a number, 1 when absent, -1 on bad type. */
static int dataflow_json_get_int(cJSON *in, const char *name, int *value, int default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(in, name);

    if (item == NULL) {
        *value = default_value;
        return 1;
    }
    if (!cJSON_IsNumber(item)) {
        return -1;
    }
    *value = item->valueint;
    return 0;
}

static void dataflow_json_add_attach_array(cJSON *out, bool current_instance_only)
{
    dataflow_attach_t attaches[DATAFLOW_MAX_ATTACH];
    int count = dataflow_attach_list(attaches, DATAFLOW_MAX_ATTACH);
    cJSON *arr = cJSON_AddArrayToObject(out, "attaches");

    if (arr == NULL) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (current_instance_only &&
            attaches[i].s_ins_idx != dataflow_default_instance() &&
            attaches[i].r_ins_idx != dataflow_default_instance()) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(item, "attach_idx", attaches[i].attach_idx);
        cJSON_AddNumberToObject(item, "s_ins_idx", attaches[i].s_ins_idx);
        cJSON_AddNumberToObject(item, "r_ins_idx", attaches[i].r_ins_idx);
        cJSON_AddNumberToObject(item, "data_type", attaches[i].data_type);
        cJSON_AddItemToArray(arr, item);
    }
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                   */
/* ------------------------------------------------------------------ */
static int dataflow_json_get_ins_list(api_json_req_t *req)
{
    cJSON *out = dataflow_json_new_response(DATAFLOW_GET_INS_LIST);

    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }

    cJSON *instances = cJSON_AddArrayToObject(out, "instances");
    cJSON *instance = cJSON_CreateObject();
    if (instances == NULL || instance == NULL) {
        cJSON_Delete(out);
        return API_JSON_INTERNAL_ERR;
    }

    cJSON_AddNumberToObject(instance, "ins_idx", dataflow_default_instance());
    cJSON_AddNumberToObject(instance, "mod_idx", 0);
    cJSON_AddNumberToObject(instance, "mod_type", DATAFLOW_UART_MOD_TYPE);
    cJSON *port_info = cJSON_AddObjectToObject(instance, "port_info");
    if (port_info != NULL) {
        cJSON_AddNumberToObject(port_info, "periph_num", dataflow_uart_periph_num());
    }
    cJSON_AddItemToArray(instances, instance);

    req->out = out;
    return API_JSON_OK;
}

static int dataflow_json_get_cur_ins(api_json_req_t *req)
{
    cJSON *out = dataflow_json_new_response(DATAFLOW_GET_CUR_INS);

    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "ins_idx", dataflow_default_instance());
    cJSON_AddNumberToObject(out, "mod_idx", 0);
    req->out = out;
    return API_JSON_OK;
}

static int dataflow_json_get_attach_list(api_json_req_t *req, uint16_t cmd, bool current_only)
{
    cJSON *out = dataflow_json_new_response(cmd);

    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    dataflow_json_add_attach_array(out, current_only);
    req->out = out;
    return API_JSON_OK;
}

static int dataflow_json_attach(api_json_req_t *req)
{
    int ins_idx;
    int s_ins_idx;
    int r_ins_idx;
    int data_type = DATAFLOW_DATA_TYPE_RAW;

    if (dataflow_json_get_int(req->in, "ins_idx", &ins_idx, dataflow_default_instance()) < 0 ||
        dataflow_json_get_int(req->in, "s_ins_idx", &s_ins_idx, dataflow_default_instance()) < 0 ||
        dataflow_json_get_int(req->in, "r_ins_idx", &r_ins_idx, dataflow_default_instance()) < 0 ||
        dataflow_json_get_int(req->in, "data_type", &data_type, DATAFLOW_DATA_TYPE_RAW) < 0) {
        return API_JSON_PROPERTY_ERR;
    }
    (void)ins_idx;

    uint8_t attach_idx = dataflow_attach_add((uint8_t)s_ins_idx, (uint8_t)r_ins_idx,
                                             (uint8_t)data_type);
    if (attach_idx == 0xFF) {
        return API_JSON_BUSY;
    }

    cJSON *out = dataflow_json_new_response(DATAFLOW_ATTACH);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "attach_idx", attach_idx);
    cJSON_AddNumberToObject(out, "ins_idx", s_ins_idx);
    cJSON_AddNumberToObject(out, "s_ins_idx", s_ins_idx);
    cJSON_AddNumberToObject(out, "r_ins_idx", r_ins_idx);
    cJSON_AddNumberToObject(out, "data_type", data_type);
    req->out = out;
    return API_JSON_OK;
}

static int dataflow_json_attach_cur(api_json_req_t *req, uint16_t cmd)
{
    int data_type = dataflow_get_data_type();
    int ins_idx = dataflow_default_instance();

    if (dataflow_json_get_int(req->in, "data_type", &data_type, data_type) < 0 ||
        dataflow_json_get_int(req->in, "ins_idx", &ins_idx, ins_idx) < 0) {
        return API_JSON_PROPERTY_ERR;
    }

    uint8_t attach_idx = dataflow_attach_add((uint8_t)ins_idx, (uint8_t)ins_idx,
                                             (uint8_t)data_type);
    if (attach_idx == 0xFF) {
        return API_JSON_BUSY;
    }

    cJSON *out = dataflow_json_new_response(cmd);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "attach_idx", attach_idx);
    cJSON_AddNumberToObject(out, "ins_idx", ins_idx);
    cJSON_AddNumberToObject(out, "data_type", data_type);
    req->out = out;
    return API_JSON_OK;
}

static int dataflow_json_detach(api_json_req_t *req, uint16_t cmd)
{
    int attach_idx;

    if (dataflow_json_get_int(req->in, "attach_idx", &attach_idx, -1) != 0 || attach_idx < 0) {
        return API_JSON_PROPERTY_ERR;
    }

    /* Unknown attach_idx is not an error for the Web UI (idempotent detach). */
    (void)dataflow_attach_del((uint8_t)attach_idx);

    cJSON *out = dataflow_json_new_response(cmd);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "attach_idx", attach_idx);
    req->out = out;
    return API_JSON_OK;
}

static int dataflow_json_set_data_type(api_json_req_t *req)
{
    int ins_idx = dataflow_default_instance();
    int data_type;

    if (dataflow_json_get_int(req->in, "ins_idx", &ins_idx, ins_idx) < 0 ||
        dataflow_json_get_int(req->in, "data_type", &data_type, -1) != 0 ||
        data_type < 0) {
        return API_JSON_PROPERTY_ERR;
    }

    dataflow_set_data_type((uint8_t)data_type);

    cJSON *out = dataflow_json_new_response(DATAFLOW_SET_DATA_TYPE);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "ins_idx", ins_idx);
    cJSON_AddNumberToObject(out, "data_type", data_type);
    req->out = out;
    return API_JSON_OK;
}

/* ------------------------------------------------------------------ */
/* Module dispatch / registration                                     */
/* ------------------------------------------------------------------ */
static int on_json_req(uint16_t cmd, api_json_req_t *req, api_json_module_async_t *async)
{
    (void)async;

    if (req == NULL || req->in == NULL) {
        return API_JSON_BAD_REQUEST;
    }

    switch (cmd) {
    case DATAFLOW_GET_INS_LIST:
        return dataflow_json_get_ins_list(req);
    case DATAFLOW_GET_CUR_INS:
        return dataflow_json_get_cur_ins(req);
    case DATAFLOW_GET_CUR_ATTACH_LIST:
        return dataflow_json_get_attach_list(req, cmd, true);
    case DATAFLOW_GET_ATTACH_LIST:
        return dataflow_json_get_attach_list(req, cmd, false);
    case DATAFLOW_ATTACH:
        return dataflow_json_attach(req);
    case DATAFLOW_ATTACH_CUR_TO_RECVER:
    case DATAFLOW_ATTACH_CUR_TO_SENDER:
        return dataflow_json_attach_cur(req, cmd);
    case DATAFLOW_DETACH_SINGLE:
    case DATAFLOW_DETACH_CUR_FROM:
        return dataflow_json_detach(req, cmd);
    case DATAFLOW_SET_DATA_TYPE:
        return dataflow_json_set_data_type(req);
    default:
        ESP_LOGI(TAG, "cmd %u not supported", (unsigned)cmd);
        return API_JSON_UNSUPPORTED_CMD;
    }
}

static int dataflow_api_json_init(api_json_module_cfg_t *cfg)
{
    cfg->on_req = on_json_req;
    cfg->module_id = DATA_FLOW_MODULE_ID;
    return 0;
}

API_JSON_MODULE_REGISTER(dataflow_api_json_init)
