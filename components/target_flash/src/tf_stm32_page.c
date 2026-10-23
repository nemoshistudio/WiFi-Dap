/*
 * SPDX-License-Identifier: MIT
 *
 * Page-based STM32 internal-flash algorithms for the target flash engine:
 *   - STM32G4 (primary target of this work package, RM0440)
 *   - STM32L4 / L4+ (RM0351/RM0394/RM0432)
 *   - STM32G0 (RM0444/RM0454)
 *   - STM32WB (RM0434)
 *   - STM32WL (RM0461)
 *
 * These families share the same "L4-style" flash controller register layout
 * (KEYR 0x08, SR 0x10, CR 0x14, OPTR 0x20; WB/WL move the block to
 * 0x58004000), page erase (PER + PNB + optional BKER + STRT) and 64-bit
 * double-word programming (PG, write two 32-bit words, poll SR.BSY).
 *
 * G4 register reference (RM0440):
 *   FLASH base 0x40022000; KEYR 0x08, SR 0x10, CR 0x14, OPTR 0x20.
 *   CR: PG(0) PER(1) MER1(2) PNB[6:0](bits 3..9) BKER(11) STRT(16)
 *       OPTSTRT(17) LOCK(31).
 *   SR: EOP(0) OPERR(1) PROGERR(3) WRPERR(4) PGAERR(5) SIZERR(6) PGSERR(7)
 *       BSY(16), optional BSY2(17) on dual-bank G0.
 *   Cat.2 (G431/G441) and Cat.4 (G491/G4A1) are single-bank with 2 KiB pages;
 *   Cat.3 (G47x/G48x) has 4 KiB pages in single-bank mode and 2 KiB pages in
 *   dual-bank mode (FLASH_OPTR.DBANK bit 22), bank 2 selected with CR.BKER.
 *   Programming granularity is one 64-bit double word.
 *
 * Unlock/lock, error clearing, BSY timeouts and address/range validation are
 * implemented here.  The core is halted by the engine before these operations
 * run.  The algorithms were written against the reference manuals and the
 * OpenOCD stm32l4x flash driver/loader (same register sequence); they still
 * need first-hardware validation on a G4 board.
 */
#include <string.h>

#include <esp_log.h>

#include "tf_dap.h"
#include "tf_stm32_geom.h"
#include "tf_stm32_id.h"
#include "tf_swd.h"
#include "tf_internal.h"

/* L4-style flash controller register offsets from the family block base. */
#define PAGE_REG_KEYR    0x08U
#define PAGE_REG_SR      0x10U
#define PAGE_REG_CR      0x14U

#define PAGE_CR_PG       (1U << 0)
#define PAGE_CR_PER      (1U << 1)
#define PAGE_CR_MER1     (1U << 2)
#define PAGE_CR_PNB_SHIFT 3U
#define PAGE_CR_STRT     (1U << 16)
#define PAGE_CR_LOCK     (1U << 31)

#define PAGE_SR_EOP      (1U << 0)
#define PAGE_SR_OPERR    (1U << 1)
#define PAGE_SR_PROGERR  (1U << 3)
#define PAGE_SR_WRPERR   (1U << 4)
#define PAGE_SR_PGAERR   (1U << 5)
#define PAGE_SR_SIZERR   (1U << 6)
#define PAGE_SR_PGSERR   (1U << 7)
#define PAGE_SR_BSY      (1U << 16)
#define PAGE_SR_ERRORS   (PAGE_SR_OPERR | PAGE_SR_PROGERR | PAGE_SR_WRPERR | \
                          PAGE_SR_PGAERR | PAGE_SR_SIZERR | PAGE_SR_PGSERR)

#define PAGE_KEY1        0x45670123U
#define PAGE_KEY2        0xCDEF89ABU

#define PAGE_PROGRAM_UNIT 8U          /* 64-bit double word */
#define PAGE_BSY_TIMEOUT_MS 1000U
#define PAGE_ERASE_TIMEOUT_MS 10000U

static uint32_t page_reg(const tf_target_t *target, uint32_t offset)
{
    return target->page.regs_base + offset;
}

static uint32_t page_bsy_mask(const tf_page_geom_t *geom)
{
    uint32_t mask = PAGE_SR_BSY;

    if (geom->bsy2_shift != 0U) {
        mask |= (1U << geom->bsy2_shift);   /* G0B/G0C bank 2 busy */
    }
    if (geom->cfgbsy_shift != 0U) {
        mask |= (1U << geom->cfgbsy_shift); /* G0B/G0C control-register busy */
    }
    return mask;
}

static esp_err_t page_wait_ready(const tf_target_t *target)
{
    return tf_swd_wait_clear(page_reg(target, PAGE_REG_SR), page_bsy_mask(&target->page),
                             PAGE_BSY_TIMEOUT_MS);
}

/** Read SR, clear error/EOP flags, report whether any error flag was set. */
static esp_err_t page_sr_check(const tf_target_t *target, bool *had_error)
{
    uint32_t sr = 0U;

    *had_error = false;
    if (tf_swd_read_u32(page_reg(target, PAGE_REG_SR), &sr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((sr & PAGE_SR_ERRORS) != 0U) {
        ESP_LOGW(TF_TAG, "%s: SR error flags 0x%08X", target->family_str,
                 (unsigned)(sr & PAGE_SR_ERRORS));
        (void)tf_swd_clear_error_bits(page_reg(target, PAGE_REG_SR),
                                      (sr & PAGE_SR_ERRORS) | PAGE_SR_EOP);
        *had_error = true;
    }
    return ESP_OK;
}

static esp_err_t page_unlock(tf_target_t *target)
{
    uint32_t cr = 0U;

    if (tf_swd_write_u32(page_reg(target, PAGE_REG_KEYR), PAGE_KEY1) != ESP_OK ||
        tf_swd_write_u32(page_reg(target, PAGE_REG_KEYR), PAGE_KEY2) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tf_swd_read_u32(page_reg(target, PAGE_REG_CR), &cr) != ESP_OK) {
        return ESP_FAIL;
    }
    if ((cr & PAGE_CR_LOCK) != 0U) {
        ESP_LOGE(TF_TAG, "%s: flash not unlocked (CR=0x%08X)", target->family_str,
                 (unsigned)cr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t page_erase(tf_target_t *target, uint32_t addr, uint32_t size)
{
    uint32_t first = 0U;
    uint32_t last = 0U;
    uint32_t offset;
    uint32_t erased = 0U;

    if (addr < target->flash_base) {
        return ESP_ERR_INVALID_ARG;
    }
    offset = addr - target->flash_base;
    if (!tf_stm32_page_cover(&target->page, offset, size, &first, &last)) {
        ESP_LOGE(TF_TAG, "%s: erase range 0x%08X+%u outside flash (%u KiB)",
                 target->family_str, (unsigned)addr, (unsigned)size,
                 (unsigned)(target->flash_size / 1024U));
        return ESP_ERR_INVALID_SIZE;
    }
    if (page_wait_ready(target) != ESP_OK) {
        return ESP_FAIL;
    }
    for (uint32_t page = first; page <= last; page++) {
        uint32_t pnb = 0U;
        uint32_t cr;
        bool bank2 = false;
        bool had_error = false;

        if (!tf_stm32_page_cr_fields(&target->page, page, &pnb, &bank2)) {
            return ESP_FAIL;
        }
        cr = PAGE_CR_PER | ((pnb << PAGE_CR_PNB_SHIFT) & target->page.pnb_mask);
        if (bank2) {
            cr |= (1U << target->page.bker_bit);
        }
        ESP_LOGI(TF_TAG, "%s: erase page %u%s @0x%08X (0x%08X), image 0x%08X..0x%08X",
                 target->family_str, (unsigned)pnb, bank2 ? " bank2" : "",
                 (unsigned)(target->flash_base + tf_stm32_page_offset(&target->page, page)),
                 (unsigned)cr, (unsigned)addr, (unsigned)(addr + size));
        if (tf_swd_write_u32(page_reg(target, PAGE_REG_CR), cr | PAGE_CR_STRT) != ESP_OK) {
            return ESP_FAIL;
        }
        if (tf_swd_wait_clear(page_reg(target, PAGE_REG_SR), page_bsy_mask(&target->page),
                              PAGE_ERASE_TIMEOUT_MS) != ESP_OK) {
            return ESP_FAIL;
        }
        if (page_sr_check(target, &had_error) != ESP_OK) {
            return ESP_FAIL;
        }
        if (had_error) {
            return ESP_FAIL;
        }
        erased++;
    }
    /* Drop PER/PNB unless the controller stays unlocked for programming. */
    if (tf_swd_write_u32(page_reg(target, PAGE_REG_CR), 0U) != ESP_OK) {
        return ESP_FAIL;
    }
    if (erased == 0U) {
        ESP_LOGW(TF_TAG, "%s: no page needed erasing", target->family_str);
    }
    return ESP_OK;
}

static esp_err_t page_program(tf_target_t *target, uint32_t addr, const uint8_t *data,
                              size_t len)
{
    size_t offset = 0U;

    if ((len % PAGE_PROGRAM_UNIT) != 0U || (addr % PAGE_PROGRAM_UNIT) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr < target->flash_base ||
        (addr - target->flash_base) > target->flash_size ||
        len > (size_t)(target->flash_size - (addr - target->flash_base))) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* f1_erase()/page_erase() clears CR, so re-arm PG here. */
    if (tf_swd_write_u32(page_reg(target, PAGE_REG_CR), PAGE_CR_PG) != ESP_OK) {
        return ESP_FAIL;
    }
    while (offset < len) {
        uint32_t unit_addr = addr + (uint32_t)offset;
        const uint8_t *unit = data + offset;
        tf_dap_item_t items[3];
        bool had_error = false;

        items[0].request = TF_REQ_AP_WRITE(TF_AP_TAR);
        items[0].data = unit_addr;
        items[1].request = TF_REQ_AP_WRITE(TF_AP_DRW);
        items[1].data = (uint32_t)unit[0] | ((uint32_t)unit[1] << 8) |
                        ((uint32_t)unit[2] << 16) | ((uint32_t)unit[3] << 24);
        items[2].request = TF_REQ_AP_WRITE(TF_AP_DRW); /* AP auto-increment -> +4 */
        items[2].data = (uint32_t)unit[4] | ((uint32_t)unit[5] << 8) |
                        ((uint32_t)unit[6] << 16) | ((uint32_t)unit[7] << 24);
        if (tf_dap_transfer(items, 3U, NULL, NULL) != ESP_OK) {
            (void)tf_swd_write_u32(page_reg(target, PAGE_REG_CR), 0U);
            return ESP_FAIL;
        }
        if (page_wait_ready(target) != ESP_OK ||
            page_sr_check(target, &had_error) != ESP_OK) {
            (void)tf_swd_write_u32(page_reg(target, PAGE_REG_CR), 0U);
            return ESP_FAIL;
        }
        if (had_error) {
            (void)tf_swd_write_u32(page_reg(target, PAGE_REG_CR), 0U);
            return ESP_FAIL;
        }
        offset += PAGE_PROGRAM_UNIT;
    }
    return tf_swd_write_u32(page_reg(target, PAGE_REG_CR), 0U);
}

static esp_err_t page_program_begin(tf_target_t *target)
{
    if (page_unlock(target) != ESP_OK) {
        return ESP_FAIL;
    }
    if (page_wait_ready(target) != ESP_OK) {
        return ESP_FAIL;
    }
    {
        bool had_error = false;
        if (page_sr_check(target, &had_error) != ESP_OK) {
            return ESP_FAIL;
        }
        (void)had_error; /* stale flags from an earlier session are cleared */
    }
    return ESP_OK;
}

static esp_err_t page_program_end(tf_target_t *target)
{
    return tf_swd_write_u32(page_reg(target, PAGE_REG_CR), PAGE_CR_LOCK);
}

static tf_core_class_t page_kind_core(tf_page_kind_t kind)
{
    return (kind == TF_PAGE_KIND_G0) ? TF_CORE_M0PLUS : TF_CORE_M4;
}

/**
 * Common probe for the page-based families.
 *
 * The caller (tf_target_apply_part) has already run the generic DBGMCU probe
 * and filled target->dev_id/target->idcode/cpuid, so this probe only validates
 * the part against the id table, reads FSIZE/OPTR/UID and computes the page
 * geometry.  ESP_ERR_NOT_SUPPORTED is returned for a valid DEV_ID whose
 * flash-size/OPTR combination is not modelled (the caller then keeps the part
 * as identified-but-unsupported).
 */
static esp_err_t page_probe(tf_target_t *target, tf_page_kind_t kind, const char *name,
                            const char *expected_family)
{
    const tf_stm32_part_t *part;
    uint32_t idcode = target->idcode;
    uint32_t cpuid = 0U;
    uint32_t size_reg = 0U;
    uint32_t optr = 0U;
    uint32_t uid[3] = {0U, 0U, 0U};
    uint32_t dev_id;
    uint32_t flash_size;
    tf_page_geom_t geom;
    tf_core_class_t core;

    dev_id = (uint32_t)target->dev_id;
    if (dev_id == 0U) {
        dev_id = idcode & 0xFFFU;
    }
    part = tf_stm32_part_find((uint16_t)dev_id);
    if (part == NULL || strcmp(part->series, expected_family) != 0) {
        ESP_LOGW(TF_TAG, "%s: unsupported DBGMCU DEV_ID 0x%03X", name, (unsigned)dev_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_REG_CPUID, &cpuid) != ESP_OK) {
        return ESP_FAIL;
    }
    core = tf_stm32_core_from_cpuid(cpuid);
    if (core != page_kind_core(kind)) {
        ESP_LOGW(TF_TAG, "%s: unexpected CPUID 0x%08X (%s, want %s)", name,
                 (unsigned)cpuid, tf_stm32_core_name(core),
                 tf_stm32_core_name(page_kind_core(kind)));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_u32(TF_STM32_FSIZE_1FFF75E0, &size_reg) != ESP_OK) {
        return ESP_FAIL;
    }
    flash_size = (size_reg & 0xFFFFU) * 1024U;
    if (flash_size < 16U * 1024U || flash_size > 2048U * 1024U) {
        ESP_LOGE(TF_TAG, "%s: invalid flash size (FSIZE reg=0x%08X)", name,
                 (unsigned)size_reg);
        return ESP_FAIL;
    }
    {
        /* OPTR resolves the dual-bank option of G4 Cat.3, L4, G0B/C and
         * L4R/S/P/Q.  All families in this driver implement it. */
        uint32_t optr_addr = (kind == TF_PAGE_KIND_WB || kind == TF_PAGE_KIND_WL)
                                 ? (TF_STM32_FLASH_REGS_58004000 +
                                    TF_STM32_FLASH_OPTR_OFFSET)
                                 : (TF_STM32_FLASH_REGS_40022000 +
                                    TF_STM32_FLASH_OPTR_OFFSET);
        if (tf_swd_read_u32(optr_addr, &optr) != ESP_OK) {
            /* Never guess the bank mode: a wrong DBANK assumption would erase
             * the wrong pages on a dual-bank part. */
            ESP_LOGW(TF_TAG, "%s: FLASH_OPTR not readable, marking unsupported", name);
            (void)tf_swd_recover_bus();
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (!tf_stm32_page_geom(kind, (uint16_t)dev_id, flash_size, optr, &geom)) {
        ESP_LOGW(TF_TAG,
                 "%s: DEV_ID=0x%03X flash=%u KiB OPTR=0x%08X not modelled "
                 "(trimmed dual-bank/unsupported size)",
                 name, (unsigned)dev_id, (unsigned)(flash_size / 1024U), (unsigned)optr);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (tf_swd_read_mem(TF_STM32_UID_1FFF7590, uid, sizeof(uid)) != ESP_OK) {
        return ESP_FAIL;
    }

    target->dev_id = (uint16_t)dev_id;
    target->idcode = idcode;
    target->cpuid = cpuid;
    memcpy(target->uid, uid, sizeof(target->uid));
    target->flash_base = TF_MAIN_FLASH_BASE;
    target->flash_size = flash_size;
    target->max_image_size = flash_size;
    target->page = geom;
    snprintf(target->family_str, sizeof(target->family_str), "%s", name);
    ESP_LOGI(TF_TAG,
             "%s detected: DEV_ID=0x%03X REV=0x%04X CPUID=0x%08X (%s) flash=%u KiB "
             "(%s bank, %u KiB pages, %u pages, BKER bit %u, OPTR=0x%08X)",
             name, (unsigned)dev_id, (unsigned)(idcode >> 16), (unsigned)cpuid,
             tf_stm32_core_name(core), (unsigned)(flash_size / 1024U),
             geom.dual_bank ? "dual" : "single", (unsigned)(geom.page_size / 1024U),
             (unsigned)geom.num_pages, (unsigned)geom.bker_bit, (unsigned)optr);
    return ESP_OK;
}

static esp_err_t g4_probe(tf_target_t *target)
{
    return page_probe(target, TF_PAGE_KIND_G4, "stm32g4", "STM32G4");
}

static esp_err_t l4_probe(tf_target_t *target)
{
    return page_probe(target, TF_PAGE_KIND_L4, "stm32l4", "STM32L4");
}

static esp_err_t g0_probe(tf_target_t *target)
{
    return page_probe(target, TF_PAGE_KIND_G0, "stm32g0", "STM32G0");
}

static esp_err_t wb_probe(tf_target_t *target)
{
    return page_probe(target, TF_PAGE_KIND_WB, "stm32wb", "STM32WB");
}

static esp_err_t wl_probe(tf_target_t *target)
{
    return page_probe(target, TF_PAGE_KIND_WL, "stm32wl", "STM32WL");
}

const tf_family_ops_t tf_family_stm32g4_ops = {
    .name = "stm32g4",
    .probe = g4_probe,
    .erase = page_erase,
    .program_begin = page_program_begin,
    .program = page_program,
    .program_end = page_program_end,
};

const tf_family_ops_t tf_family_stm32l4_ops = {
    .name = "stm32l4",
    .probe = l4_probe,
    .erase = page_erase,
    .program_begin = page_program_begin,
    .program = page_program,
    .program_end = page_program_end,
};

const tf_family_ops_t tf_family_stm32g0_ops = {
    .name = "stm32g0",
    .probe = g0_probe,
    .erase = page_erase,
    .program_begin = page_program_begin,
    .program = page_program,
    .program_end = page_program_end,
};

const tf_family_ops_t tf_family_stm32wb_ops = {
    .name = "stm32wb",
    .probe = wb_probe,
    .erase = page_erase,
    .program_begin = page_program_begin,
    .program = page_program,
    .program_end = page_program_end,
};

const tf_family_ops_t tf_family_stm32wl_ops = {
    .name = "stm32wl",
    .probe = wl_probe,
    .erase = page_erase,
    .program_begin = page_program_begin,
    .program = page_program,
    .program_end = page_program_end,
};
