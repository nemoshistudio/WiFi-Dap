/*
 * SPDX-License-Identifier: MIT
 *
 * STM32F4 (Cortex-M4) internal-flash algorithm for the target flash engine.
 *
 * Register reference: RM0090 (STM32F405/407/415/417, FLASH at 0x40023C00).
 *   KEYR 0x04, SR 0x0C, CR 0x10.
 *   CR: PG(0) SER(1) STRT(16) PSIZE(9:8) LOCK(31).
 *   SR: EOP(0) OPERR(1) WRPERR(4) PGAERR(5) PGPERR(6) PGSERR(7) BSY(16).
 *
 * Programming strategy ("A4" x64 parallel optimization):
 *   1. erase only the sectors that cover the image;
 *   2. program 64-bit units with PSIZE=3 (x64 parallel) and poll SR.BSY after
 *      every unit;
 *   3. if an x64 unit reports a programming error, clear the flags, switch to
 *      PSIZE=2 (x32) and retry the unit as two 32-bit units.  x32 is also used
 *      for the rest of the image after the first fallback.
 *
 * The core must be halted while the flash is erased/programmed; the engine
 * halts it before calling these operations.
 */
#include <string.h>

#include <esp_log.h>

#include "tf_dap.h"
#include "tf_swd.h"
#include "tf_internal.h"

#define F4_FLASH_REGS        0x40023C00U
#define F4_REG_KEYR          (F4_FLASH_REGS + 0x04U)
#define F4_REG_SR            (F4_FLASH_REGS + 0x0CU)
#define F4_REG_CR            (F4_FLASH_REGS + 0x10U)

#define F4_CR_PG             (1U << 0)
#define F4_CR_SER            (1U << 1)
#define F4_CR_STRT           (1U << 16)
#define F4_CR_PSIZE_64       (3U << 8)
#define F4_CR_PSIZE_32       (2U << 8)
#define F4_CR_LOCK           (1U << 31)

#define F4_SR_EOP            (1U << 0)
#define F4_SR_OPERR          (1U << 1)
#define F4_SR_WRPERR         (1U << 4)
#define F4_SR_PGAERR         (1U << 5)
#define F4_SR_PGPERR         (1U << 6)
#define F4_SR_PGSERR         (1U << 7)
#define F4_SR_BSY            (1U << 16)
#define F4_SR_ERRORS         (F4_SR_OPERR | F4_SR_WRPERR | F4_SR_PGAERR | \
                              F4_SR_PGPERR | F4_SR_PGSERR)

#define F4_KEY1             0x45670123U
#define F4_KEY2             0xCDEF89ABU

#define F4_PROGRAM_UNIT      8U          /* x64 parallel unit */
#define F4_BSY_TIMEOUT_MS    1000U
#define F4_ERASE_TIMEOUT_MS  10000U

typedef struct {
    uint32_t offset;
    uint32_t size;
} f4_sector_t;

/* DEV_IDs of the single-bank (<= 1 MiB) STM32F4 parts.  2 MiB dual-bank parts
 * (F42x/F43x, DEV_ID 0x419/0x421) are intentionally reported as unsupported:
 * their sector numbering uses the bank bit (SNB|0x10) which needs a second
 * bank-aware erase/program path (kept for a follow-up when hardware exists). */
static bool f4_dev_id_supported(uint32_t dev_id)
{
    switch (dev_id) {
    case 0x411: /* F2x0/F2x5 (same flash controller family) */
    case 0x413: /* F405/F407/F415/F417 - first hardware: F407 */
    case 0x423: /* F401xB/C */
    case 0x431: /* F411 */
    case 0x433: /* F401xD/E */
    case 0x441: /* F412/F413 */
        return true;
    default:
        return false;
    }
}

static size_t f4_build_sectors(const tf_target_t *target, f4_sector_t *sectors, size_t cap)
{
    size_t count = 0U;
    uint32_t offset = 0U;

    if (target->flash_size == 0U) {
        return 0U;
    }
    /* Sectors 0..3: 16 KiB each, sector 4: 64 KiB, sectors 5+: 128 KiB each. */
    for (uint32_t i = 0; i < 4U && offset < target->flash_size; i++) {
        sectors[count].offset = offset;
        sectors[count].size = 16U * 1024U;
        offset += 16U * 1024U;
        count++;
        if (count >= cap) {
            return count;
        }
    }
    if (offset < target->flash_size && count < cap) {
        sectors[count].offset = offset;
        sectors[count].size = 64U * 1024U;
        offset += 64U * 1024U;
        count++;
    }
    while (offset < target->flash_size && count < cap) {
        uint32_t size = 128U * 1024U;
        if (target->flash_size - offset < size) {
            size = target->flash_size - offset;
        }
        sectors[count].offset = offset;
        sectors[count].size = size;
        offset += size;
        count++;
    }
    return count;
}

static esp_err_t f4_unlock(tf_target_t *target)
{
    uint32_t cr = 0U;

    (void)target;
    if (tf_swd_write_u32(F4_REG_KEYR, F4_KEY1) != ESP_OK ||
        tf_swd_write_u32(F4_REG_KEYR, F4_KEY2) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_read_u32(F4_REG_CR, &cr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((cr & F4_CR_LOCK) != 0U) {
        ESP_LOGE(TF_TAG, "F4: flash not unlocked (CR=0x%08X)", (unsigned)cr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Read SR, clear error flags, return ESP_FAIL when any error flag was set. */
static esp_err_t f4_sr_check(bool *had_error)
{
    uint32_t sr = 0U;

    *had_error = false;
    if (tf_swd_read_u32(F4_REG_SR, &sr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((sr & F4_SR_ERRORS) != 0U) {
        ESP_LOGW(TF_TAG, "F4: SR error flags 0x%08X", (unsigned)(sr & F4_SR_ERRORS));
        (void)tf_swd_clear_error_bits(F4_REG_SR, sr & (F4_SR_ERRORS | F4_SR_EOP));
        *had_error = true;
        return ESP_OK;
    }
    return ESP_OK;
}

/** Wait for BSY=0 and report/clear programming errors. */
static esp_err_t f4_wait_unit(bool *had_error)
{
    esp_err_t err;

    *had_error = false;
    err = tf_swd_wait_clear(F4_REG_SR, F4_SR_BSY, F4_BSY_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return f4_sr_check(had_error);
}

static esp_err_t f4_write_unit_x64(uint32_t addr, const uint8_t *data)
{
    tf_dap_item_t items[3];
    uint32_t w0 = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                  ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    uint32_t w1 = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                  ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

    items[0].request = TF_REQ_AP_WRITE(TF_AP_TAR);
    items[0].data = addr;
    items[1].request = TF_REQ_AP_WRITE(TF_AP_DRW);
    items[1].data = w0;
    items[2].request = TF_REQ_AP_WRITE(TF_AP_DRW); /* AP auto-increment -> +4 */
    items[2].data = w1;
    return tf_dap_transfer(items, 3U, NULL, NULL);
}

static esp_err_t f4_write_word_x32(uint32_t addr, const uint8_t *data)
{
    tf_dap_item_t items[2];

    items[0].request = TF_REQ_AP_WRITE(TF_AP_TAR);
    items[0].data = addr;
    items[1].request = TF_REQ_AP_WRITE(TF_AP_DRW);
    items[1].data = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                    ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return tf_dap_transfer(items, 2U, NULL, NULL);
}

static esp_err_t f4_program_unit_x32(tf_target_t *target, uint32_t addr, const uint8_t *data)
{
    bool had_error = false;
    esp_err_t err;

    if (!target->f4_x32_mode) {
        target->f4_x32_mode = true;
        if (tf_swd_write_u32(F4_REG_CR, F4_CR_PG | F4_CR_PSIZE_32) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    for (uint32_t word = 0U; word < F4_PROGRAM_UNIT / 4U; word++) {
        if (f4_write_word_x32(addr + word * 4U, data + word * 4U) != ESP_OK) {
            return ESP_FAIL;
        }
        err = f4_wait_unit(&had_error);
        if (err != ESP_OK) {
            return err;
        }
        if (had_error) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t f4_program(tf_target_t *target, uint32_t addr, const uint8_t *data, size_t len)
{
    size_t offset = 0U;

    if ((len % F4_PROGRAM_UNIT) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* f4_erase() clears CR after the erase loop, so re-arm PG/PSIZE here. */
    if (tf_swd_write_u32(F4_REG_CR,
                         F4_CR_PG | (target->f4_x32_mode ? F4_CR_PSIZE_32
                                                          : F4_CR_PSIZE_64)) != ESP_OK) {
        return ESP_FAIL;
    }

    while (offset < len) {
        uint32_t unit_addr = addr + (uint32_t)offset;
        const uint8_t *unit = data + offset;
        bool had_error = false;
        esp_err_t err;

        if (!target->f4_x32_mode) {
            if (f4_write_unit_x64(unit_addr, unit) != ESP_OK) {
                return ESP_FAIL;
            }
            err = f4_wait_unit(&had_error);
            if (err != ESP_OK) {
                return err;
            }
            if (!had_error) {
                offset += F4_PROGRAM_UNIT;
                continue;
            }
            /* x64 parallel programming failed: clear flags and fall back to x32
             * for this unit (flash controller is still unlocked). */
            ESP_LOGW(TF_TAG, "F4: x64 programming failed at 0x%08X, falling back to x32",
                     (unsigned)unit_addr);
            target->f4_x32_mode = true;
            if (tf_swd_write_u32(F4_REG_CR, F4_CR_PG | F4_CR_PSIZE_32) != ESP_OK) {
                return ESP_FAIL;
            }
        }
        err = f4_program_unit_x32(target, unit_addr, unit);
        if (err != ESP_OK) {
            return err;
        }
        offset += F4_PROGRAM_UNIT;
    }
    return ESP_OK;
}

static esp_err_t f4_erase(tf_target_t *target, uint32_t addr, uint32_t size)
{
    f4_sector_t sectors[16];
    size_t count;
    size_t erased = 0U;
    uint32_t start = addr - target->flash_base;
    uint32_t end = start + size;

    count = f4_build_sectors(target, sectors, sizeof(sectors) / sizeof(sectors[0]));
    if (count == 0U) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < count; i++) {
        uint32_t sector_end = sectors[i].offset + sectors[i].size;
        bool had_error = false;
        esp_err_t err;

        if (sector_end <= start || sectors[i].offset >= end) {
            continue;
        }
        ESP_LOGI(TF_TAG, "F4: erase sector %u @0x%08X (%u KiB), image 0x%08X..0x%08X",
                 (unsigned)i, (unsigned)(target->flash_base + sectors[i].offset),
                 (unsigned)(sectors[i].size / 1024U),
                 (unsigned)addr, (unsigned)(addr + size));
        if (tf_swd_write_u32(F4_REG_CR,
                             F4_CR_SER | ((uint32_t)i << 3) | F4_CR_STRT) != ESP_OK) {
            return ESP_FAIL;
        }
        err = tf_swd_wait_clear(F4_REG_SR, F4_SR_BSY, F4_ERASE_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (f4_sr_check(&had_error) != ESP_OK) {
            return ESP_FAIL;
        }
        if (had_error) {
            return ESP_FAIL;
        }
        erased++;
    }
    /* Clear SER/STRT but keep the controller unlocked for programming. */
    if (tf_swd_write_u32(F4_REG_CR, 0U) != ESP_OK) {
        return ESP_FAIL;
    }
    if (erased == 0U) {
        ESP_LOGW(TF_TAG, "F4: no sector needed erasing");
    }
    return ESP_OK;
}

static esp_err_t f4_probe(tf_target_t *target)
{
    uint32_t idcode = 0U;
    uint32_t cpuid = 0U;
    uint32_t size_reg = 0U;
    uint32_t uid[3];
    uint32_t dev_id;
    uint32_t flash_size;

    if (tf_swd_read_u32(TF_F4_DBGMCU_IDCODE, &idcode) != ESP_OK) {
        return ESP_FAIL;
    }
    dev_id = idcode & 0xFFFU;
    if (!f4_dev_id_supported(dev_id)) {
        ESP_LOGW(TF_TAG, "F4: unsupported DBGMCU DEV_ID 0x%03X (IDCODE=0x%08X)",
                 (unsigned)dev_id, (unsigned)idcode);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_REG_CPUID, &cpuid) != ESP_OK) {
        return ESP_FAIL;
    }
    /* Cortex-M4 CPUID (PARTNO 0xC24, variant 0x1). */
    if ((cpuid & 0xFF00FF00U) != 0x4100C200U && cpuid != 0x410FC241U) {
        ESP_LOGW(TF_TAG, "F4: unexpected CPUID 0x%08X (not Cortex-M4)", (unsigned)cpuid);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_F4_FLASH_SIZE_REG, &size_reg) != ESP_OK) {
        return ESP_FAIL;
    }
    flash_size = ((size_reg >> 16) & 0xFFFFU) * 1024U;
    if (flash_size == 0U) {
        ESP_LOGE(TF_TAG, "F4: invalid flash size (FSIZE reg=0x%08X)", (unsigned)size_reg);
        return ESP_FAIL;
    }
    if (flash_size > 1024U * 1024U) {
        ESP_LOGW(TF_TAG, "F4: %u KiB dual-bank parts are not supported yet",
                 (unsigned)(flash_size / 1024U));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_mem(TF_F4_UID_BASE, uid, sizeof(uid)) != ESP_OK) {
        return ESP_FAIL;
    }

    target->idcode = idcode;
    target->cpuid = cpuid;
    memcpy(target->uid, uid, sizeof(target->uid));
    target->flash_base = TF_MAIN_FLASH_BASE;
    target->flash_size = flash_size;
    target->max_image_size = flash_size;
    target->f4_x32_mode = false;
    snprintf(target->family_str, sizeof(target->family_str), "stm32f4");
    ESP_LOGI(TF_TAG, "F4 detected: DEV_ID=0x%03X REV=0x%04X CPUID=0x%08X flash=%u KiB",
             (unsigned)dev_id, (unsigned)(idcode >> 16), (unsigned)cpuid,
             (unsigned)(flash_size / 1024U));
    return ESP_OK;
}

static esp_err_t f4_program_begin(tf_target_t *target)
{
    uint32_t sr = 0U;

    if (f4_unlock(target) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_read_u32(F4_REG_SR, &sr) == ESP_OK && (sr & F4_SR_ERRORS) != 0U) {
        (void)tf_swd_clear_error_bits(F4_REG_SR, sr & (F4_SR_ERRORS | F4_SR_EOP));
    }
    target->f4_x32_mode = false;
    if (tf_swd_write_u32(F4_REG_CR, F4_CR_PG | F4_CR_PSIZE_64) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t f4_program_end(tf_target_t *target)
{
    (void)target;
    return tf_swd_write_u32(F4_REG_CR, F4_CR_LOCK);
}

const tf_family_ops_t tf_family_stm32f4_ops = {
    .name = "stm32f4",
    .probe = f4_probe,
    .erase = f4_erase,
    .program_begin = f4_program_begin,
    .program = f4_program,
    .program_end = f4_program_end,
};
