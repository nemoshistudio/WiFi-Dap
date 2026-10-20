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

/** Clear DP/AP sticky errors and re-arm the AHB-AP CSW after a failed access
 *  (wrong DBGMCU base probes use this before trying the next candidate). */
esp_err_t tf_swd_recover_bus(void);

/** Read/write one 32-bit word through the AHB-AP (TAR + DRW). */
esp_err_t tf_swd_read_u32(uint32_t addr, uint32_t *value);
esp_err_t tf_swd_write_u32(uint32_t addr, uint32_t value);

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
 * @brief Bring-up diagnostic: run the link sequence step by step and append a
 *        human-readable report to `out` (used by the Kconfig-gated /tf?cmd=diag
 *        endpoint; not used in the production flash path).
 */
esp_err_t tf_swd_link_diag(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* TF_SWD_H */
