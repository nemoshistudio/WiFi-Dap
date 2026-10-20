/**
 * @file dataflow.c
 * @brief Minimal DATA_FLOW state (module 2, WP-204) — docs/protocol.md §2.3
 *
 * Holds the single UART instance identity, the attach records and the default
 * data type.  The real data plane is owned by components/uart_service and the
 * WS binary router (uri_bin_router.c).
 */
#include "sdkconfig.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "dataflow.h"
#include "uart_service.h"

/* The JSON/binary control planes may run in different tasks (httpd/WS), so the
 * small state below is protected by a portMUX critical section. */
static portMUX_TYPE s_dataflow_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_data_type = DATAFLOW_DATA_TYPE_RAW;
static dataflow_attach_t s_attaches[DATAFLOW_MAX_ATTACH];
static uint8_t s_attach_count = 0;
static uint8_t s_next_attach_idx = 0;

int dataflow_instance_count(void)
{
    return 1;
}

int dataflow_default_instance(void)
{
    return DATAFLOW_DEFAULT_INSTANCE;
}

int dataflow_uart_periph_num(void)
{
    /* The only DATA_FLOW instance is the UART service instance. */
    return (int)uart_service_port();
}

uint8_t dataflow_get_data_type(void)
{
    uint8_t data_type;

    portENTER_CRITICAL(&s_dataflow_mux);
    data_type = s_data_type;
    portEXIT_CRITICAL(&s_dataflow_mux);
    return data_type;
}

void dataflow_set_data_type(uint8_t data_type)
{
    portENTER_CRITICAL(&s_dataflow_mux);
    s_data_type = data_type;
    portEXIT_CRITICAL(&s_dataflow_mux);
}

uint8_t dataflow_attach_add(uint8_t s_ins_idx, uint8_t r_ins_idx, uint8_t data_type)
{
    uint8_t attach_idx = 0xFF;

    portENTER_CRITICAL(&s_dataflow_mux);

    /* Update the existing record for the same sender/receiver pair instead of
     * growing the table (the Web UI may attach repeatedly on reconnect). */
    for (uint8_t i = 0; i < s_attach_count; ++i) {
        if (s_attaches[i].s_ins_idx == s_ins_idx && s_attaches[i].r_ins_idx == r_ins_idx) {
            s_attaches[i].data_type = data_type;
            attach_idx = s_attaches[i].attach_idx;
            break;
        }
    }

    if (attach_idx == 0xFF && s_attach_count < DATAFLOW_MAX_ATTACH) {
        dataflow_attach_t *entry = &s_attaches[s_attach_count];

        entry->attach_idx = s_next_attach_idx++;
        entry->s_ins_idx = s_ins_idx;
        entry->r_ins_idx = r_ins_idx;
        entry->data_type = data_type;
        attach_idx = entry->attach_idx;
        s_attach_count++;
    }

    portEXIT_CRITICAL(&s_dataflow_mux);
    return attach_idx;
}

int dataflow_attach_del(uint8_t attach_idx)
{
    int found = -1;

    portENTER_CRITICAL(&s_dataflow_mux);
    for (uint8_t i = 0; i < s_attach_count; ++i) {
        if (s_attaches[i].attach_idx == attach_idx) {
            /* Preserve order: shift the remaining entries down. */
            for (uint8_t j = i + 1; j < s_attach_count; ++j) {
                s_attaches[j - 1] = s_attaches[j];
            }
            s_attach_count--;
            memset(&s_attaches[s_attach_count], 0, sizeof(s_attaches[s_attach_count]));
            found = 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_dataflow_mux);

    return found;
}

int dataflow_attach_list(dataflow_attach_t *out, int max)
{
    int count;

    if (out == NULL || max <= 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_dataflow_mux);
    count = s_attach_count < max ? s_attach_count : max;
    for (int i = 0; i < count; ++i) {
        out[i] = s_attaches[i];
    }
    portEXIT_CRITICAL(&s_dataflow_mux);

    return count;
}
