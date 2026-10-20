/*
 * SPDX-License-Identifier: MIT
 *
 * Panic-capture data shared between the optional linker-wrapped handlers and
 * the /tf debug endpoint (bring-up builds only, -DTF_PANIC_CAPTURE=ON).
 */
#ifndef TF_PANIC_CAPTURE_H
#define TF_PANIC_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    uint32_t fault_pc;      /* instruction that faulted (panic_get_address) */
    uint32_t panic_addr;    /* panic_info_t.addr */
    int exception;          /* panic_exception_t */
    int core;
    uint32_t assert_line;
    char panic_desc[48];
    char panic_reason[24];
    char assert_file[64];
    char assert_expr[80];
    char abort_details[80];
} tf_panic_capture_t;

/** Copy the last captured panic (zeroed/valid=false when nothing captured). */
void tf_panic_capture_get(tf_panic_capture_t *out);

/** Clear the captured panic (called by the boot counter setup). */
void tf_panic_capture_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* TF_PANIC_CAPTURE_H */
