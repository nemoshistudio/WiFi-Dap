/**
 * @file dataflow.h
 * @brief WiFi Dap — minimal DATA_FLOW model (module 2, WP-204)
 *
 * Minimal implementation of docs/protocol.md §2.3: one instance (the UART
 * service instance), attach records and the default data type.  The Web UI
 * (D) only depends on GET_DEFAULT_NUM + ATTACH_CUR_TO_SENDER to start the UART
 * data flow, so every attach operation must succeed.
 *
 * The actual data plane lives in:
 *   - components/uart_service        (UART RX/TX owner, binary module 4)
 *   - project_components/web_server/uri_modules/uri_bin_router.c
 *                                    (RAW -> UART TX, UART RX -> RAW_BROADCAST)
 *
 * JSON module ownership: components/dataflow/src/dataflow_api_json.c
 * (API_JSON_MODULE_REGISTER, whole-archive).
 */
#ifndef DATAFLOW_H_GUARD
#define DATAFLOW_H_GUARD

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JSON module id (docs/protocol.md §2). */
#define DATA_FLOW_MODULE_ID 2

/* DATA_FLOW(2) command ids (docs/protocol.md §2.3). */
typedef enum dataflow_cmd_e {
    DATAFLOW_GET_INS_LIST = 1,
    DATAFLOW_GET_CUR_INS = 2,
    DATAFLOW_GET_CUR_ATTACH_LIST = 3,
    DATAFLOW_GET_ATTACH_LIST = 4,
    DATAFLOW_ATTACH = 5,
    DATAFLOW_ATTACH_CUR_TO_RECVER = 6,
    DATAFLOW_ATTACH_CUR_TO_SENDER = 7,
    DATAFLOW_DETACH_SINGLE = 8,
    DATAFLOW_DETACH_CUR_FROM = 9,
    DATAFLOW_SET_DATA_TYPE = 10,
} dataflow_cmd_e;

/* mod_type reported by GET_INS_LIST: 130 (0x82) = UART, per docs/protocol.md. */
#define DATAFLOW_UART_MOD_TYPE 0x82

/* Default binary data type: WT_DATA_RAW (docs/protocol.md §3). */
#define DATAFLOW_DATA_TYPE_RAW 0x20

#define DATAFLOW_MAX_ATTACH       4
#define DATAFLOW_DEFAULT_INSTANCE 0

/** One attach record (GET_ATTACH_LIST / GET_CUR_ATTACH_LIST). */
typedef struct dataflow_attach_t {
    uint8_t attach_idx;
    uint8_t s_ins_idx; /**< sender instance */
    uint8_t r_ins_idx; /**< receiver instance */
    uint8_t data_type; /**< WT_DATA_RAW */
} dataflow_attach_t;

/** Number of protocol-visible instances (1: the UART instance). */
int dataflow_instance_count(void);

/** Default/current instance index (0). */
int dataflow_default_instance(void);

/** Peripheral number reported for the UART instance (UART port number). */
int dataflow_uart_periph_num(void);

/** Current default data type (WT_DATA_RAW). */
uint8_t dataflow_get_data_type(void);

/** Set the default data type (WT_DATA_RAW only for now). */
void dataflow_set_data_type(uint8_t data_type);

/** Add (or reuse) an attach record; returns its attach_idx, or 0xFF on error. */
uint8_t dataflow_attach_add(uint8_t s_ins_idx, uint8_t r_ins_idx, uint8_t data_type);

/** Remove an attach record; returns 0 on success, -1 when unknown. */
int dataflow_attach_del(uint8_t attach_idx);

/** Copy up to max records into out; returns the number of records copied. */
int dataflow_attach_list(dataflow_attach_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DATAFLOW_H_GUARD */
