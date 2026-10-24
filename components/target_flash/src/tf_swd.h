/*
 * SPDX-License-Identifier: MIT
 *
 * SW-DP / AHB-AP access layer for the target flash engine.
 */
#ifndef TF_SWD_H
#define TF_SWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "tf_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect the DAP to SWD and bring the debug port up.
 *
 * Performs DAP_Connect(SWD), line reset + JTAG-to-SWD + line reset + idle,
 * SWJ_Clock(clock_hz), SWD_Configure(turnaround=1, data_phase=0),
 * TransferConfigure, nRESET release, DP CTRL/STAT power-up request,
 * DP SELECT=0 and AHB-AP CSW=32-bit increment.
 *
 * @param clock_hz requested SWD/JTAG clock (probe retries at 2/1/0.5 MHz).
 * @param[out] dpidr SW-DP IDCODE (may be NULL).
 */
esp_err_t tf_swd_link_init(uint32_t clock_hz, uint32_t *dpidr);

/**
 * Select a forced SWD clock for subsequent link bring-ups (0 = auto probe).
 * Valid manual values are 100 kHz..10 MHz; the caller (Web/API) validates the
 * range before this call.  Shared by target_flash and curve_sampler so the
 * Web choice applies to both detect/flash and local sampling.
 */
void tf_swd_set_forced_clock(uint32_t clock_hz);

/** @brief Forced SWD clock, or 0 when automatic probing is enabled. */
uint32_t tf_swd_get_forced_clock(void);

/** Clear DP/AP sticky errors and re-arm the AHB-AP CSW after a failed access
 *  (wrong DBGMCU base probes use this before trying the next candidate). */
esp_err_t tf_swd_recover_bus(void);

/** Read/write one 32-bit word through the AHB-AP (TAR + DRW). */
esp_err_t tf_swd_read_u32(uint32_t addr, uint32_t *value);
esp_err_t tf_swd_write_u32(uint32_t addr, uint32_t value);

/**
 * @brief Switch the AHB-AP CSW access size between 32-bit (default) and
 *        16-bit single-increment accesses.
 *
 * Only the F0/F1/F3 flash path needs 16-bit accesses (their flash controller
 * programs one half-word per data write).  The caller must always restore
 * 32-bit mode after the half-word sequence, even on error.
 */
esp_err_t tf_swd_set_access_16bit(bool enable);

/** Write one half-word; the caller must have enabled 16-bit access first. */
esp_err_t tf_swd_write_u16_raw(uint32_t addr, uint16_t value);
/** Read one half-word; the caller must have enabled 16-bit access first. */
esp_err_t tf_swd_read_u16_raw(uint32_t addr, uint16_t *value);

/** Read a memory range through the AHB-AP (chunked block reads). */
esp_err_t tf_swd_read_mem(uint32_t addr, void *buf, size_t len);

/** Halt the core (DHCSR C_HALT) / release it (C_DEBUGEN only). */
esp_err_t tf_swd_halt(void);
esp_err_t tf_swd_resume(void);
esp_err_t tf_swd_is_halted(bool *halted);

/** SYSRESETREQ; `run` true leaves the core running after reset. */
esp_err_t tf_swd_sysreset(bool run);

/** Read the halted core's PC through DCRSR/DCRDR. */
esp_err_t tf_swd_read_pc(uint32_t *pc);

/**
 * @brief Read one Cortex-M core register through DCRSR/DCRDR (read direction,
 *        REGWnR=0), waiting for DHCSR.S_REGRDY before reading DCRDR.
 *
 * The core must already be halted (DCRSR is only accepted in debug/halt state;
 * the caller owns the halt, e.g. the curve sampler's per-tick halt). Unlike
 * tf_swd_read_pc() this does not halt/resume the core itself.
 *
 * @param reg   0..15 = R0..R15 (13 = SP, 14 = LR, 15 = PC).
 * @param[out] value register value.
 * @return ESP_OK / ESP_ERR_INVALID_ARG (reg > 15 or value NULL) / ESP_FAIL /
 *         ESP_ERR_TIMEOUT when S_REGRDY never asserts.
 */
esp_err_t tf_swd_read_reg(uint8_t reg, uint32_t *value);

/**
 * @brief Bring-up diagnostic: run the link sequence step by step and append a
 *        human-readable report to `out` (used by the Kconfig-gated /tf?cmd=diag
 *        endpoint; not used in the production flash path).
 */
esp_err_t tf_swd_link_diag(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* TF_SWD_H */
