/*
 * SPDX-License-Identifier: MIT
 *
 * Thin CMSIS-DAP command layer used by the target flash engine.
 *
 * All functions drive components/DAP through DAP_ExecuteCommand() and are only
 * called while the flash engine owns the DAP exclusively (see
 * wt_target_flash.h); request/response staging buffers are therefore static
 * and do not grow the calling task's stack.
 */
#ifndef TF_DAP_H
#define TF_DAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "tf_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One item of a DAP_Transfer batch: request byte + optional write data word. */
typedef struct {
    uint8_t request;
    uint32_t data;
} tf_dap_item_t;

/** DAP_Connect(SWD) - returns the connected port byte. */
esp_err_t tf_dap_connect_swd(uint8_t *port_out);

/** DAP_Disconnect(). */
esp_err_t tf_dap_disconnect(void);

/** DAP_SWJ_Sequence: drive `count_bits` (1..256) TMS/SWDIO bits. */
esp_err_t tf_dap_swj_sequence(uint32_t count_bits, const uint8_t *data);

/** DAP_SWJ_Clock: request the SWD/JTAG clock in Hz. */
esp_err_t tf_dap_swj_clock(uint32_t hz);

/** DAP_SWD_Configure: turnaround cycles (1..4) and data phase. */
esp_err_t tf_dap_swd_configure(uint8_t turnaround_cycles, bool data_phase);

/** DAP_TransferConfigure: idle cycles + WAIT retry + match retry. */
esp_err_t tf_dap_transfer_configure(uint8_t idle_cycles, uint16_t retry_count,
                                    uint16_t match_retry);

/** DAP_SWJ_Pins: set/select pins and wait up to `wait_us` microseconds. */
esp_err_t tf_dap_swj_pins(uint8_t value, uint8_t select, uint32_t wait_us);

/**
 * @brief DAP_Transfer batch.
 *
 * @param items      request items (read or write); write items carry data.
 * @param item_count number of items (1..255 and request must fit one packet).
 * @param read_data  receives one word per read item (items with request bit1).
 * @param read_count in: capacity of read_data; out: words written.
 * @return ESP_OK only when the DAP response status is DAP_TRANSFER_OK and the
 *         response layout matches the request.
 */
esp_err_t tf_dap_transfer(const tf_dap_item_t *items, size_t item_count,
                          uint32_t *read_data, size_t *read_count);

/**
 * @brief DAP_TransferBlock batch (auto-incrementing AP/DP access).
 *
 * @param request    request byte (e.g. TF_REQ_AP_READ(TF_AP_DRW)).
 * @param word_count number of 32-bit words (limited by the DAP packet size).
 * @param data       read: output words; write: input words.
 * @param is_read    true for a read block, false for a write block.
 */
esp_err_t tf_dap_transfer_block(uint8_t request, uint32_t word_count,
                                uint32_t *data, bool is_read);

#ifdef __cplusplus
}
#endif

#endif /* TF_DAP_H */
