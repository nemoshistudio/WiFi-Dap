/*
 * SPDX-License-Identifier: MIT
 *
 * SW-DP / AHB-AP access layer for the target flash engine.
 *
 * Register/PPB accesses are memory-mapped through the Cortex-M AHB-AP:
 *   TAR = address, then DRW accesses (the AP auto-increments the address).
 * Block reads use DAP_TransferBlock with TF_REQ_AP_READ(TF_AP_DRW); block
 * writes are not used for flash programming (per-program-unit BSY polling is
 * required), only single-word register writes.
 */
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_timer.h>

#include "tf_dap.h"
#include "tf_swd.h"

#define TF_SWD_WORD_CHUNK   120U        /* words per block read, < 1 KiB wrap */
#define TF_CTRL_STAT_PWR_MASK 0xA0000000U
#define TF_CTRL_STAT_PWR_REQ  0x50000000U
#define TF_DP_ABORT_CLEAR     0x0000001EU

uint64_t tf_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void tf_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1U);
}

static esp_err_t tf_swd_read_dp(uint32_t reg, uint32_t *value)
{
    tf_dap_item_t item = {.request = TF_REQ_DP_READ(reg), .data = 0U};
    uint32_t data[1];
    size_t count = 1U;

    if (tf_dap_transfer(&item, 1U, data, &count) != ESP_OK || count != 1U) {
        return ESP_FAIL;
    }
    *value = data[0];
    return ESP_OK;
}

static esp_err_t tf_swd_write_dp(uint32_t reg, uint32_t value)
{
    tf_dap_item_t item = {.request = TF_REQ_DP_WRITE(reg), .data = value};
    return tf_dap_transfer(&item, 1U, NULL, NULL);
}

static esp_err_t tf_swd_write_ap(uint32_t reg, uint32_t value)
{
    tf_dap_item_t item = {.request = TF_REQ_AP_WRITE(reg), .data = value};
    return tf_dap_transfer(&item, 1U, NULL, NULL);
}

esp_err_t tf_swd_read_u32(uint32_t addr, uint32_t *value)
{
    tf_dap_item_t items[2];
    uint32_t data[1];
    size_t count = 1U;

    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    items[0].request = TF_REQ_AP_WRITE(TF_AP_TAR);
    items[0].data = addr;
    items[1].request = TF_REQ_AP_READ(TF_AP_DRW);
    items[1].data = 0U;

    if (tf_dap_transfer(items, 2U, data, &count) != ESP_OK || count != 1U) {
        ESP_LOGE(TF_TAG, "read32 0x%08X failed", (unsigned)addr);
        return ESP_FAIL;
    }
    *value = data[0];
    return ESP_OK;
}

esp_err_t tf_swd_write_u32(uint32_t addr, uint32_t value)
{
    tf_dap_item_t items[2];

    items[0].request = TF_REQ_AP_WRITE(TF_AP_TAR);
    items[0].data = addr;
    items[1].request = TF_REQ_AP_WRITE(TF_AP_DRW);
    items[1].data = value;

    if (tf_dap_transfer(items, 2U, NULL, NULL) != ESP_OK) {
        ESP_LOGE(TF_TAG, "write32 0x%08X<=0x%08X failed",
                 (unsigned)addr, (unsigned)value);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tf_swd_wait_clear(uint32_t addr, uint32_t mask, uint32_t timeout_ms)
{
    uint64_t deadline = tf_now_ms() + timeout_ms;
    uint32_t value = 0U;

    do {
        if (tf_swd_read_u32(addr, &value) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((value & mask) == 0U) {
            return ESP_OK;
        }
    } while (tf_now_ms() < deadline);

    ESP_LOGE(TF_TAG, "timeout waiting 0x%08X & 0x%08X == 0 (last=0x%08X)",
             (unsigned)addr, (unsigned)mask, (unsigned)value);
    return ESP_ERR_TIMEOUT;
}

esp_err_t tf_swd_wait_set(uint32_t addr, uint32_t mask, uint32_t timeout_ms)
{
    uint64_t deadline = tf_now_ms() + timeout_ms;
    uint32_t value = 0U;

    do {
        if (tf_swd_read_u32(addr, &value) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((value & mask) == mask) {
            return ESP_OK;
        }
    } while (tf_now_ms() < deadline);

    ESP_LOGE(TF_TAG, "timeout waiting 0x%08X & 0x%08X == mask (last=0x%08X)",
             (unsigned)addr, (unsigned)mask, (unsigned)value);
    return ESP_ERR_TIMEOUT;
}

esp_err_t tf_swd_clear_error_bits(uint32_t addr, uint32_t bits)
{
    if (bits == 0U) {
        return ESP_OK;
    }
    /* Write-1-to-clear error flags. */
    return tf_swd_write_u32(addr, bits);
}

static esp_err_t tf_swd_read_words(uint32_t addr, uint32_t word_count, uint32_t *words)
{
    uint32_t done = 0U;

    while (done < word_count) {
        uint32_t chunk = word_count - done;
        if (chunk > TF_SWD_WORD_CHUNK) {
            chunk = TF_SWD_WORD_CHUNK;
        }
        if (tf_swd_write_ap(TF_AP_TAR, addr + done * 4U) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_dap_transfer_block(TF_REQ_AP_READ(TF_AP_DRW), chunk,
                                  &words[done], true) != ESP_OK) {
            ESP_LOGE(TF_TAG, "block read 0x%08X/%u failed",
                     (unsigned)(addr + done * 4U), (unsigned)chunk);
            return ESP_FAIL;
        }
        done += chunk;
    }
    return ESP_OK;
}

esp_err_t tf_swd_read_mem(uint32_t addr, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t words[TF_SWD_WORD_CHUNK];

    if (buf == NULL && len != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    while (len > 0U) {
        if (((addr & 0x03U) == 0U) && (len >= 4U)) {
            uint32_t count = (uint32_t)(len / 4U);
            if (count > TF_SWD_WORD_CHUNK) {
                count = TF_SWD_WORD_CHUNK;
            }
            if (tf_swd_read_words(addr, count, words) != ESP_OK) {
                return ESP_FAIL;
            }
            memcpy(out, words, count * 4U);
            out += count * 4U;
            addr += count * 4U;
            len -= count * 4U;
        } else {
            uint32_t value;
            uint32_t skip = addr & 0x03U;
            size_t take = 4U - skip;

            if (tf_swd_read_u32(addr & ~0x03U, &value) != ESP_OK) {
                return ESP_FAIL;
            }
            if (take > len) {
                take = len;
            }
            memcpy(out, ((const uint8_t *)&value) + skip, take);
            out += take;
            addr += (uint32_t)take;
            len -= take;
        }
    }
    return ESP_OK;
}

esp_err_t tf_swd_halt(void)
{
    uint64_t deadline;
    uint32_t dhcsr = 0U;

    if (tf_swd_write_u32(TF_REG_DHCSR,
                         TF_DHCSR_DBGKEY | TF_DHCSR_C_DEBUGEN | TF_DHCSR_C_HALT) != ESP_OK) {
        return ESP_FAIL;
    }
    deadline = tf_now_ms() + 1000U;
    do {
        if (tf_swd_read_u32(TF_REG_DHCSR, &dhcsr) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((dhcsr & TF_DHCSR_S_HALT) != 0U) {
            return ESP_OK;
        }
    } while (tf_now_ms() < deadline);

    ESP_LOGE(TF_TAG, "core did not halt (DHCSR=0x%08X)", (unsigned)dhcsr);
    return ESP_ERR_TIMEOUT;
}

esp_err_t tf_swd_resume(void)
{
    if (tf_swd_write_u32(TF_REG_DHCSR, TF_DHCSR_DBGKEY | TF_DHCSR_C_DEBUGEN) != ESP_OK) {
        return ESP_FAIL;
    }
    tf_delay_ms(10);
    return ESP_OK;
}

esp_err_t tf_swd_is_halted(bool *halted)
{
    uint32_t dhcsr;

    if (halted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tf_swd_read_u32(TF_REG_DHCSR, &dhcsr) != ESP_OK) {
        return ESP_FAIL;
    }
    *halted = (dhcsr & TF_DHCSR_S_HALT) != 0U;
    return ESP_OK;
}

esp_err_t tf_swd_sysreset(bool run)
{
    if (tf_swd_write_u32(TF_REG_AIRCR, TF_AIRCR_SYSRESETREQ) != ESP_OK) {
        return ESP_FAIL;
    }
    tf_delay_ms(20);

    if (run) {
        /* Make sure the core is not held in halt after the reset. */
        if (tf_swd_write_u32(TF_REG_DHCSR,
                             TF_DHCSR_DBGKEY | TF_DHCSR_C_DEBUGEN) != ESP_OK) {
            return ESP_FAIL;
        }
        tf_delay_ms(10);
        return ESP_OK;
    }

    return tf_swd_halt();
}

esp_err_t tf_swd_read_pc(uint32_t *pc)
{
    uint32_t dhcsr = 0U;
    uint64_t deadline;

    if (pc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Ensure the core is halted and debug is enabled. */
    if (tf_swd_halt() != ESP_OK) {
        return ESP_FAIL;
    }
    /* DCRSR: REGWnR=0 (read), REGSEL=PC. */
    if (tf_swd_write_u32(TF_REG_DCRSR, TF_DCRSR_REGSEL_PC) != ESP_OK) {
        return ESP_FAIL;
    }
    deadline = tf_now_ms() + 200U;
    do {
        if (tf_swd_read_u32(TF_REG_DHCSR, &dhcsr) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((dhcsr & TF_DHCSR_S_REGRDY) != 0U) {
            return tf_swd_read_u32(TF_REG_DCRDR, pc);
        }
    } while (tf_now_ms() < deadline);

    return ESP_ERR_TIMEOUT;
}

esp_err_t tf_swd_link_init(uint32_t *dpidr)
{
    static const uint8_t line_reset[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t jtag_to_swd[2] = {0x9E, 0xE7}; /* 0xE79E, LSB first */
    static const uint8_t idle[1] = {0x00};
    uint32_t idcode = 0U;

    if (tf_dap_connect_swd(NULL) != ESP_OK) {
        return ESP_FAIL;
    }
    /* Line reset, JTAG-to-SWD switch sequence, line reset, idle cycles. */
    if (tf_dap_swj_sequence(51U, line_reset) != ESP_OK ||
        tf_dap_swj_sequence(16U, jtag_to_swd) != ESP_OK ||
        tf_dap_swj_sequence(51U, line_reset) != ESP_OK ||
        tf_dap_swj_sequence(8U, idle) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_dap_swj_clock(TF_SWD_CLOCK_HZ) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_dap_swd_configure(1U, false) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_dap_transfer_configure(0U, 1000U, 100U) != ESP_OK) {
        return ESP_FAIL;
    }
    /* Release nRESET before touching the target. */
    (void)tf_dap_swj_pins(0x80U, 0x80U, 0U);

    /* Clear sticky DP errors, then request debug+system power-up. */
    if (tf_swd_write_dp(TF_DP_ABORT, TF_DP_ABORT_CLEAR) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_write_dp(TF_DP_CTRL_STAT, TF_CTRL_STAT_PWR_REQ) != ESP_OK) {
        return ESP_FAIL;
    }
    /* Wait for the DP power-up ACKs (DP CTRL/STAT is a DP register, not
     * memory-mapped, so read it directly over the DP, not through the AP). */
    {
        uint32_t dp_ctrl = 0U;
        uint64_t deadline = tf_now_ms() + 500U;
        do {
            /* DP CTRL/STAT is not memory-mapped: read through the DP. */
            tf_dap_item_t item = {.request = TF_REQ_DP_READ(TF_DP_CTRL_STAT), .data = 0U};
            uint32_t data[1];
            size_t count = 1U;
            if (tf_dap_transfer(&item, 1U, data, &count) != ESP_OK || count != 1U) {
                return ESP_FAIL;
            }
            dp_ctrl = data[0];
            if ((dp_ctrl & TF_CTRL_STAT_PWR_MASK) == TF_CTRL_STAT_PWR_MASK) {
                break;
            }
        } while (tf_now_ms() < deadline);
        if ((dp_ctrl & TF_CTRL_STAT_PWR_MASK) != TF_CTRL_STAT_PWR_MASK) {
            ESP_LOGE(TF_TAG, "DP power-up failed (CTRL/STAT=0x%08X)", (unsigned)dp_ctrl);
            return ESP_ERR_TIMEOUT;
        }
    }
    /* SELECT = AP0/bank0, then configure the AHB-AP for 32-bit incrementing. */
    if (tf_swd_write_dp(TF_DP_SELECT, 0U) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_write_ap(TF_AP_CSW, TF_AP_CSW_32BIT_INC) != ESP_OK) {
        return ESP_FAIL;
    }
    /* DPIDR (IDCODE) */
    if (tf_swd_read_dp(0x00U, &idcode) != ESP_OK) {
        return ESP_FAIL;
    }
    if (dpidr != NULL) {
        *dpidr = idcode;
    }
    return ESP_OK;
}
