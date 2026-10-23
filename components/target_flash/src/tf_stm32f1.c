/*
 * SPDX-License-Identifier: MIT
 *
 * STM32F0/F1/F3 (Cortex-M0/M3/M4) internal-flash algorithm for the target
 * flash engine.
 *
 * Register reference: RM0008 (STM32F1), RM0091 (STM32F0), RM0316 (STM32F3):
 *   FLASH base 0x40022000 (bank 2 on F1 XL-density at +0x40):
 *   ACR 0x00, KEYR 0x04, OPTKEYR 0x08, SR 0x0C, CR 0x10, AR 0x14.
 *   CR: PG(0) PER(1) MER(2) OPTPG(4) OPTER(5) STRT(6) LOCK(7) OBL_LAUNCH(13).
 *   SR: BSY(0) PGERR(2) WRPRTERR(4) EOP(5).
 *
 * Programming strategy (matches the OpenOCD stm32f1x flash loader):
 *   1. erase every page that covers the image (PER + AR + STRT, poll BSY);
 *   2. program one 16-bit half-word at a time (PG, 16-bit AHB store, poll BSY,
 *      check PGERR/WRPRTERR); the AHB-AP CSW is switched to 16-bit accesses
 *      for the whole sequence and restored afterwards;
 *   3. F1 XL-density images may span bank 2 (0x08080000, registers +0x40).
 *
 * The core must be halted while the flash is erased/programmed; the engine
 * halts it before calling these operations.  First-hardware validation is
 * still pending for these families.
 */
#include <string.h>

#include <esp_log.h>

#include "tf_dap.h"
#include "tf_stm32_id.h"
#include "tf_swd.h"
#include "tf_internal.h"

#define F1_FLASH_REGS        0x40022000U
#define F1_REG_KEYR          0x04U
#define F1_REG_SR            0x0CU
#define F1_REG_CR            0x10U
#define F1_REG_AR            0x14U

#define F1_CR_PG             (1U << 0)
#define F1_CR_PER            (1U << 1)
#define F1_CR_MER            (1U << 2)
#define F1_CR_STRT           (1U << 6)
#define F1_CR_LOCK           (1U << 7)

#define F1_SR_BSY            (1U << 0)
#define F1_SR_PGERR          (1U << 2)
#define F1_SR_WRPRTERR       (1U << 4)
#define F1_SR_EOP            (1U << 5)
#define F1_SR_ERRORS         (F1_SR_PGERR | F1_SR_WRPRTERR)

#define F1_KEY1              0x45670123U
#define F1_KEY2              0xCDEF89ABU

#define F1_PROGRAM_UNIT      2U        /* one half-word per write */
#define F1_BSY_TIMEOUT_MS    1000U
#define F1_ERASE_TIMEOUT_MS  10000U
#define F1_HALFWORD_TIMEOUT_MS 100U

/* F1 XL-density bank 2: flash address 0x08080000, registers at +0x40. */
#define F1_XL_BANK2_OFFSET   0x00080000U

static uint32_t f1_bank_reg_offset(const tf_target_t *target, uint32_t addr)
{
    if (!target->f1.dual_bank) {
        return 0U;
    }
    return ((addr - target->flash_base) >= F1_XL_BANK2_OFFSET)
               ? target->f1.bank_reg_offset
               : 0U;
}

static uint32_t f1_reg(const tf_target_t *target, uint32_t addr, uint32_t reg)
{
    return target->f1.regs_base + f1_bank_reg_offset(target, addr) + reg;
}

/** Read SR, clear PGERR/WRPRTERR/EOP, report whether an error flag was set. */
static esp_err_t f1_sr_check(const tf_target_t *target, uint32_t addr, bool *had_error)
{
    uint32_t sr = 0U;

    *had_error = false;
    if (tf_swd_read_u32(f1_reg(target, addr, F1_REG_SR), &sr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((sr & F1_SR_ERRORS) != 0U) {
        ESP_LOGW(TF_TAG, "%s: SR error flags 0x%08X", target->family_str,
                 (unsigned)(sr & F1_SR_ERRORS));
        (void)tf_swd_clear_error_bits(f1_reg(target, addr, F1_REG_SR),
                                      (sr & F1_SR_ERRORS) | F1_SR_EOP);
        *had_error = true;
    }
    return ESP_OK;
}

/** Unlock one F1-style controller block (bank 1 or bank 2). */
static esp_err_t f1_unlock_block(const tf_target_t *target, uint32_t reg_offset)
{
    uint32_t cr = 0U;

    if (tf_swd_write_u32(target->f1.regs_base + reg_offset + F1_REG_KEYR, F1_KEY1) != ESP_OK ||
        tf_swd_write_u32(target->f1.regs_base + reg_offset + F1_REG_KEYR, F1_KEY2) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_read_u32(target->f1.regs_base + reg_offset + F1_REG_CR, &cr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((cr & F1_CR_LOCK) != 0U) {
        ESP_LOGE(TF_TAG, "%s: flash block +0x%02X not unlocked (CR=0x%08X)",
                 target->family_str, (unsigned)reg_offset, (unsigned)cr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t f1_program_begin(tf_target_t *target)
{
    bool had_error = false;

    if (f1_unlock_block(target, 0U) != ESP_OK) {
        return ESP_FAIL;
    }
    if (target->f1.dual_bank &&
        f1_unlock_block(target, target->f1.bank_reg_offset) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_wait_clear(f1_reg(target, target->flash_base, F1_REG_SR), F1_SR_BSY,
                          F1_BSY_TIMEOUT_MS) != ESP_OK) {
        return ESP_FAIL;
    }
    if (target->f1.dual_bank) {
        if (tf_swd_wait_clear(f1_reg(target, target->flash_base + F1_XL_BANK2_OFFSET,
                                     F1_REG_SR),
                              F1_SR_BSY, F1_BSY_TIMEOUT_MS) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (f1_sr_check(target, target->flash_base, &had_error) != ESP_OK) {
        return ESP_FAIL;
    }
    if (target->f1.dual_bank &&
        f1_sr_check(target, target->flash_base + F1_XL_BANK2_OFFSET, &had_error) != ESP_OK) {
        return ESP_FAIL;
    }
    (void)had_error; /* stale flags from an earlier session are cleared */
    return ESP_OK;
}

static esp_err_t f1_erase(tf_target_t *target, uint32_t addr, uint32_t size)
{
    uint32_t offset;
    uint32_t end_offset;
    uint32_t erased = 0U;

    if (addr < target->flash_base) {
        return ESP_ERR_INVALID_ARG;
    }
    offset = addr - target->flash_base;
    if (size == 0U || offset > target->flash_size ||
        size > (target->flash_size - offset)) {
        ESP_LOGE(TF_TAG, "%s: erase range 0x%08X+%u outside flash (%u KiB)",
                 target->family_str, (unsigned)addr, (unsigned)size,
                 (unsigned)(target->flash_size / 1024U));
        return ESP_ERR_INVALID_SIZE;
    }
    end_offset = offset + size;
    offset &= ~(target->f1.page_size - 1U);
    while (offset < end_offset) {
        uint32_t page_addr = target->flash_base + offset;
        uint32_t cr_base = f1_reg(target, page_addr, F1_REG_CR);
        uint32_t sr_base = f1_reg(target, page_addr, F1_REG_SR);
        bool had_error = false;

        ESP_LOGI(TF_TAG, "%s: erase page @0x%08X, image 0x%08X..0x%08X",
                 target->family_str, (unsigned)page_addr, (unsigned)addr,
                 (unsigned)(addr + size));
        if (tf_swd_wait_clear(sr_base, F1_SR_BSY, F1_BSY_TIMEOUT_MS) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_swd_write_u32(cr_base, F1_CR_PER) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_swd_write_u32(f1_reg(target, page_addr, F1_REG_AR), page_addr) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_swd_write_u32(cr_base, F1_CR_PER | F1_CR_STRT) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_swd_wait_clear(sr_base, F1_SR_BSY, F1_ERASE_TIMEOUT_MS) != ESP_OK) {
            return ESP_FAIL;
        }
        if (f1_sr_check(target, page_addr, &had_error) != ESP_OK) {
            return ESP_FAIL;
        }
        if (had_error) {
            return ESP_FAIL;
        }
        erased++;
        offset += target->f1.page_size;
    }
    /* Drop PER before programming (the controller stays unlocked). */
    if (tf_swd_write_u32(f1_reg(target, target->flash_base, F1_REG_CR), 0U) != ESP_OK) {
        return ESP_FAIL;
    }
    if (erased == 0U) {
        ESP_LOGW(TF_TAG, "%s: no page needed erasing", target->family_str);
    }
    return ESP_OK;
}

/** Wait for BSY to clear while running with 16-bit AHB accesses. */
static esp_err_t f1_wait_halfword_done(const tf_target_t *target, uint32_t addr)
{
    uint64_t deadline = tf_now_ms() + F1_HALFWORD_TIMEOUT_MS;
    uint32_t sr_addr = f1_reg(target, addr, F1_REG_SR);
    uint16_t sr = 0U;

    for (;;) {
        if (tf_swd_read_u16_raw(sr_addr, &sr) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((sr & F1_SR_BSY) == 0U) {
            break;
        }
        if (tf_now_ms() > deadline) {
            ESP_LOGE(TF_TAG, "%s: timeout waiting for BSY at 0x%08X",
                     target->family_str, (unsigned)addr);
            return ESP_ERR_TIMEOUT;
        }
    }
    if ((sr & F1_SR_ERRORS) != 0U) {
        ESP_LOGW(TF_TAG, "%s: programming error flags 0x%04X at 0x%08X",
                 target->family_str, (unsigned)(sr & F1_SR_ERRORS), (unsigned)addr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Restore 32-bit AP accesses and clear PG/PER on the touched bank(s). */
static esp_err_t f1_end_access(tf_target_t *target, uint32_t addr, bool clear_errors)
{
    esp_err_t err = tf_swd_set_access_16bit(false);

    if (target->f1.dual_bank) {
        (void)tf_swd_write_u32(f1_reg(target, target->flash_base, F1_REG_CR), 0U);
        (void)tf_swd_write_u32(f1_reg(target, target->flash_base + F1_XL_BANK2_OFFSET,
                                      F1_REG_CR), 0U);
    } else if (tf_swd_write_u32(f1_reg(target, addr, F1_REG_CR), 0U) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (clear_errors) {
        bool had_error = false;
        (void)f1_sr_check(target, target->flash_base, &had_error);
        if (target->f1.dual_bank) {
            (void)f1_sr_check(target, target->flash_base + F1_XL_BANK2_OFFSET, &had_error);
        }
    }
    return err;
}

static esp_err_t f1_program(tf_target_t *target, uint32_t addr, const uint8_t *data,
                            size_t len)
{
    size_t offset = 0U;
    esp_err_t result = ESP_FAIL;

    if ((len % F1_PROGRAM_UNIT) != 0U || (addr % F1_PROGRAM_UNIT) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr < target->flash_base ||
        (addr - target->flash_base) > target->flash_size ||
        len > (size_t)(target->flash_size - (addr - target->flash_base))) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Erase left the controller unlocked with CR cleared; arm programming
     * while the AP is still in 32-bit mode (CR is a 32-bit register). */
    if (tf_swd_write_u32(f1_reg(target, addr, F1_REG_CR), F1_CR_PG) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_set_access_16bit(true) != ESP_OK) {
        (void)tf_swd_write_u32(f1_reg(target, addr, F1_REG_CR), 0U);
        return ESP_FAIL;
    }
    while (offset < len) {
        uint32_t unit_addr = addr + (uint32_t)offset;
        uint16_t halfword = (uint16_t)((uint16_t)data[offset] |
                                       ((uint16_t)data[offset + 1U] << 8));

        /* The bank can change inside one image (F1 XL).  A 1 KiB engine block
         * never straddles the 0x08080000 bank boundary, but keep the switch
         * safe anyway: re-arm PG with 32-bit accesses on the new bank. */
        if (f1_bank_reg_offset(target, unit_addr) != f1_bank_reg_offset(target, addr)) {
            if (tf_swd_set_access_16bit(false) != ESP_OK ||
                tf_swd_write_u32(f1_reg(target, unit_addr, F1_REG_CR), F1_CR_PG) != ESP_OK ||
                tf_swd_set_access_16bit(true) != ESP_OK) {
                break;
            }
        }
        if (tf_swd_write_u16_raw(unit_addr, halfword) != ESP_OK) {
            break;
        }
        if (f1_wait_halfword_done(target, unit_addr) != ESP_OK) {
            break;
        }
        offset += F1_PROGRAM_UNIT;
    }
    if (offset == len) {
        result = ESP_OK;
    }
    {
        esp_err_t end_err = f1_end_access(target, addr, result != ESP_OK);

        if (end_err != ESP_OK && result == ESP_OK) {
            result = end_err;
        }
    }
    return result;
}

static esp_err_t f1_program_end(tf_target_t *target)
{
    esp_err_t err = ESP_OK;

    if (tf_swd_write_u32(f1_reg(target, target->flash_base, F1_REG_CR), F1_CR_LOCK) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (target->f1.dual_bank &&
        tf_swd_write_u32(f1_reg(target, target->flash_base + F1_XL_BANK2_OFFSET,
                                F1_REG_CR), F1_CR_LOCK) != ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

/** Map the DEV_ID series to the public lower-case family string. */
static const char *f1_family_name(const tf_stm32_part_t *part)
{
    if (strcmp(part->series, "STM32F0") == 0) {
        return "stm32f0";
    }
    if (strcmp(part->series, "STM32F3") == 0) {
        return "stm32f3";
    }
    return "stm32f1";
}

static tf_core_class_t f1_family_core(const tf_stm32_part_t *part)
{
    if (strcmp(part->series, "STM32F0") == 0) {
        return TF_CORE_M0;
    }
    if (strcmp(part->series, "STM32F3") == 0) {
        return TF_CORE_M4;
    }
    return TF_CORE_M3;
}

static esp_err_t f1_probe(tf_target_t *target)
{
    static const uint32_t idcode_candidates[] = {
        TF_STM32_DBGMCU_IDCODE_E0042000,
        TF_STM32_DBGMCU_IDCODE_40015800,
    };
    const tf_stm32_part_t *part;
    uint32_t idcode = 0U;
    uint32_t cpuid = 0U;
    uint32_t size_word = 0U;
    uint32_t uid[3] = {0U, 0U, 0U};
    uint32_t dev_id;
    uint32_t flash_size;
    tf_f1_geom_t geom;
    tf_core_class_t core;
    bool idcode_ok = false;

    for (size_t i = 0U; i < sizeof(idcode_candidates) / sizeof(idcode_candidates[0]); i++) {
        if (tf_swd_read_u32(idcode_candidates[i], &idcode) == ESP_OK &&
            (idcode & 0xFFFU) != 0U && (idcode & 0xFFFU) != 0xFFFU) {
            idcode_ok = true;
            break;
        }
        (void)tf_swd_recover_bus();
    }
    if (!idcode_ok) {
        ESP_LOGE(TF_TAG, "F1X: DBGMCU IDCODE not readable");
        return ESP_FAIL;
    }
    dev_id = idcode & 0xFFFU;
    part = tf_stm32_part_find((uint16_t)dev_id);
    if (part == NULL ||
        (strcmp(part->series, "STM32F0") != 0 &&
         strcmp(part->series, "STM32F1") != 0 &&
         strcmp(part->series, "STM32F3") != 0)) {
        ESP_LOGW(TF_TAG, "F1X: unsupported DBGMCU DEV_ID 0x%03X", (unsigned)dev_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_REG_CPUID, &cpuid) != ESP_OK) {
        return ESP_FAIL;
    }
    core = tf_stm32_core_from_cpuid(cpuid);
    if (core != f1_family_core(part)) {
        ESP_LOGW(TF_TAG, "F1X: unexpected CPUID 0x%08X (%s, want %s)",
                 (unsigned)cpuid, tf_stm32_core_name(core),
                 tf_stm32_core_name(f1_family_core(part)));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (part->flash_size_reg == 0U || part->flash_size_shift == 0xFFU) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(part->flash_size_reg, &size_word) != ESP_OK) {
        return ESP_FAIL;
    }
    flash_size = ((size_word >> part->flash_size_shift) & 0xFFFFU) * 1024U;
    if (flash_size < 16U * 1024U || flash_size > 1024U * 1024U) {
        ESP_LOGE(TF_TAG, "F1X: invalid flash size (FSIZE reg=0x%08X)", (unsigned)size_word);
        return ESP_FAIL;
    }
    if (!tf_stm32_f1_geom((uint16_t)dev_id, flash_size, &geom)) {
        ESP_LOGW(TF_TAG, "F1X: DEV_ID=0x%03X flash=%u KiB not modelled",
                 (unsigned)dev_id, (unsigned)(flash_size / 1024U));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (part->uid_base != 0U) {
        if (tf_swd_read_mem(part->uid_base, uid, sizeof(uid)) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    target->dev_id = (uint16_t)dev_id;
    target->idcode = idcode;
    target->cpuid = cpuid;
    memcpy(target->uid, uid, sizeof(target->uid));
    target->flash_base = TF_MAIN_FLASH_BASE;
    target->flash_size = flash_size;
    target->max_image_size = flash_size;
    target->f1 = geom;
    snprintf(target->family_str, sizeof(target->family_str), "%s", f1_family_name(part));
    ESP_LOGI(TF_TAG,
             "%s detected: DEV_ID=0x%03X REV=0x%04X CPUID=0x%08X (%s) flash=%u KiB "
             "(%u KiB pages, %u pages%s)",
             target->family_str, (unsigned)dev_id, (unsigned)(idcode >> 16),
             (unsigned)cpuid, tf_stm32_core_name(core),
             (unsigned)(flash_size / 1024U), (unsigned)(geom.page_size / 1024U),
             (unsigned)geom.num_pages, geom.dual_bank ? ", dual bank" : "");
    return ESP_OK;
}

const tf_family_ops_t tf_family_stm32f1_ops = {
    .name = "stm32f1x",
    .probe = f1_probe,
    .erase = f1_erase,
    .program_begin = f1_program_begin,
    .program = f1_program,
    .program_end = f1_program_end,
};
