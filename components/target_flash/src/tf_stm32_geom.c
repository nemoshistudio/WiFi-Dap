/*
 * SPDX-License-Identifier: MIT
 *
 * Pure STM32 flash geometry helpers - see tf_stm32_geom.h for the reference
 * manuals and the limitations.  Host-testable: no ESP-IDF include.
 */
#include "tf_stm32_geom.h"

#include <string.h>

#define TF_KIB 1024U

/* FLASH_OPTR dual-bank option bits (per family). */
#define OPTR_L4_DBANK_BIT   21U  /* L47/L48, L49/L4A, G0B/G0C */
#define OPTR_L4R_DBANK_BIT  22U  /* L4R/L4S (max size), G4 Cat.3 */
#define OPTR_L4R_DB1M_BIT   21U  /* L4R/L4S below max, L4P/L4Q below max */

/* F1-style DEV_IDs. */
#define DEV_F1_MD   0x410U
#define DEV_F1_LD   0x412U
#define DEV_F1_HD   0x414U
#define DEV_F1_CL   0x418U
#define DEV_F1_VLMD 0x420U
#define DEV_F1_VLHD 0x428U
#define DEV_F1_XL   0x430U
#define DEV_F3_302  0x422U
#define DEV_F3_37x  0x432U
#define DEV_F3_33x  0x438U
#define DEV_F3_301  0x439U
#define DEV_F3_HD   0x446U
#define DEV_F0_05x  0x440U
#define DEV_F0_09x  0x442U
#define DEV_F0_03x  0x444U
#define DEV_F0_04x  0x445U
#define DEV_F0_07x  0x448U

/* G4 DEV_IDs. */
#define DEV_G4_43x  0x468U
#define DEV_G4_47x  0x469U
#define DEV_G4_49x  0x479U

/* L4 DEV_IDs. */
#define DEV_L4_47x  0x415U
#define DEV_L4_43x  0x435U
#define DEV_L4_49x  0x461U
#define DEV_L4_45x  0x462U
#define DEV_L4_41x  0x464U
#define DEV_L4R_S   0x470U
#define DEV_L4P_Q   0x471U

/* G0 DEV_IDs. */
#define DEV_G0_05x  0x456U
#define DEV_G0_07x  0x460U
#define DEV_G0_03x  0x466U
#define DEV_G0_0BxC 0x467U

/* WB / WL DEV_IDs. */
#define DEV_WB_5xx  0x495U
#define DEV_WL_Ex   0x497U

/* F2/F4/F7 DEV_IDs. */
#define DEV_F2_20x  0x411U
#define DEV_F4_40x  0x413U
#define DEV_F4_19x  0x419U
#define DEV_F4_46x  0x421U
#define DEV_F4_401B 0x423U
#define DEV_F4_411  0x431U
#define DEV_F4_401D 0x433U
#define DEV_F4_46x9 0x434U
#define DEV_F4_412  0x441U
#define DEV_F4_410  0x458U
#define DEV_F4_413  0x463U
#define DEV_F7_74x  0x449U
#define DEV_F7_76x  0x451U
#define DEV_F7_72x  0x452U

static bool is_pow2(uint32_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

static void page_geom_defaults(tf_page_geom_t *geom, uint32_t regs_base,
                               uint32_t idcode_addr)
{
    memset(geom, 0, sizeof(*geom));
    geom->regs_base = regs_base;
    geom->idcode_addr = idcode_addr;
    geom->fsize_addr = TF_STM32_FSIZE_1FFF75E0;
    geom->fsize_shift = 0U;
    geom->uid_addr = TF_STM32_UID_1FFF7590;
    geom->optr_addr = regs_base + TF_STM32_FLASH_OPTR_OFFSET;
    geom->bker_bit = 11U;
    geom->pnb_mask = 0xFFU << 3; /* 8 bits; G0B/G0C widen it below */
}

static bool page_geom_finish(tf_page_geom_t *geom, uint32_t flash_size)
{
    if (geom->page_size == 0U || !is_pow2(geom->page_size)) {
        return false;
    }
    if (geom->max_flash_size != 0U && flash_size > geom->max_flash_size) {
        return false;
    }
    if (flash_size < geom->page_size || (flash_size % geom->page_size) != 0U) {
        return false;
    }
    geom->num_pages = flash_size / geom->page_size;
    if (geom->num_pages == 0U || geom->num_pages > 4096U) {
        return false;
    }
    if (geom->dual_bank) {
        if (geom->num_pages < 2U || (geom->num_pages % 2U) != 0U) {
            return false;
        }
        geom->bank1_pages = geom->num_pages / 2U;
    } else {
        geom->bank1_pages = geom->num_pages;
    }
    return true;
}

bool tf_stm32_page_geom(tf_page_kind_t kind, uint16_t dev_id, uint32_t flash_size,
                        uint32_t optr, tf_page_geom_t *out)
{
    tf_page_geom_t geom;

    if (out == NULL || flash_size == 0U) {
        return false;
    }

    switch (kind) {
    case TF_PAGE_KIND_G4:
        page_geom_defaults(&geom, TF_STM32_FLASH_REGS_40022000,
                           TF_STM32_DBGMCU_IDCODE_E0042000);
        switch (dev_id) {
        case DEV_G4_43x: /* G431/G441: Cat.2, single bank, 2 KiB pages, 128 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 128U * TF_KIB;
            break;
        case DEV_G4_49x: /* G491/G4A1: Cat.4, single bank, 2 KiB pages, 512 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 512U * TF_KIB;
            break;
        case DEV_G4_47x: /* G47x/G48x: Cat.3, single 4 KiB / dual 2 KiB pages */
            geom.max_flash_size = 512U * TF_KIB;
            geom.dual_bank = (optr & (1U << OPTR_L4R_DBANK_BIT)) != 0U;
            geom.page_size = geom.dual_bank ? (2U * TF_KIB) : (4U * TF_KIB);
            /* Trimmed Cat.3 devices place a gap between the banks (OpenOCD
             * stm32l4x.c); those are rejected by the mapping checks below. */
            if (geom.dual_bank && flash_size != geom.max_flash_size) {
                return false;
            }
            break;
        default:
            return false;
        }
        break;

    case TF_PAGE_KIND_L4:
        page_geom_defaults(&geom, TF_STM32_FLASH_REGS_40022000,
                           TF_STM32_DBGMCU_IDCODE_E0042000);
        switch (dev_id) {
        case DEV_L4_47x: /* L47x/L48x: 2 KiB pages, dual bank option bit 21 */
        case DEV_L4_49x: /* L49x/L4Ax: same controller */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 1024U * TF_KIB;
            geom.dual_bank = (flash_size == geom.max_flash_size) ||
                             ((optr & (1U << OPTR_L4_DBANK_BIT)) != 0U);
            break;
        case DEV_L4_43x: /* L43x/L44x: single bank, 2 KiB pages */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 256U * TF_KIB;
            break;
        case DEV_L4_45x: /* L45x/L46x: single bank, 2 KiB pages */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 512U * TF_KIB;
            break;
        case DEV_L4_41x: /* L41x/L42x: single bank, 2 KiB pages */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 128U * TF_KIB;
            break;
        case DEV_L4R_S: /* L4R/L4S: 8 KiB pages single, 4 KiB pages dual */
            geom.max_flash_size = 2048U * TF_KIB;
            geom.dual_bank = (flash_size == geom.max_flash_size)
                                 ? ((optr & (1U << OPTR_L4R_DBANK_BIT)) != 0U)
                                 : ((optr & (1U << OPTR_L4R_DB1M_BIT)) != 0U);
            geom.page_size = geom.dual_bank ? (4U * TF_KIB) : (8U * TF_KIB);
            break;
        case DEV_L4P_Q: /* L4P/L4Q: 8 KiB pages single, 4 KiB pages dual */
            geom.max_flash_size = 1024U * TF_KIB;
            geom.dual_bank = (flash_size == geom.max_flash_size)
                                 ? ((optr & (1U << OPTR_L4R_DBANK_BIT)) != 0U)
                                 : ((optr & (1U << OPTR_L4R_DB1M_BIT)) != 0U);
            geom.page_size = geom.dual_bank ? (4U * TF_KIB) : (8U * TF_KIB);
            break;
        default:
            return false;
        }
        break;

    case TF_PAGE_KIND_G0:
        page_geom_defaults(&geom, TF_STM32_FLASH_REGS_40022000,
                           TF_STM32_DBGMCU_IDCODE_40015800);
        switch (dev_id) {
        case DEV_G0_05x: /* G05x/G06x: 2 KiB pages, 64 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 64U * TF_KIB;
            break;
        case DEV_G0_07x: /* G07x/G08x: 2 KiB pages, 128 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 128U * TF_KIB;
            break;
        case DEV_G0_03x: /* G03x/G04x: 2 KiB pages, 64 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 64U * TF_KIB;
            break;
        case DEV_G0_0BxC: /* G0Bx/G0Cx: 2 KiB pages both modes, 512 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 512U * TF_KIB;
            geom.bker_bit = 13U; /* G0 uses CR bit 13 as the bank select */
            geom.bsy2_shift = 17U;
            geom.cfgbsy_shift = 18U;
            /* G0B/G0C page numbers use a global encoding: bank 2 pages are
             * numbered 256 + page-in-bank (BKER set), which is why the PNB
             * field is 10 bits wide on this group. */
            geom.pnb_mask = 0x3FFU << 3;
            geom.bank2_pnb_base = 256U;
            /* All G0Bx/G0Cx parts above 128 KiB have two physical banks; the
             * DUAL_BANK option bit does not change the address mapping (Zephyr
             * flash_stm32g0x.c, tested on G0B1RE).  nSWAP_BANK (OPTR bit20,
             * active low) swaps the bank1/bank2 address mapping. */
            geom.dual_bank = (flash_size > (128U * TF_KIB));
            geom.bank_swap = geom.dual_bank && ((optr & (1U << 20)) == 0U);
            break;
        default:
            return false;
        }
        break;

    case TF_PAGE_KIND_WB:
        page_geom_defaults(&geom, TF_STM32_FLASH_REGS_58004000,
                           TF_STM32_DBGMCU_IDCODE_E0042000);
        switch (dev_id) {
        case DEV_WB_5xx: /* WB5x: single bank, 4 KiB pages, 1 MiB max */
            geom.page_size = 4U * TF_KIB;
            geom.max_flash_size = 1024U * TF_KIB;
            break;
        default:
            return false;
        }
        geom.bker_bit = 0U;
        break;

    case TF_PAGE_KIND_WL:
        page_geom_defaults(&geom, TF_STM32_FLASH_REGS_58004000,
                           TF_STM32_DBGMCU_IDCODE_E0042000);
        switch (dev_id) {
        case DEV_WL_Ex: /* WLE5/WL55: single bank, 2 KiB pages, 256 KiB max */
            geom.page_size = 2U * TF_KIB;
            geom.max_flash_size = 256U * TF_KIB;
            break;
        default:
            return false;
        }
        geom.bker_bit = 0U;
        break;

    default:
        return false;
    }

    if (!page_geom_finish(&geom, flash_size)) {
        return false;
    }
    *out = geom;
    return true;
}

uint32_t tf_stm32_page_offset(const tf_page_geom_t *geom, uint32_t page)
{
    if (geom == NULL || page >= geom->num_pages) {
        return UINT32_MAX;
    }
    return page * geom->page_size;
}

bool tf_stm32_page_index(const tf_page_geom_t *geom, uint32_t offset, uint32_t *page)
{
    uint32_t index;

    if (geom == NULL || page == NULL) {
        return false;
    }
    index = offset / geom->page_size;
    if (index >= geom->num_pages) {
        return false;
    }
    *page = index;
    return true;
}

bool tf_stm32_page_cr_fields(const tf_page_geom_t *geom, uint32_t page,
                             uint32_t *pnb, bool *bank2)
{
    bool in_bank2 = false;
    uint32_t index;

    if (geom == NULL || page >= geom->num_pages) {
        return false;
    }
    if (geom->dual_bank) {
        bool lower = page < geom->bank1_pages;

        in_bank2 = geom->bank_swap ? lower : !lower;
        index = lower ? page : (page - geom->bank1_pages);
        if (in_bank2) {
            index += geom->bank2_pnb_base;
        }
    } else {
        index = page;
    }
    if (in_bank2 && geom->bker_bit == 0U) {
        return false;
    }
    /* The PNB field is at least 7 bits wide (8 bits on L4R/S/P/Q and WB,
     * 10 bits on G0B/G0C); reject values the configured mask cannot hold. */
    if (geom->pnb_mask == 0U || (index & ~(geom->pnb_mask >> 3)) != 0U) {
        return false;
    }
    if (pnb != NULL) {
        *pnb = index;
    }
    if (bank2 != NULL) {
        *bank2 = in_bank2;
    }
    return true;
}

bool tf_stm32_page_cover(const tf_page_geom_t *geom, uint32_t offset, uint32_t size,
                         uint32_t *first_page, uint32_t *last_page)
{
    uint32_t first;
    uint32_t last;

    if (geom == NULL || size == 0U) {
        return false;
    }
    if (offset > (UINT32_MAX - size)) {
        return false;
    }
    if (!tf_stm32_page_index(geom, offset, &first) ||
        !tf_stm32_page_index(geom, offset + size - 1U, &last)) {
        return false;
    }
    if (first_page != NULL) {
        *first_page = first;
    }
    if (last_page != NULL) {
        *last_page = last;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* F1-style page controller (F0/F1/F3)                                        */
/* ------------------------------------------------------------------------- */

bool tf_stm32_f1_geom(uint16_t dev_id, uint32_t flash_size, tf_f1_geom_t *out)
{
    tf_f1_geom_t geom;
    uint32_t max_flash;

    if (out == NULL || flash_size == 0U) {
        return false;
    }
    memset(&geom, 0, sizeof(geom));
    geom.regs_base = TF_STM32_FLASH_REGS_40022000;

    switch (dev_id) {
    case DEV_F0_05x: /* F05x: 1 KiB pages, 64 KiB max */
        geom.page_size = 1U * TF_KIB;
        max_flash = 64U * TF_KIB;
        break;
    case DEV_F0_03x: /* F03x */
    case DEV_F0_04x: /* F04x */
        geom.page_size = 1U * TF_KIB;
        max_flash = 32U * TF_KIB;
        break;
    case DEV_F0_07x: /* F07x: 2 KiB pages, 128 KiB max */
        geom.page_size = 2U * TF_KIB;
        max_flash = 128U * TF_KIB;
        break;
    case DEV_F0_09x: /* F09x: 2 KiB pages, 256 KiB max */
        geom.page_size = 2U * TF_KIB;
        max_flash = 256U * TF_KIB;
        break;
    case DEV_F1_LD: /* F1 low density: 1 KiB pages, 32 KiB max */
        geom.page_size = 1U * TF_KIB;
        max_flash = 32U * TF_KIB;
        break;
    case DEV_F1_MD: /* F1 medium density: 1 KiB pages, 128 KiB max */
    case DEV_F1_VLMD: /* F100 value line medium density */
        geom.page_size = 1U * TF_KIB;
        max_flash = 128U * TF_KIB;
        break;
    case DEV_F1_HD: /* F1 high density: 2 KiB pages, 512 KiB max */
    case DEV_F1_CL: /* F1 connectivity line: 2 KiB pages, 256 KiB max */
    case DEV_F1_VLHD: /* F100 value line high density */
        geom.page_size = 2U * TF_KIB;
        max_flash = (dev_id == DEV_F1_CL) ? (256U * TF_KIB) : (512U * TF_KIB);
        break;
    case DEV_F1_XL: /* F1 XL density: 2 KiB pages, two banks, 1 MiB max */
        geom.page_size = 2U * TF_KIB;
        max_flash = 1024U * TF_KIB;
        geom.bank2_addr = 0x08080000U;
        geom.bank_reg_offset = 0x40U;
        break;
    case DEV_F3_302:
    case DEV_F3_37x:
    case DEV_F3_33x:
    case DEV_F3_301:
    case DEV_F3_HD:
        geom.page_size = 2U * TF_KIB;
        max_flash = 512U * TF_KIB;
        break;
    default:
        return false;
    }

    if (flash_size > max_flash || flash_size < geom.page_size ||
        (flash_size % geom.page_size) != 0U || !is_pow2(geom.page_size)) {
        return false;
    }
    geom.num_pages = flash_size / geom.page_size;
    if (geom.num_pages == 0U || geom.num_pages > 1024U) {
        return false;
    }
    if (dev_id == DEV_F1_XL && flash_size > (512U * TF_KIB)) {
        /* Bank 1 is fixed at 512 KiB; bank 2 starts at 0x08080000. */
        geom.dual_bank = true;
        geom.bank1_pages = (512U * TF_KIB) / geom.page_size;
    } else {
        geom.dual_bank = false;
        geom.bank1_pages = geom.num_pages;
    }
    *out = geom;
    return true;
}

/* ------------------------------------------------------------------------- */
/* F2/F4/F7 sector map                                                        */
/* ------------------------------------------------------------------------- */

/* Max sector size in KiB per DEV_ID group (OpenOCD stm32f2x.c):
 * F2/F4 = 128 KiB, F74x/F75x = 256 KiB, F72x/F73x = 128 KiB. */
static uint32_t sector_max_size_kb(uint16_t dev_id)
{
    switch (dev_id) {
    case DEV_F7_74x: /* F74x/F75x */
    case DEV_F7_76x: /* F76x/F77x: dual-bank 2 MiB not modelled */
        return 256U;
    case DEV_F7_72x:
        return 128U;
    case DEV_F2_20x:
    case DEV_F4_40x:
    case DEV_F4_46x:
    case DEV_F4_401B:
    case DEV_F4_411:
    case DEV_F4_401D:
    case DEV_F4_412:
    case DEV_F4_410:
        return 128U;
    default:
        return 0U;
    }
}

uint32_t tf_stm32_sector_max_flash(uint16_t dev_id)
{
    switch (dev_id) {
    case DEV_F2_20x:
        return 1024U * TF_KIB;
    case DEV_F4_40x:
        return 1024U * TF_KIB;
    case DEV_F4_401B:
        return 256U * TF_KIB;
    case DEV_F4_46x:
    case DEV_F4_411:
    case DEV_F4_401D:
    case DEV_F4_412:
        return 512U * TF_KIB;
    case DEV_F4_410:
        return 128U * TF_KIB;
    case DEV_F7_74x:
        return 1024U * TF_KIB;
    case DEV_F7_72x:
        return 512U * TF_KIB;
    case DEV_F7_76x:
        return 2048U * TF_KIB;
    default:
        return 0U;
    }
}

size_t tf_stm32_sector_map(uint16_t dev_id, uint32_t flash_size,
                           tf_sector_t *sectors, size_t cap)
{
    uint32_t max_sector_kb = sector_max_size_kb(dev_id);
    uint32_t max_flash = tf_stm32_sector_max_flash(dev_id);
    uint32_t remaining_kb;
    uint32_t index = 0U;
    size_t count = 0U;

    if (max_sector_kb == 0U || sectors == NULL || cap == 0U ||
        flash_size == 0U || (flash_size % TF_KIB) != 0U) {
        return 0U;
    }
    if (max_flash == 0U || flash_size > max_flash) {
        return 0U;
    }
    /* Dual-bank sector numbering (SNB|0x10) is not modelled here: the 2 MiB
     * F76x/F77x dual-bank layout needs a bank-gap sector map and a second
     * bank-aware erase path, so those parts stay identified-only. */
    if (dev_id == DEV_F7_76x) {
        return 0U;
    }
    /* F42x/F43x/F469/F479 2 MiB parts are dual-bank only. */
    if (dev_id == DEV_F4_19x || dev_id == DEV_F4_46x9) {
        return 0U;
    }

    remaining_kb = flash_size / TF_KIB;
    while (remaining_kb > 0U && count < cap) {
        uint32_t size_kb;

        if (index < 4U) {
            size_kb = max_sector_kb / 8U;  /* 16 KiB (F2/F4) / 32 KiB (F7) */
        } else if (index == 4U) {
            size_kb = max_sector_kb / 2U;  /* 64 KiB / 128 KiB */
        } else {
            size_kb = max_sector_kb;       /* 128 KiB / 256 KiB */
        }
        if (size_kb > remaining_kb) {
            size_kb = remaining_kb;        /* clip the last sector */
        }
        sectors[count].offset = (flash_size / TF_KIB - remaining_kb) * TF_KIB;
        sectors[count].size = size_kb * TF_KIB;
        remaining_kb -= size_kb;
        index++;
        count++;
    }
    return (remaining_kb == 0U) ? count : 0U;
}
