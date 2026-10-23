/*
 * SPDX-License-Identifier: MIT
 *
 * Pure STM32 flash geometry helpers shared by the family flash drivers.
 *
 * Everything in this file/module is host-testable: it only maps a DBGMCU
 * DEV_ID + the FLASH size (and, for dual-bank parts, the FLASH_OPTR option
 * register) to the page/sector geometry and the controller registers that the
 * family driver needs.  No ESP-IDF or SWD dependency.
 *
 * References:
 *   - RM0440 (STM32G4): page 2 KiB (single-bank Cat.2/Cat.4, dual-bank
 *     Cat.3) / 4 KiB (Cat.3 single-bank), FLASH_OPTR.DBANK bit 22;
 *   - RM0351/RM0394/RM0432 (STM32L4): page 2 KiB, 4/8 KiB for L4R/S/P/Q,
 *     FLASH_OPTR DBANK/DB1M/DB512K bits;
 *   - RM0444/RM0454 (STM32G0): page 2 KiB, dual-bank bit 21 and BKER bit 13;
 *   - RM0434 (STM32WB55) / RM0461 (STM32WLEx): FLASH at 0x58004000,
 *     page 4 KiB (WB) / 2 KiB (WL);
 *   - RM0008 (STM32F1), RM0091 (STM32F0), RM0316 (STM32F3): page 1/2 KiB,
 *     F1 XL-density has a second controller block at +0x40 and bank 2 at
 *     0x08080000;
 *   - RM0090/RM0033 (STM32F4/F2), RM0385/RM0410/RM0431 (STM32F7):
 *     16+64+128 KiB sector map (F2/F4), 32+128+256 KiB map with clipping
 *     (F7, max sector 256 KiB) - matches OpenOCD stm32f2x.c.
 */
#ifndef TF_STM32_GEOM_H
#define TF_STM32_GEOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DBGMCU / factory register addresses (32-bit aligned word addresses). */
#define TF_STM32_DBGMCU_IDCODE_E0042000 0xE0042000U
#define TF_STM32_DBGMCU_IDCODE_40015800 0x40015800U
#define TF_STM32_FSIZE_1FFF75E0         0x1FFF75E0U  /* low half-word: KiB */
#define TF_STM32_UID_1FFF7590           0x1FFF7590U
#define TF_STM32_FLASH_REGS_40022000    0x40022000U
#define TF_STM32_FLASH_REGS_58004000    0x58004000U
#define TF_STM32_FLASH_OPTR_OFFSET      0x20U

/* ------------------------------------------------------------------------- */
/* L4-style page controller (G4/L4/G0/WB/WL)                                  */
/* ------------------------------------------------------------------------- */

/** Page-controller sub-family (register layout is identical; geometry differs). */
typedef enum {
    TF_PAGE_KIND_G4 = 0,
    TF_PAGE_KIND_L4,
    TF_PAGE_KIND_G0,
    TF_PAGE_KIND_WB,
    TF_PAGE_KIND_WL,
} tf_page_kind_t;

/** Geometry + probe addresses of one page-based STM32 part. */
typedef struct {
    uint32_t idcode_addr;  /**< DBGMCU_IDCODE address */
    uint32_t fsize_addr;   /**< word containing the FSIZE half-word */
    uint32_t fsize_shift;  /**< 0 = low half-word, 16 = high half-word */
    uint32_t uid_addr;     /**< 96-bit UID base */
    uint32_t regs_base;    /**< FLASH register block base */
    uint32_t optr_addr;    /**< FLASH_OPTR address */
    uint32_t page_size;    /**< bytes per erasable page */
    uint32_t num_pages;    /**< total mapped pages */
    uint32_t bank1_pages;  /**< pages in bank 1 (== num_pages when single) */
    uint32_t max_flash_size; /**< bytes, datasheet maximum for this DEV_ID group */
    uint32_t pnb_mask;     /**< FLASH_CR PNB mask (already shifted to bits 3..) */
    uint32_t bank2_pnb_base; /**< G0B/G0C: physical bank-2 page number base (256) */
    uint8_t bker_bit;      /**< CR bank-select bit (0 = no bank bit used) */
    uint8_t bsy2_shift;    /**< extra BSY bit shift (17 for G0B/G0C, 0 = none) */
    uint8_t cfgbsy_shift;  /**< CFGBSY bit shift (18 for G0B/G0C, 0 = none) */
    bool dual_bank;        /**< opt-in dual-bank layout active */
    bool bank_swap;        /**< G0B/G0C nSWAP_BANK=0: lower addresses map to bank 2 */
} tf_page_geom_t;

/**
 * Compute the page geometry for a page-based part.
 *
 * @param kind       controller sub-family.
 * @param dev_id     DBGMCU DEV_ID.
 * @param flash_size FSIZE in bytes (must be > 0, page aligned).
 * @param optr       FLASH_OPTR register value (read at runtime).
 * @param[out] out   geometry on success.
 * @return false when the DEV_ID/flash-size/OPTR combination is not modelled
 *         (the caller must report the part as identified-but-unsupported).
 */
bool tf_stm32_page_geom(tf_page_kind_t kind, uint16_t dev_id, uint32_t flash_size,
                        uint32_t optr, tf_page_geom_t *out);

/** Byte offset (relative to the flash base) of a mapped page, or UINT32_MAX
 *  for an out-of-range page. */
uint32_t tf_stm32_page_offset(const tf_page_geom_t *geom, uint32_t page);

/** Map a byte offset to a mapped page index (false when out of range). */
bool tf_stm32_page_index(const tf_page_geom_t *geom, uint32_t offset, uint32_t *page);

/** Map a page index to the FLASH_CR PNB field value and the bank-2 select flag. */
bool tf_stm32_page_cr_fields(const tf_page_geom_t *geom, uint32_t page,
                             uint32_t *pnb, bool *bank2);

/** First/last mapped page covering [offset, offset+size); false when the range
 *  leaves the flash. */
bool tf_stm32_page_cover(const tf_page_geom_t *geom, uint32_t offset, uint32_t size,
                         uint32_t *first_page, uint32_t *last_page);

/* ------------------------------------------------------------------------- */
/* F1-style page controller (F0/F1/F3)                                        */
/* ------------------------------------------------------------------------- */

/** Geometry of one F1-style part (page erase by address, 16-bit programming). */
typedef struct {
    uint32_t regs_base;       /**< FLASH register block base (bank 1) */
    uint32_t page_size;       /**< bytes per erasable page */
    uint32_t num_pages;
    uint32_t bank1_pages;
    uint32_t bank2_addr;      /**< F1 XL bank 2 flash address, 0 = single bank */
    uint32_t bank_reg_offset; /**< register offset of bank 2 (0x40 for F1 XL) */
    bool dual_bank;
} tf_f1_geom_t;

bool tf_stm32_f1_geom(uint16_t dev_id, uint32_t flash_size, tf_f1_geom_t *out);

/* ------------------------------------------------------------------------- */
/* F2/F4/F7 sector map                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t offset;
    uint32_t size;
} tf_sector_t;

/** Maximum flash size (bytes) the sector map supports for a DEV_ID group. */
uint32_t tf_stm32_sector_max_flash(uint16_t dev_id);

/** Fill the sector map (offset/size pairs in ascending order) for a single-bank
 *  F2/F4/F7 part.  Returns the number of sectors, 0 when the DEV_ID is not
 *  modelled (dual-bank 2 MiB parts are intentionally excluded). */
size_t tf_stm32_sector_map(uint16_t dev_id, uint32_t flash_size,
                           tf_sector_t *sectors, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TF_STM32_GEOM_H */
