/*
 * SPDX-License-Identifier: MIT
 *
 * Debug hooks for the target flash engine, consumed by the optional
 * components/target_flash_debug bring-up endpoint (Kconfig default n).
 * Not part of the frozen Web-facing API in wt_target_flash.h.
 */
#ifndef WT_TARGET_FLASH_DEBUG_H
#define WT_TARGET_FLASH_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "wt_target_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Result of the last completed flash session (SHA-256 + timing). */
typedef struct {
    bool valid;
    bool sha_match;
    char sha_expected[65];
    char sha_actual[65];
    uint32_t image_size;
    uint32_t elapsed_ms;
    wt_tf_info_t info;
} tf_debug_result_t;

void tf_debug_get_result(tf_debug_result_t *out);

/** Halt (if needed), read the target PC, resume when it was running before. */
esp_err_t tf_debug_read_pc(uint32_t *pc, bool *halted, bool *lockup);

#ifdef __cplusplus
}
#endif

#endif /* WT_TARGET_FLASH_DEBUG_H */
