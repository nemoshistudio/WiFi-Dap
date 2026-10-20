/**
 * @file uart_service_api_json.c
 * @brief UART JSON module (module 4) — docs/protocol.md §2.4, WP-203
 *
 * Commands: GET_AVAILABLE_NUMS(1), GET/SET_BAUD(4/5), GET/SET_CONFIG(6/7),
 * GET/SET_FLOW_CTRL(8/9), GET/SET_PINS_NUM(10/11), GET/SET_MODE(12/13),
 * GET/SET_STATUS(20/21), GET/SET_DATA_TYPE(22/23), GET_DEFAULT_NUM(24).
 *
 * Self-registers with API_JSON_MODULE_REGISTER (same pattern as the C tree's
 * wifi_manager / wt_system modules).  Response JSON keeps the established
 * "module"/"cmd" header fields.  PINS_NUM/MODE are not implemented at runtime:
 * per the contract they echo the request and add err:"unsupported" so that the
 * front end does not block.
 */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_log.h"

#include "api_json_module.h"
#include "uart_service.h"

#define TAG "uart_json"

/* UART(4) command ids (docs/protocol.md §2.4). */
enum uart_json_cmd {
    UART_JSON_GET_AVAILABLE_NUMS = 1,
    UART_JSON_GET_BAUD = 4,
    UART_JSON_SET_BAUD = 5,
    UART_JSON_GET_CONFIG = 6,
    UART_JSON_SET_CONFIG = 7,
    UART_JSON_GET_FLOW_CTRL = 8,
    UART_JSON_SET_FLOW_CTRL = 9,
    UART_JSON_GET_PINS_NUM = 10,
    UART_JSON_SET_PINS_NUM = 11,
    UART_JSON_GET_MODE = 12,
    UART_JSON_SET_MODE = 13,
    UART_JSON_GET_STATUS = 20,
    UART_JSON_SET_STATUS = 21,
    UART_JSON_GET_DATA_TYPE = 22,
    UART_JSON_SET_DATA_TYPE = 23,
    UART_JSON_GET_DEFAULT_NUM = 24,
};

/* ------------------------------------------------------------------ */
/* JSON helpers                                                       */
/* ------------------------------------------------------------------ */
static cJSON *uart_json_new_response(uint16_t cmd)
{
    cJSON *out = cJSON_CreateObject();

    if (out == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(out, "module", UART_SERVICE_MODULE_ID);
    cJSON_AddNumberToObject(out, "cmd", cmd);
    return out;
}

/**
 * Echo the request fields (used by the "not implemented" commands).
 * The auth token is not echoed back.
 */
static cJSON *uart_json_echo_request(cJSON *in, uint16_t cmd)
{
    cJSON *out = cJSON_Duplicate(in, 1);

    if (out == NULL) {
        return NULL;
    }
    cJSON_DeleteItemFromObject(out, "token");
    if (cJSON_GetObjectItemCaseSensitive(out, "cmd") == NULL) {
        cJSON_AddNumberToObject(out, "cmd", cmd);
    }
    if (cJSON_GetObjectItemCaseSensitive(out, "module") == NULL) {
        cJSON_AddNumberToObject(out, "module", UART_SERVICE_MODULE_ID);
    }
    return out;
}

/** Returns 0 when the field is a number, 1 when it is absent, -1 on bad type. */
static int uart_json_get_int(cJSON *in, const char *name, int *value, int default_value)
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

/** Returns 0 on success, -1 on bad type/absent. */
static int uart_json_get_bool(cJSON *in, const char *name, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(in, name);

    if (item == NULL) {
        return -1;
    }
    if (cJSON_IsBool(item)) {
        *value = cJSON_IsTrue(item);
        return 0;
    }
    if (cJSON_IsNumber(item)) {
        *value = item->valueint != 0;
        return 0;
    }
    return -1;
}

/** sub_mod is the UART instance: only instance 0 exists. */
static int uart_json_validate_sub_mod(cJSON *in)
{
    int sub_mod = UART_SERVICE_DEFAULT_INSTANCE;

    if (uart_json_get_int(in, "sub_mod", &sub_mod, UART_SERVICE_DEFAULT_INSTANCE) < 0) {
        return API_JSON_PROPERTY_ERR;
    }
    if (sub_mod < 0 || sub_mod >= uart_service_instance_count()) {
        return API_JSON_PROPERTY_ERR;
    }
    return API_JSON_OK;
}

static int uart_json_status_from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return API_JSON_OK;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NOT_SUPPORTED:
        return API_JSON_PROPERTY_ERR;
    default:
        return API_JSON_INTERNAL_ERR;
    }
}

static void uart_json_add_config(cJSON *out, const uart_service_config_t *cfg)
{
    cJSON_AddNumberToObject(out, "baud", cfg->baud);
    cJSON_AddNumberToObject(out, "data_bits", cfg->data_bits);
    cJSON_AddNumberToObject(out, "parity", cfg->parity);
    cJSON_AddNumberToObject(out, "stop_bits", cfg->stop_bits);
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                   */
/* ------------------------------------------------------------------ */
static int uart_json_get_config_all(api_json_req_t *req, uint16_t cmd)
{
    uart_service_config_t cfg;
    esp_err_t err = uart_service_get_config(&cfg);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(cmd);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    uart_json_add_config(out, &cfg);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_get_baud(api_json_req_t *req, uint16_t cmd)
{
    uart_service_config_t cfg;
    esp_err_t err = uart_service_get_config(&cfg);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(cmd);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "baud", cfg.baud);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_set_baud(api_json_req_t *req)
{
    int baud = 0;

    if (uart_json_get_int(req->in, "baud", &baud, 0) != 0 || baud <= 0) {
        return API_JSON_PROPERTY_ERR;
    }

    esp_err_t err = uart_service_set_baud((uint32_t)baud);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    uart_service_config_t cfg;
    err = uart_service_get_config(&cfg);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(UART_JSON_SET_BAUD);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "baud", cfg.baud);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_set_config(api_json_req_t *req)
{
    uart_service_config_t cfg;
    int value;

    esp_err_t err = uart_service_get_config(&cfg);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    if (uart_json_get_int(req->in, "data_bits", &value, cfg.data_bits) < 0) {
        return API_JSON_PROPERTY_ERR;
    }
    cfg.data_bits = (uint8_t)value;

    if (uart_json_get_int(req->in, "parity", &value, cfg.parity) < 0) {
        return API_JSON_PROPERTY_ERR;
    }
    cfg.parity = (uint8_t)value;

    if (uart_json_get_int(req->in, "stop_bits", &value, cfg.stop_bits) < 0) {
        return API_JSON_PROPERTY_ERR;
    }
    cfg.stop_bits = (uint8_t)value;

    int baud = (int)cfg.baud;
    int baud_present = uart_json_get_int(req->in, "baud", &baud, (int)cfg.baud);
    if (baud_present < 0) {
        return API_JSON_PROPERTY_ERR;
    }
    if (baud_present == 0) {
        if (baud <= 0) {
            return API_JSON_PROPERTY_ERR;
        }
        cfg.baud = (uint32_t)baud;
    }

    err = uart_service_set_config(&cfg);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(UART_JSON_SET_CONFIG);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    uart_json_add_config(out, &cfg);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_get_flow_ctrl(api_json_req_t *req, uint16_t cmd)
{
    uart_service_config_t cfg;
    esp_err_t err = uart_service_get_config(&cfg);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(cmd);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "flow_ctrl", cfg.flow_ctrl);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_set_flow_ctrl(api_json_req_t *req)
{
    int flow_ctrl = 0;

    if (uart_json_get_int(req->in, "flow_ctrl", &flow_ctrl, 0) < 0) {
        return API_JSON_PROPERTY_ERR;
    }

    if (flow_ctrl != 0) {
        /* No RTS/CTS pins in the board pin map: contract-compatible
         * "echo + err unsupported". */
        cJSON *out = uart_json_echo_request(req->in, UART_JSON_SET_FLOW_CTRL);
        if (out == NULL) {
            return API_JSON_INTERNAL_ERR;
        }
        cJSON_AddStringToObject(out, "err", "unsupported");
        req->out = out;
        return API_JSON_OK;
    }

    cJSON *out = uart_json_new_response(UART_JSON_SET_FLOW_CTRL);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "flow_ctrl", 0);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_unsupported_echo(api_json_req_t *req, uint16_t cmd)
{
    cJSON *out = uart_json_echo_request(req->in, cmd);

    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddStringToObject(out, "err", "unsupported");
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_get_status(api_json_req_t *req)
{
    esp_err_t err = uart_service_init();
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(UART_JSON_GET_STATUS);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddBoolToObject(out, "enable", uart_service_is_enabled());
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_set_status(api_json_req_t *req)
{
    bool enable = true;

    if (uart_json_get_bool(req->in, "enable", &enable) != 0) {
        return API_JSON_PROPERTY_ERR;
    }

    esp_err_t err = uart_service_set_enabled(enable);
    if (err != ESP_OK) {
        return uart_json_status_from_esp(err);
    }

    cJSON *out = uart_json_new_response(UART_JSON_SET_STATUS);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddBoolToObject(out, "enable", enable);
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_get_data_type(api_json_req_t *req)
{
    cJSON *out = uart_json_new_response(UART_JSON_GET_DATA_TYPE);

    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "data_type", uart_service_get_data_type());
    req->out = out;
    return API_JSON_OK;
}

static int uart_json_set_data_type(api_json_req_t *req)
{
    int data_type = 0;

    if (uart_json_get_int(req->in, "data_type", &data_type, -1) < 0 || data_type < 0) {
        return API_JSON_PROPERTY_ERR;
    }

    if (uart_service_set_data_type((uint8_t)data_type) != ESP_OK) {
        return uart_json_unsupported_echo(req, UART_JSON_SET_DATA_TYPE);
    }

    cJSON *out = uart_json_new_response(UART_JSON_SET_DATA_TYPE);
    if (out == NULL) {
        return API_JSON_INTERNAL_ERR;
    }
    cJSON_AddNumberToObject(out, "data_type", data_type);
    req->out = out;
    return API_JSON_OK;
}

/* ------------------------------------------------------------------ */
/* Module dispatch / registration                                     */
/* ------------------------------------------------------------------ */
static int on_json_req(uint16_t cmd, api_json_req_t *req, api_json_module_async_t *async)
{
    int status;
    cJSON *out;

    (void)async;

    if (req == NULL || req->in == NULL) {
        return API_JSON_BAD_REQUEST;
    }

    status = uart_json_validate_sub_mod(req->in);
    if (status != API_JSON_OK) {
        return status;
    }

    switch (cmd) {
    case UART_JSON_GET_AVAILABLE_NUMS:
        out = uart_json_new_response(cmd);
        if (out == NULL) {
            return API_JSON_INTERNAL_ERR;
        }
        cJSON_AddNumberToObject(out, "num", uart_service_instance_count());
        req->out = out;
        return API_JSON_OK;

    case UART_JSON_GET_BAUD:
        return uart_json_get_baud(req, cmd);
    case UART_JSON_SET_BAUD:
        return uart_json_set_baud(req);
    case UART_JSON_GET_CONFIG:
        return uart_json_get_config_all(req, cmd);
    case UART_JSON_SET_CONFIG:
        return uart_json_set_config(req);
    case UART_JSON_GET_FLOW_CTRL:
        return uart_json_get_flow_ctrl(req, cmd);
    case UART_JSON_SET_FLOW_CTRL:
        return uart_json_set_flow_ctrl(req);
    case UART_JSON_GET_PINS_NUM:
    case UART_JSON_SET_PINS_NUM:
    case UART_JSON_GET_MODE:
    case UART_JSON_SET_MODE:
        return uart_json_unsupported_echo(req, cmd);
    case UART_JSON_GET_STATUS:
        return uart_json_get_status(req);
    case UART_JSON_SET_STATUS:
        return uart_json_set_status(req);
    case UART_JSON_GET_DATA_TYPE:
        return uart_json_get_data_type(req);
    case UART_JSON_SET_DATA_TYPE:
        return uart_json_set_data_type(req);
    case UART_JSON_GET_DEFAULT_NUM:
        out = uart_json_new_response(cmd);
        if (out == NULL) {
            return API_JSON_INTERNAL_ERR;
        }
        cJSON_AddNumberToObject(out, "num", uart_service_default_instance());
        req->out = out;
        return API_JSON_OK;

    default:
        ESP_LOGI(TAG, "cmd %u not supported", (unsigned)cmd);
        return API_JSON_UNSUPPORTED_CMD;
    }
}

static int uart_api_json_init(api_json_module_cfg_t *cfg)
{
    cfg->on_req = on_json_req;
    cfg->module_id = UART_SERVICE_MODULE_ID;
    return 0;
}

API_JSON_MODULE_REGISTER(uart_api_json_init)
