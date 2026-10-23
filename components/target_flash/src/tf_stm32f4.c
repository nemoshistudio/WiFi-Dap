/*
 * SPDX-License-Identifier: MIT
 *
 * STM32F2/F4/F7 (Cortex-M3/M4/M7) internal-flash algorithm for the target
 * flash engine.
 *
 * Register reference: RM0090 (STM32F4), RM0033 (STM32F2), RM0385/RM0410/RM0431
 * (STM32F7) - all three use the same sector-based controller:
 *   FLASH base 0x40023C00.  KEYR 0x04, SR 0x0C, CR 0x10.
 *   CR: PG(0) SER(1) MER(2) SNB(6:3) PSIZE(9:8) STRT(16) LOCK(31).
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
 * Coverage:
 *   F2              - 0x411 (sector map 4x16 + 64 + 128 KiB)
 *   F4 single-bank  - 0x413/0x423/0x431/0x433/0x441/0x458
 *   F7 single-bank  - 0x449 (F74x/F75x, 32+128+256 KiB sectors) and 0x452
 *                     (F72x/F73x, 16+64+128 KiB sectors)
 * The 2 MiB dual-bank parts (F42x/F43x, F469/F479, F76x/F77x dual-bank and
 * single-bank-option variants) use bank-relative sector numbering (SNB|0x10)
 * and are intentionally reported as identified-but-unsupported.
 *
 * The core must be halted while the flash is erased/programmed; the engine
 * halts it before calling these operations.  F407 hardware runs the F4 path;
 * F2/F7 use the same register sequence but still need first-hardware
 * validation.
 */
#include <string.h>

#include <esp_log.h>

#include "tf_dap.h"
#include "tf_stm32_geom.h"
#include "tf_stm32_id.h"
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
    uint16_t dev_id;
    const char *family;   /* lower-case public family string */
    tf_core_class_t core; /* CPUID core class check */
} f4_part_t;

static const f4_part_t f4_parts[] = {
    { 0x411U, "stm32f2", TF_CORE_M3 }, /* F205/F207/F215/F217 */
    { 0x413U, "stm32f4", TF_CORE_M4 }, /* F405/F407/F415/F417 */
    { 0x421U, "stm32f4", TF_CORE_M4 }, /* F446 */
    { 0x423U, "stm32f4", TF_CORE_M4 }, /* F401xB/xC */
    { 0x431U, "stm32f4", TF_CORE_M4 }, /* F411 */
    { 0x433U, "stm32f4", TF_CORE_M4 }, /* F401xD/xE */
    { 0x441U, "stm32f4", TF_CORE_M4 }, /* F412/F413 */
    { 0x458U, "stm32f4", TF_CORE_M4 }, /* F410 */
    { 0x449U, "stm32f7", TF_CORE_M7 }, /* F74x/F75x */
    { 0x452U, "stm32f7", TF_CORE_M7 }, /* F72x/F73x */
};

static const f4_part_t *f4_part_find(uint32_t dev_id)
{
    for (size_t i = 0U; i < sizeof(f4_parts) / sizeof(f4_parts[0]); i++) {
        if (f4_parts[i].dev_id == (uint16_t)dev_id) {
            return &f4_parts[i];
        }
    }
    return NULL;
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

    if ((len % F4_PROGRAM_UNIT) != 0U || (addr % F4_PROGRAM_UNIT) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr < target->flash_base ||
        (addr - target->flash_base) > target->flash_size ||
        len > (size_t)(target->flash_size - (addr - target->flash_base))) {
        return ESP_ERR_INVALID_SIZE;
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
    tf_sector_t sectors[32];
    size_t count;
    size_t erased = 0U;
    uint32_t start = addr - target->flash_base;
    uint32_t end = start + size;

    if (size == 0U || addr < target->flash_base || start > target->flash_size ||
        size > (target->flash_size - start)) {
        return ESP_ERR_INVALID_SIZE;
    }
    count = tf_stm32_sector_map((uint16_t)target->dev_id, target->flash_size, sectors,
                                sizeof(sectors) / sizeof(sectors[0]));
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
        ESP_LOGI(TF_TAG, "%s: erase sector %u @0x%08X (%u KiB), image 0x%08X..0x%08X",
                 target->family_str, (unsigned)i,
                 (unsigned)(target->flash_base + sectors[i].offset),
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
    const tf_stm32_part_t *part;
    const f4_part_t *cfg;
    uint32_t idcode = 0U;
    uint32_t cpuid = 0U;
    uint32_t size_word = 0U;
    uint32_t uid[3] = {0U, 0U, 0U};
    uint32_t dev_id;
    uint32_t flash_size;
    tf_core_class_t core;
    tf_sector_t sectors[32];

    if (tf_swd_read_u32(TF_F4_DBGMCU_IDCODE, &idcode) != ESP_OK) {
        return ESP_FAIL;
    }
    dev_id = idcode & 0xFFFU;
    cfg = f4_part_find(dev_id);
    if (cfg == NULL) {
        ESP_LOGW(TF_TAG, "F4: unsupported DBGMCU DEV_ID 0x%03X (IDCODE=0x%08X)",
                 (unsigned)dev_id, (unsigned)idcode);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_REG_CPUID, &cpuid) != ESP_OK) {
        return ESP_FAIL;
    }
    core = tf_stm32_core_from_cpuid(cpuid);
    if (core != cfg->core) {
        ESP_LOGW(TF_TAG, "%s: unexpected CPUID 0x%08X (%s, want %s)",
                 cfg->family, (unsigned)cpuid, tf_stm32_core_name(core),
                 tf_stm32_core_name(cfg->core));
        return ESP_ERR_NOT_SUPPORTED;
    }
    part = tf_stm32_part_find((uint16_t)dev_id);
    if (part == NULL || part->flash_size_reg == 0U || part->flash_size_shift == 0xFFU) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(part->flash_size_reg, &size_word) != ESP_OK) {
        return ESP_FAIL;
    }
    flash_size = ((size_word >> part->flash_size_shift) & 0xFFFFU) * 1024U;
    if (flash_size < 16U * 1024U ||
        flash_size > tf_stm32_sector_max_flash((uint16_t)dev_id)) {
        ESP_LOGE(TF_TAG, "%s: unsupported flash size %u KiB (FSIZE reg=0x%08X)",
                 cfg->family, (unsigned)(flash_size / 1024U), (unsigned)size_word);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_stm32_sector_map((uint16_t)dev_id, flash_size, sectors,
                            sizeof(sectors) / sizeof(sectors[0])) == 0U) {
        ESP_LOGW(TF_TAG, "%s: DEV_ID=0x%03X flash=%u KiB sector map not modelled",
                 cfg->family, (unsigned)dev_id, (unsigned)(flash_size / 1024U));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (part->uid_base != 0U &&
        tf_swd_read_mem(part->uid_base, uid, sizeof(uid)) != ESP_OK) {
        return ESP_FAIL;
    }

    target->dev_id = (uint16_t)dev_id;
    target->idcode = idcode;
    target->cpuid = cpuid;
    memcpy(target->uid, uid, sizeof(target->uid));
    target->flash_base = TF_MAIN_FLASH_BASE;
    target->flash_size = flash_size;
    target->max_image_size = flash_size;
    target->f4_x32_mode = false;
    snprintf(target->family_str, sizeof(target->family_str), "%s", cfg->family);
    ESP_LOGI(TF_TAG, "%s detected: DEV_ID=0x%03X REV=0x%04X CPUID=0x%08X (%s) flash=%u KiB",
             cfg->family, (unsigned)dev_id, (unsigned)(idcode >> 16), (unsigned)cpuid,
             tf_stm32_core_name(core), (unsigned)(flash_size / 1024U));
    return ESP_OK;
}

static esp_err_t f4_program_begin(tf_target_t *target)
{
    uint32_t sr = 0U;

    if (f4_unlock(target) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_wait_clear(F4_REG_SR, F4_SR_BSY, F4_BSY_TIMEOUT_MS) != ESP_OK) {
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

const tf_family_ops_t tf_family_stm32f7_ops = {
    .name = "stm32f7",
    .probe = f4_probe,
    .erase = f4_erase,
    .program_begin = f4_program_begin,
    .program = f4_program,
    .program_end = f4_program_end,
};
