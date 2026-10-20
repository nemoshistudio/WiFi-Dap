/*
 * SPDX-License-Identifier: MIT
 *
 * CMSIS-DAP command layer for the target flash engine.
 */
#include <string.h>

#include <esp_log.h>

#include "cmsis-dap/include/DAP.h"

#include "tf_internal.h"

#include "tf_dap.h"

#define DAP_TRANSFER_STATUS_OK 0x01U
#define DAP_TRANSFER_MAX_COUNT 255U
/* TransferBlock count caps enforced by components/DAP (512-byte packets):
 * read: echo+3+4*n <= 512 -> n <= 127; write: echo+4+4*n <= 512 -> n <= 126. */
#define TF_BLOCK_READ_MAX  127U
#define TF_BLOCK_WRITE_MAX 126U

/*
 * Request/response staging. The flash engine owns the DAP engine exclusively
 * for the duration of an operation (wt_target_flash.h), so a single static
 * packet pair is safe and keeps the 4 KB caller stacks free.
 */
static uint8_t s_req[TF_DAP_PACKET_SIZE];
static uint8_t s_resp[TF_DAP_PACKET_SIZE];

static uint32_t s_resp_len;

uint8_t tf_dap_last_status;
uint8_t tf_dap_last_count;

/** Run the staged request through DAP_ExecuteCommand and validate the echo. */
static esp_err_t tf_dap_exec(uint32_t req_len)
{
    uint32_t ret;

    if (req_len == 0U || req_len > TF_DAP_PACKET_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(s_resp, 0, sizeof(s_resp));
    TF_STAGE(70U);
    ret = DAP_ExecuteCommand(s_req, s_resp);
    TF_STAGE(71U);
    s_resp_len = ret & 0xFFFFU;

    if (s_resp_len < 2U || s_resp_len > TF_DAP_PACKET_SIZE) {
        ESP_LOGE(TF_TAG, "DAP cmd 0x%02X: invalid response length %u",
                 s_req[0], (unsigned)s_resp_len);
        return ESP_FAIL;
    }
    if (s_resp[0] != s_req[0]) {
        ESP_LOGE(TF_TAG, "DAP cmd 0x%02X: response echo 0x%02X mismatch",
                 s_req[0], s_resp[0]);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Check the one-byte status returned by simple DAP commands. */
static esp_err_t tf_dap_check_status(uint8_t status)
{
    if (status != DAP_OK) {
        ESP_LOGW(TF_TAG, "DAP command failed, status=%u", (unsigned)status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tf_dap_connect_swd(uint8_t *port_out)
{
    uint8_t port;

    s_req[0] = ID_DAP_Connect;
    s_req[1] = DAP_PORT_SWD;
    if (tf_dap_exec(2) != ESP_OK) {
        return ESP_FAIL;
    }
    port = s_resp[1];
    if (port != DAP_PORT_SWD) {
        ESP_LOGE(TF_TAG, "DAP_Connect: SWD not available (port=0x%02X)", port);
        return ESP_FAIL;
    }
    if (port_out != NULL) {
        *port_out = port;
    }
    return ESP_OK;
}

esp_err_t tf_dap_disconnect(void)
{
    s_req[0] = ID_DAP_Disconnect;
    if (tf_dap_exec(1) != ESP_OK) {
        return ESP_FAIL;
    }
    return tf_dap_check_status(s_resp[1]);
}

esp_err_t tf_dap_swj_sequence(uint32_t count_bits, const uint8_t *data)
{
    uint32_t bytes;
    uint8_t count_byte;

    if (count_bits == 0U || count_bits > 256U || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bytes = (count_bits + 7U) / 8U;
    if (bytes > 32U) {
        return ESP_ERR_INVALID_ARG;
    }
    count_byte = (count_bits >= 256U) ? 0U : (uint8_t)count_bits; /* 0 => 256 */

    s_req[0] = ID_DAP_SWJ_Sequence;
    s_req[1] = count_byte;
    memcpy(&s_req[2], data, bytes);
    if (tf_dap_exec(2U + bytes) != ESP_OK) {
        return ESP_FAIL;
    }
    return tf_dap_check_status(s_resp[1]);
}

esp_err_t tf_dap_swj_clock(uint32_t hz)
{
    s_req[0] = ID_DAP_SWJ_Clock;
    s_req[1] = (uint8_t)(hz >> 0);
    s_req[2] = (uint8_t)(hz >> 8);
    s_req[3] = (uint8_t)(hz >> 16);
    s_req[4] = (uint8_t)(hz >> 24);
    if (tf_dap_exec(5) != ESP_OK) {
        return ESP_FAIL;
    }
    return tf_dap_check_status(s_resp[1]);
}

esp_err_t tf_dap_swd_configure(uint8_t turnaround_cycles, bool data_phase)
{
    uint8_t cfg;

    if (turnaround_cycles < 1U || turnaround_cycles > 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg = (uint8_t)((turnaround_cycles - 1U) & 0x03U);
    if (data_phase) {
        cfg |= 0x04U;
    }
    s_req[0] = ID_DAP_SWD_Configure;
    s_req[1] = cfg;
    if (tf_dap_exec(2) != ESP_OK) {
        return ESP_FAIL;
    }
    return tf_dap_check_status(s_resp[1]);
}

esp_err_t tf_dap_transfer_configure(uint8_t idle_cycles, uint16_t retry_count,
                                    uint16_t match_retry)
{
    s_req[0] = ID_DAP_TransferConfigure;
    s_req[1] = idle_cycles;
    s_req[2] = (uint8_t)(retry_count >> 0);
    s_req[3] = (uint8_t)(retry_count >> 8);
    s_req[4] = (uint8_t)(match_retry >> 0);
    s_req[5] = (uint8_t)(match_retry >> 8);
    if (tf_dap_exec(6) != ESP_OK) {
        return ESP_FAIL;
    }
    return tf_dap_check_status(s_resp[1]);
}

esp_err_t tf_dap_swj_pins(uint8_t value, uint8_t select, uint32_t wait_us)
{
    s_req[0] = ID_DAP_SWJ_Pins;
    s_req[1] = value;
    s_req[2] = select;
    s_req[3] = (uint8_t)(wait_us >> 0);
    s_req[4] = (uint8_t)(wait_us >> 8);
    s_req[5] = (uint8_t)(wait_us >> 16);
    s_req[6] = (uint8_t)(wait_us >> 24);
    if (tf_dap_exec(7) != ESP_OK) {
        return ESP_FAIL;
    }
    if (s_resp_len != 2U) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tf_dap_transfer(const tf_dap_item_t *items, size_t item_count,
                          uint32_t *read_data, size_t *read_count)
{
    uint32_t req_len = 3U;            /* cmd + index + count */
    size_t read_items = 0;
    size_t read_cap = (read_count != NULL) ? *read_count : 0U;
    size_t out_words = 0;
    uint8_t status;
    uint8_t resp_count;
    uint32_t offset;
    size_t i;

    if (items == NULL || item_count == 0U || item_count > DAP_TRANSFER_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < item_count; i++) {
        size_t add = 1U + (((items[i].request & 0x02U) != 0U) ? 0U : 4U);
        if (req_len + add > TF_DAP_PACKET_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        req_len += (uint32_t)add;
        if ((items[i].request & 0x02U) != 0U) {
            read_items++;
        }
    }
    if (read_items > read_cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_req[0] = ID_DAP_Transfer;
    s_req[1] = 0U;                    /* DAP index */
    s_req[2] = (uint8_t)item_count;
    offset = 3U;
    for (i = 0; i < item_count; i++) {
        s_req[offset++] = items[i].request;
        if ((items[i].request & 0x02U) == 0U) {
            s_req[offset++] = (uint8_t)(items[i].data >> 0);
            s_req[offset++] = (uint8_t)(items[i].data >> 8);
            s_req[offset++] = (uint8_t)(items[i].data >> 16);
            s_req[offset++] = (uint8_t)(items[i].data >> 24);
        }
    }

    if (tf_dap_exec(req_len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (s_resp_len < 3U) {
        return ESP_FAIL;
    }
    resp_count = s_resp[1];
    status = s_resp[2];
    tf_dap_last_status = status;
    tf_dap_last_count = resp_count;

    if (status != DAP_TRANSFER_STATUS_OK) {
        ESP_LOGW(TF_TAG, "DAP_Transfer status=0x%02X (count=%u/%u)",
                 status, resp_count, (unsigned)item_count);
        return ESP_FAIL;
    }
    if (resp_count != (uint8_t)item_count) {
        ESP_LOGW(TF_TAG, "DAP_Transfer completed %u/%u items",
                 resp_count, (unsigned)item_count);
        return ESP_FAIL;
    }
    if (s_resp_len != 3U + 4U * read_items) {
        ESP_LOGW(TF_TAG, "DAP_Transfer response length %u, expected %u",
                 (unsigned)s_resp_len, (unsigned)(3U + 4U * read_items));
        return ESP_FAIL;
    }

    offset = 3U;
    for (i = 0; i < item_count; i++) {
        if ((items[i].request & 0x02U) != 0U) {
            read_data[out_words++] = (uint32_t)s_resp[offset + 0] |
                                     ((uint32_t)s_resp[offset + 1] << 8) |
                                     ((uint32_t)s_resp[offset + 2] << 16) |
                                     ((uint32_t)s_resp[offset + 3] << 24);
            offset += 4U;
        }
    }
    if (read_count != NULL) {
        *read_count = out_words;
    }
    return ESP_OK;
}

esp_err_t tf_dap_transfer_block(uint8_t request, uint32_t word_count,
                                uint32_t *data, bool is_read)
{
    uint32_t req_len;
    uint32_t i;
    uint32_t count;
    uint8_t status;

    if (data == NULL || word_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_read) {
        if (word_count > TF_BLOCK_READ_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        req_len = 5U;
    } else {
        if (word_count > TF_BLOCK_WRITE_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        req_len = 5U + 4U * word_count;
        if (req_len > TF_DAP_PACKET_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    s_req[0] = ID_DAP_TransferBlock;
    s_req[1] = 0U;                          /* DAP index */
    s_req[2] = (uint8_t)(word_count >> 0);
    s_req[3] = (uint8_t)(word_count >> 8);
    s_req[4] = request;
    if (!is_read) {
        for (i = 0; i < word_count; i++) {
            uint32_t off = 5U + 4U * i;
            s_req[off + 0] = (uint8_t)(data[i] >> 0);
            s_req[off + 1] = (uint8_t)(data[i] >> 8);
            s_req[off + 2] = (uint8_t)(data[i] >> 16);
            s_req[off + 3] = (uint8_t)(data[i] >> 24);
        }
    }

    if (tf_dap_exec(req_len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (s_resp_len < 4U) {
        return ESP_FAIL;
    }
    status = s_resp[3];
    count = (uint32_t)s_resp[1] | ((uint32_t)s_resp[2] << 8);
    tf_dap_last_status = status;
    tf_dap_last_count = (uint8_t)count;
    if (status != DAP_TRANSFER_STATUS_OK) {
        ESP_LOGW(TF_TAG, "DAP_TransferBlock status=0x%02X (count=%u/%u)",
                 status, (unsigned)count, (unsigned)word_count);
        return ESP_FAIL;
    }
    if (count != word_count) {
        ESP_LOGW(TF_TAG, "DAP_TransferBlock completed %u/%u words",
                 (unsigned)count, (unsigned)word_count);
        return ESP_FAIL;
    }
    if (is_read) {
        if (s_resp_len != 4U + 4U * word_count) {
            return ESP_FAIL;
        }
        for (i = 0; i < word_count; i++) {
            uint32_t off = 4U + 4U * i;
            data[i] = (uint32_t)s_resp[off + 0] |
                      ((uint32_t)s_resp[off + 1] << 8) |
                      ((uint32_t)s_resp[off + 2] << 16) |
                      ((uint32_t)s_resp[off + 3] << 24);
        }
    }
    return ESP_OK;
}
