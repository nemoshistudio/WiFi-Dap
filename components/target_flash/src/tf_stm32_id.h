/*
 * SPDX-License-Identifier: MIT
 *
 * Static STM32 DBGMCU DEV_ID -> series/model identification table.
 *
 * Generated from the ST-Link device database (stlink-org/stlink,
 * config/chips (one .chip file per device group), master 2026-07-27, BSD-3-Clause; each entry keeps the
 * source file name and reference-manual id in a comment).  Cross-checked
 * against OpenOCD master tcl/target/stm32*.cfg (DBGMCU base addresses) and
 * the STM32 reference manuals for UID/flash-size register addresses.
 *
 * Notes / limitations:
 *  - DEV_ID identifies a device group, not a unique order code.  The model
 *    string is therefore a "/"-separated candidate list for that group; the
 *    flash size read at runtime narrows it down (H7/F4 special cases) and the
 *    standard order-code density letter is appended for classic families
 *    (see tf_stm32_model_candidates()).  Package and full order code can
 *    never be read over SWD; the Web layer compares the detected group with a
 *    user-selected MCU/board profile instead.
 *  - Everything here is host-testable pure data/helpers: no ESP-IDF include.
 *  - DBGMCU base: 0xE0042000 (F1/F2/F3/F4/F7/L1/L4/WB/WL), 0x40015800
 *    (F0/G0/G4/L0/U0/C0), 0x5C001000 (H7), 0xE0044000 (L5/U5/H5/C5).
 *  - DBGMCU IDCODE layout: bits[11:0] DEV_ID, bits[31:16] REV_ID.
 *  - flash_size_reg is the 32-bit aligned word address and the FSIZE field
 *    is the 16-bit half selected by flash_size_shift; only sane 16..8192 KiB
 *    values are accepted at runtime (WB0 groups have no in-system FSIZE and
 *    use flash_size_shift=0xFF to skip the read).
 *
 * Do not edit by hand: regenerate from the source data if the table grows.
 */
#ifndef TF_STM32_ID_H
#define TF_STM32_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Cortex-M core class (CPUID PARTNO family). */
typedef enum {
    TF_CORE_UNKNOWN = 0,
    TF_CORE_M0,
    TF_CORE_M0PLUS,
    TF_CORE_M3,
    TF_CORE_M4,
    TF_CORE_M7,
    TF_CORE_M33,
} tf_core_class_t;

/** Flash implementations that can actually program the part. */
enum {
    TF_STM32_FLASH_OPS_NONE = 0,
    TF_STM32_FLASH_OPS_F4 = 1,   /* tf_stm32f4.c (FLASH regs 0x40023C00) */
    TF_STM32_FLASH_OPS_H7 = 2,   /* tf_stm32h7.c (FLASH regs 0x52002000) */
};

typedef struct {
    uint16_t dev_id;          /**< DBGMCU IDCODE bits[11:0] */
    uint8_t core;             /**< expected Cortex-M core class */
    uint8_t flash_ops;        /**< TF_STM32_FLASH_OPS_* (0 = identify only) */
    uint8_t flash_size_shift; /**< 0/16, or 0xFF when no FSIZE register */
    const char *series;       /**< "STM32H7" */
    const char *model;        /**< candidate model list, e.g. "STM32H743/H750" */
    uint32_t dbg_base;        /**< DBGMCU register block base */
    uint32_t flash_size_reg;  /**< aligned 32-bit word with the FSIZE field */
    uint32_t uid_base;        /**< 96-bit UID base, 0 = unknown location */
} tf_stm32_part_t;

extern const tf_stm32_part_t tf_stm32_part_table[];
extern const size_t tf_stm32_part_table_size;

/** dev_id lookup (table sorted by dev_id; NULL when unknown). */
const tf_stm32_part_t *tf_stm32_part_find(uint16_t dev_id);

/** CPUID -> core class (uses ARM PARTNO in bits[15:4]). */
tf_core_class_t tf_stm32_core_from_cpuid(uint32_t cpuid);
/** Human-readable core class ("Cortex-M7", ...). */
const char *tf_stm32_core_name(tf_core_class_t core);
/** Candidate DBGMCU bases ordered by CPU core class (returns count). */
size_t tf_stm32_dbgmc_probe_bases(tf_core_class_t core, const uint32_t **bases);

/**
 * Detection-confidence class for a DEV_ID match (mirrors the Web/API
 * `detection_confidence` field: unknown / group / flash-narrowed).
 */
typedef enum {
    TF_NARROW_UNKNOWN = 0, /**< DEV_ID not in the table (or DBGMCU unreadable) */
    TF_NARROW_GROUP = 1,   /**< DEV_ID matched, flash size unreadable: group only */
    TF_NARROW_FLASH = 2,   /**< DEV_ID matched + valid FSIZE: capacity narrowed */
} tf_narrow_t;

/**
 * Standard STM32 order-code flash-density suffix for a flash size in bytes:
 * "x4" (16 KiB), "x6" (32 KiB), "x8" (64 KiB), "xB" (128 KiB), "xC"
 * (256 KiB), "xD" (384 KiB), "xE" (512 KiB), "xF" (768 KiB), "xG" (1024 KiB),
 * "xI" (2048 KiB).  NULL for non-standard / zero sizes.
 */
const char *tf_stm32_flash_density_code(uint32_t flash_size);

/**
 * True when the flash-density letter in the order code is well defined for
 * this DEV_ID group (classic F0/F1/F2/F3/F4/F7/G0/G4/H7/L0/L1/L4 groups).
 * Used to decide whether tf_stm32_model_candidates() may append "xN".
 */
bool tf_stm32_flash_density_applies(uint16_t dev_id);

/**
 * Narrow the "/"-separated candidate group with the DEV_ID + flash size.
 *
 * Keeps the historical H7/F4 special cases (H750 value line = 128 KiB,
 * H74x/H75x >= 2 MiB drops the H750, H72x/H73x single-bank group at 1 MiB)
 * and otherwise returns `table_model` unchanged.  Result points to `buf` for
 * the special cases and to `table_model` when no narrowing applies.
 */
const char *tf_stm32_model_group(uint16_t dev_id, uint32_t flash_size,
                                 const char *table_model, char *buf, size_t buf_len);

/**
 * Candidate string for the Web/API `model_candidates` field: the narrowed
 * group with the detected FSIZE hint appended for flash-narrowed detections,
 * e.g. "STM32F100/F101/F102/F103 (MD) [x8, 64 KiB]".  When the flash size is
 * unknown the group string is returned unchanged (group-level confidence).
 * `confidence` (optional) receives the matching TF_NARROW_* class.
 */
const char *tf_stm32_model_candidates(uint16_t dev_id, uint32_t flash_size,
                                      const char *table_model, char *buf, size_t buf_len,
                                      tf_narrow_t *confidence);

#ifdef __cplusplus
}
#endif

#endif /* TF_STM32_ID_H */
