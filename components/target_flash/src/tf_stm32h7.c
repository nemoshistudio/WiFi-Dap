/*
 * SPDX-License-Identifier: MIT
 *
 * STM32H7 (Cortex-M7) internal-flash algorithm for the target flash engine.
 *
 * Register reference: RM0433 (STM32H742/743/750/753, FLASH at 0x52002000),
 * also H723/725/730/733 (DEVID 0x483, single bank).  Bank registers:
 *   bank1 base 0x52002000, bank2 base 0x52002100, same layout per bank:
 *   KEYR +0x04, CR +0x0C, SR +0x10, CCR/ICR +0x14.
 *   CR: LOCK(0) PG(1) SER(2) BER(3) PSIZE(5:4) FW(6) START(7) SNB(10:8).
 *   SR: BSY(0) QW(2) WRPERR(17) PGSERR(18) STRBERR(19) INCERR(21) OPERR(22)
 *       RDPERR(23) RDSERR(24) SNECCERR(25) DBECCERR(26).
 *
 * The H7 flash word is 256 bits: programming writes 8 consecutive 32-bit words
 * starting at a 32-byte aligned address (flash word), then waits for the
 * operation queue (SR.QW) to drain and checks the error flags.
 *
 * Hardware status: implemented against RM0433/OpenOCD stm32h7x.c; the first
 * hardware of this work package is an STM32F407, so the H7 path is compiled and
 * unit-reviewed but marked "待真机" in the bring-up report.
 */
#include <string.h>

#include <esp_log.h>

#include "tf_dap.h"
#include "tf_swd.h"
#include "tf_internal.h"

#define H7_FLASH_BANK0_BASE  0x52002000U
#define H7_FLASH_BANK1_BASE  0x52002100U
#define H7_FLASH_BANK_SIZE   0x00100000U   /* 1 MiB per bank */
#define H7_BANK_ADDR0        0x08000000U
#define H7_BANK_ADDR1        0x08100000U
#define H7_REG_KEYR         0x04U
#define H7_REG_CR           0x0CU
#define H7_REG_SR           0x10U
#define H7_REG_CCR          0x14U

#define H7_CR_LOCK          (1U << 0)
#define H7_CR_PG            (1U << 1)
#define H7_CR_SER           (1U << 2)
#define H7_CR_PSIZE_64      (3U << 4)
#define H7_CR_START         (1U << 7)
#define H7_CR_SNB(n)        (((uint32_t)(n) & 0x7U) << 8)

#define H7_SR_BSY           (1U << 0)
#define H7_SR_QW            (1U << 2)
#define H7_SR_WRPERR        (1U << 17)
#define H7_SR_PGSERR        (1U << 18)
#define H7_SR_STRBERR       (1U << 19)
#define H7_SR_INCERR        (1U << 21)
#define H7_SR_OPERR         (1U << 22)
#define H7_SR_RDPERR        (1U << 23)
#define H7_SR_RDSERR        (1U << 24)
#define H7_SR_SNECCERR      (1U << 25)
#define H7_SR_DBECCERR      (1U << 26)
#define H7_SR_ERRORS        (H7_SR_WRPERR | H7_SR_PGSERR | H7_SR_STRBERR | \
                             H7_SR_INCERR | H7_SR_OPERR | H7_SR_RDPERR | \
                             H7_SR_RDSERR | H7_SR_SNECCERR | H7_SR_DBECCERR)

#define H7_KEY1             0x45670123U
#define H7_KEY2             0xCDEF89ABU

#define H7_SECTOR_SIZE      (128U * 1024U)
#define H7_PROGRAM_UNIT     32U           /* 256-bit flash word */
#define H7_BSY_TIMEOUT_MS   1000U
#define H7_ERASE_TIMEOUT_MS 10000U

static uint32_t h7_bank_base(uint32_t addr)
{
    return (addr >= H7_BANK_ADDR1) ? H7_FLASH_BANK1_BASE : H7_FLASH_BANK0_BASE;
}

static uint32_t h7_bank_snb(const tf_target_t *target, uint32_t addr)
{
    uint32_t bank_addr = (addr >= H7_BANK_ADDR1) ? H7_BANK_ADDR1 : H7_BANK_ADDR0;
    uint32_t offset = (addr - bank_addr);
    uint32_t sectors = H7_FLASH_BANK_SIZE / H7_SECTOR_SIZE;

    if (target->flash_size <= H7_FLASH_BANK_SIZE) {
        /* Single-bank part: all flash is mapped at 0x08000000. */
        offset = addr - H7_BANK_ADDR0;
        bank_addr = H7_BANK_ADDR0;
    }
    if (offset >= sectors * H7_SECTOR_SIZE) {
        return UINT32_MAX;
    }
    return offset / H7_SECTOR_SIZE;
}

static esp_err_t h7_unlock_bank(uint32_t flash_base)
{
    uint32_t cr = 0U;

    if (tf_swd_read_u32(flash_base + H7_REG_CR, &cr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((cr & H7_CR_LOCK) == 0U) {
        return ESP_OK;
    }
    if (tf_swd_write_u32(flash_base + H7_REG_KEYR, H7_KEY1) != ESP_OK ||
        tf_swd_write_u32(flash_base + H7_REG_KEYR, H7_KEY2) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_read_u32(flash_base + H7_REG_CR, &cr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((cr & H7_CR_LOCK) != 0U) {
        ESP_LOGE(TF_TAG, "H7: bank 0x%08X not unlocked (CR=0x%08X)",
                 (unsigned)flash_base, (unsigned)cr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t h7_lock_banks(void)
{
    esp_err_t err = ESP_OK;

    if (tf_swd_write_u32(H7_FLASH_BANK0_BASE + H7_REG_CR, H7_CR_LOCK) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (tf_swd_write_u32(H7_FLASH_BANK1_BASE + H7_REG_CR, H7_CR_LOCK) != ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

/** Read SR of the bank owning `addr`, clear error flags, report failures. */
static esp_err_t h7_sr_check(uint32_t bank, bool *had_error)
{
    uint32_t sr = 0U;

    *had_error = false;
    if (tf_swd_read_u32(bank + H7_REG_SR, &sr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((sr & H7_SR_ERRORS) != 0U) {
        ESP_LOGW(TF_TAG, "H7: SR error flags 0x%08X (bank 0x%08X)",
                 (unsigned)(sr & H7_SR_ERRORS), (unsigned)bank);
        /* SR/CCR are write-1-to-clear on H7; clear only the error flags. */
        (void)tf_swd_clear_error_bits(bank + H7_REG_CCR, sr & H7_SR_ERRORS);
        *had_error = true;
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t h7_wait_unit(uint32_t bank, bool *had_error)
{
    esp_err_t err;

    *had_error = false;
    err = tf_swd_wait_clear(bank + H7_REG_SR, H7_SR_BSY | H7_SR_QW, H7_BSY_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return h7_sr_check(bank, had_error);
}

static esp_err_t h7_program(tf_target_t *target, uint32_t addr, const uint8_t *data, size_t len)
{
    size_t offset = 0U;

    (void)target;
    if ((len % H7_PROGRAM_UNIT) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr % H7_PROGRAM_UNIT) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    while (offset < len) {
        uint32_t unit_addr = addr + (uint32_t)offset;
        uint32_t bank = h7_bank_base(unit_addr);
        tf_dap_item_t items[1U + H7_PROGRAM_UNIT / 4U];
        bool had_error = false;
        esp_err_t err;

        if (tf_swd_write_u32(bank + H7_REG_CR, H7_CR_PG | H7_CR_PSIZE_64) != ESP_OK) {
            return ESP_FAIL;
        }
        items[0].request = TF_REQ_AP_WRITE(TF_AP_TAR);
        items[0].data = unit_addr;
        for (uint32_t word = 0U; word < H7_PROGRAM_UNIT / 4U; word++) {
            const uint8_t *p = data + offset + word * 4U;
            items[1U + word].request = TF_REQ_AP_WRITE(TF_AP_DRW);
            items[1U + word].data = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        if (tf_dap_transfer(items, 1U + H7_PROGRAM_UNIT / 4U, NULL, NULL) != ESP_OK) {
            return ESP_FAIL;
        }
        err = h7_wait_unit(bank, &had_error);
        if (err != ESP_OK) {
            return err;
        }
        if (had_error) {
            return ESP_FAIL;
        }
        offset += H7_PROGRAM_UNIT;
    }
    return ESP_OK;
}

static esp_err_t h7_erase(tf_target_t *target, uint32_t addr, uint32_t size)
{
    uint32_t start = addr;
    uint32_t end = addr + size;
    size_t erased = 0U;

    while (start < end) {
        uint32_t bank_addr = (start >= H7_BANK_ADDR1) ? H7_BANK_ADDR1 : H7_BANK_ADDR0;
        uint32_t bank = h7_bank_base(start);
        uint32_t snb = h7_bank_snb(target, start);
        bool had_error = false;
        esp_err_t err;

        if (snb == UINT32_MAX) {
            ESP_LOGE(TF_TAG, "H7: address 0x%08X outside flash", (unsigned)start);
            return ESP_FAIL;
        }
        ESP_LOGI(TF_TAG, "H7: erase bank%u sector %u @0x%08X (128 KiB), image 0x%08X..0x%08X",
                 (unsigned)((bank == H7_FLASH_BANK1_BASE) ? 1U : 0U), (unsigned)snb,
                 (unsigned)start, (unsigned)addr, (unsigned)end);

        /* RM0433 sector erase: program SER + SNB, then START.  OpenOCD issues
         * the same register value twice (without/with START). */
        if (tf_swd_write_u32(bank + H7_REG_CR,
                             H7_CR_SER | H7_CR_PSIZE_64 | H7_CR_SNB(snb)) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_swd_write_u32(bank + H7_REG_CR,
                             H7_CR_SER | H7_CR_PSIZE_64 | H7_CR_SNB(snb) | H7_CR_START) != ESP_OK) {
            return ESP_FAIL;
        }
        err = tf_swd_wait_clear(bank + H7_REG_SR, H7_SR_BSY | H7_SR_QW, H7_ERASE_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (h7_sr_check(bank, &had_error) != ESP_OK) {
            return ESP_FAIL;
        }
        if (had_error) {
            return ESP_FAIL;
        }
        erased++;
        start = bank_addr + (snb + 1U) * H7_SECTOR_SIZE;
        if (target->flash_size <= H7_FLASH_BANK_SIZE && start > H7_BANK_ADDR0 + target->flash_size) {
            break;
        }
    }
    if (erased == 0U) {
        ESP_LOGW(TF_TAG, "H7: no sector needed erasing");
    }
    return ESP_OK;
}

static esp_err_t h7_probe(tf_target_t *target)
{
    uint32_t idcode = 0U;
    uint32_t cpuid = 0U;
    uint32_t size_reg = 0U;
    uint32_t uid[3];
    uint32_t dev_id;
    uint32_t flash_size;

    if (tf_swd_read_u32(TF_H7_DBGMCU_IDCODE, &idcode) != ESP_OK) {
        return ESP_FAIL;
    }
    dev_id = idcode & 0xFFFU;
    if (dev_id != 0x450U && dev_id != 0x483U) {
        ESP_LOGW(TF_TAG, "H7: unsupported DBGMCU DEV_ID 0x%03X (IDCODE=0x%08X)",
                 (unsigned)dev_id, (unsigned)idcode);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_REG_CPUID, &cpuid) != ESP_OK) {
        return ESP_FAIL;
    }
    if (cpuid != 0x411FC271U && (cpuid & 0xFF00FF00U) != 0x4100C200U) {
        ESP_LOGW(TF_TAG, "H7: unexpected CPUID 0x%08X (not Cortex-M7)", (unsigned)cpuid);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_H7_FLASH_SIZE_REG, &size_reg) != ESP_OK) {
        return ESP_FAIL;
    }
    flash_size = (size_reg & 0xFFFFU) * 1024U;
    if (flash_size == 0U) {
        ESP_LOGE(TF_TAG, "H7: invalid flash size (FSIZE reg=0x%08X)", (unsigned)size_reg);
        return ESP_FAIL;
    }
    if (flash_size > 2048U * 1024U) {
        ESP_LOGW(TF_TAG, "H7: flash size %u KiB is not supported",
                 (unsigned)(flash_size / 1024U));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_mem(TF_H7_UID_BASE, uid, sizeof(uid)) != ESP_OK) {
        return ESP_FAIL;
    }

    target->idcode = idcode;
    target->cpuid = cpuid;
    memcpy(target->uid, uid, sizeof(target->uid));
    target->flash_base = TF_MAIN_FLASH_BASE;
    target->flash_size = flash_size;
    target->max_image_size = flash_size;
    snprintf(target->family_str, sizeof(target->family_str), "stm32h7");
    ESP_LOGI(TF_TAG, "H7 detected: DEV_ID=0x%03X REV=0x%04X CPUID=0x%08X flash=%u KiB (%s bank)",
             (unsigned)dev_id, (unsigned)(idcode >> 16), (unsigned)cpuid,
             (unsigned)(flash_size / 1024U),
             (flash_size > H7_FLASH_BANK_SIZE) ? "dual" : "single");
    return ESP_OK;
}

static esp_err_t h7_program_begin(tf_target_t *target)
{
    if (h7_unlock_bank(H7_FLASH_BANK0_BASE) != ESP_OK) {
        return ESP_FAIL;
    }
    if (target->flash_size > H7_FLASH_BANK_SIZE) {
        if (h7_unlock_bank(H7_FLASH_BANK1_BASE) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t h7_program_end(tf_target_t *target)
{
    (void)target;
    return h7_lock_banks();
}

const tf_family_ops_t tf_family_stm32h7_ops = {
    .name = "stm32h7",
    .probe = h7_probe,
    .erase = h7_erase,
    .program_begin = h7_program_begin,
    .program = h7_program,
    .program_end = h7_program_end,
};
