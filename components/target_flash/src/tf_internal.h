/*
 * SPDX-License-Identifier: MIT
 *
 * Internal types/constants shared by the target flash engine modules.
 * Not a public header (web layer only consumes include/wt_target_flash.h).
 */
#ifndef TF_INTERNAL_H
#define TF_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <sdkconfig.h>

#include "wt_target_flash.h"
#include "wt_target_flash_debug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TF_TAG "target_flash"

/* One CMSIS-DAP packet (request or response) = DAP_PACKET_SIZE. */
#define TF_DAP_PACKET_SIZE       512U

/* DAP hand-off / restore timeouts. */
#define TF_DAP_HANDOFF_TIMEOUT_MS 3000U
#define TF_DAP_RESTORE_TIMEOUT_MS 1000U

/* SWD clock requested from the DAP engine (hardware-verified default). */
#define TF_SWD_CLOCK_HZ          2000000U

/* Cortex-M debug/PPB registers (accessed through the AHB-AP). */
#define TF_REG_CPUID             0xE000ED00U
#define TF_REG_DHCSR             0xE000EDF0U
#define TF_REG_DCRSR             0xE000EDF4U
#define TF_REG_DCRDR             0xE000EDF8U
#define TF_REG_DEMCR             0xE000EDFCU
#define TF_REG_AIRCR             0xE000ED0CU

#define TF_MAIN_FLASH_BASE       0x08000000U

/* DBGMCU identification addresses. */
#define TF_F4_DBGMCU_IDCODE      0xE0042000U
#define TF_F4_UID_BASE           0x1FFF7A10U
#define TF_F4_FLASH_SIZE_REG     0x1FFF7A20U   /* FSIZE is the high halfword */
#define TF_H7_DBGMCU_IDCODE      0x5C001000U
#define TF_H7_UID_BASE           0x1FF1E800U
#define TF_H7_FLASH_SIZE_REG     0x1FF1E880U   /* FSIZE is the low halfword (KB) */

/* Cortex-M core debug bits (DHCSR). */
#define TF_DHCSR_DBGKEY          0xA05F0000U
#define TF_DHCSR_C_DEBUGEN       0x00000001U
#define TF_DHCSR_C_HALT          0x00000002U
#define TF_DHCSR_S_REGRDY        0x00010000U
#define TF_DHCSR_S_HALT          0x00020000U
#define TF_DHCSR_S_LOCKUP        0x00080000U
#define TF_DCRSR_REGSEL_PC       15U

/* AIRCR: VECTKEY | SYSRESETREQ. */
#define TF_AIRCR_SYSRESETREQ     0x05FA0004U

/* DAP request byte encoding: bit0 APnDP, bit1 RnW, bits3:2 A[3:2]. */
#define TF_REQ_DP_WRITE(reg)     (0x00U | ((((uint32_t)(reg) >> 2) & 0x3U) << 2))
#define TF_REQ_DP_READ(reg)      (0x02U | ((((uint32_t)(reg) >> 2) & 0x3U) << 2))
#define TF_REQ_AP_WRITE(reg)     (0x01U | ((((uint32_t)(reg) >> 2) & 0x3U) << 2))
#define TF_REQ_AP_READ(reg)      (0x03U | ((((uint32_t)(reg) >> 2) & 0x3U) << 2))

/* DP registers. */
#define TF_DP_ABORT              0x00U
#define TF_DP_CTRL_STAT          0x04U
#define TF_DP_SELECT             0x08U
#define TF_DP_RDBUFF             0x0CU

/* AP registers (AHB-AP, bank 0). */
#define TF_AP_CSW                0x00U
#define TF_AP_TAR                0x04U
#define TF_AP_DRW                0x0CU

/* AHB-AP CSW fields.
 * CSW_32BIT | CSW_ADDRINC_SINGLE are 0x02 | 0x10.  For Cortex-M debug accesses
 * the CSW must additionally assert HPROT1 (privileged, bit 25) and
 * HMASTER_DEBUG (bit 29): without bit 25 the AHB-AP returns FAULT for the
 * PPB debug region (CPUID/DBGMCU/DHCSR), which is exactly what the STM32F407
 * bring-up showed.  Matches OpenOCD's CSW_AHB_DEFAULT (minus DbgSwEnable bit31
 * which this AP does not implement). */
#define TF_AP_CSW_HPROT_PRIV     (1U << 25)
#define TF_AP_CSW_HMASTER_DEBUG  (1U << 29)
#define TF_AP_CSW_32BIT_INC      (0x00000012U | TF_AP_CSW_HPROT_PRIV | \
                                  TF_AP_CSW_HMASTER_DEBUG)
#define TF_AP_CSW_MODE_MASK      0x00000013U

/* Target families. */
typedef enum {
    TF_FAMILY_UNKNOWN = 0,
    TF_FAMILY_STM32F4,
    TF_FAMILY_STM32H7,
} tf_family_t;

struct tf_family_ops;

/** Runtime target description + per-session family state. */
typedef struct tf_target {
    tf_family_t family;
    const struct tf_family_ops *ops;
    char family_str[16];
    uint32_t dpidr;
    uint32_t cpuid;
    uint32_t idcode;          /* DBGMCU IDCODE */
    uint8_t uid[12];
    uint32_t flash_base;
    uint32_t flash_size;
    uint32_t max_image_size;

    /* STM32F4: false = x64 (PSIZE=3) programming, true = x32 fallback. */
    bool f4_x32_mode;
} tf_target_t;

/** Per-family flash operations (implemented in tf_stm32f4.c / tf_stm32h7.c). */
typedef struct tf_family_ops {
    const char *name;
    /** Fill target->family_str/idcode/uid/flash_* from the already-linked target. */
    esp_err_t (*probe)(tf_target_t *target);
    /** Erase every sector that overlaps [addr, addr+size). */
    esp_err_t (*erase)(tf_target_t *target, uint32_t addr, uint32_t size);
    /** Unlock the flash controller and prepare for programming. */
    esp_err_t (*program_begin)(tf_target_t *target);
    /** Program [addr, addr+len); len is aligned to the family program unit. */
    esp_err_t (*program)(tf_target_t *target, uint32_t addr, const uint8_t *data, size_t len);
    /** Lock the flash controller. */
    esp_err_t (*program_end)(tf_target_t *target);
} tf_family_ops_t;

extern const tf_family_ops_t tf_family_stm32f4_ops;
extern const tf_family_ops_t tf_family_stm32h7_ops;

/* ---- bring-up stage tracking (RTC noinit; debug endpoint builds only) ---- */
void tf_stage_mark(uint32_t stage);
uint32_t tf_stage_get(void);

#ifdef CONFIG_BIRDIEBOT_TARGET_FLASH_DEBUG_HTTP
#define TF_STAGE(stage) tf_stage_mark(stage)
#else
#define TF_STAGE(stage) ((void)(stage))
#endif

/* ---- helpers implemented in tf_swd.c (shared by the family modules) ---- */
uint64_t tf_now_ms(void);
esp_err_t tf_swd_wait_clear(uint32_t addr, uint32_t mask, uint32_t timeout_ms);
esp_err_t tf_swd_wait_set(uint32_t addr, uint32_t mask, uint32_t timeout_ms);
esp_err_t tf_swd_clear_error_bits(uint32_t addr, uint32_t bits);

#ifdef __cplusplus
}
#endif

#endif /* TF_INTERNAL_H */
