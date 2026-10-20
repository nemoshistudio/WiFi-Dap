/*
 * SPDX-License-Identifier: MIT
 *
 * Optional panic capture for target-flash bring-up debugging.
 *
 * Only active when the build is configured with -DTF_PANIC_CAPTURE=ON (a CMake
 * cache option, debug builds only): the linker then wraps esp_panic_handler /
 * panic_abort / __assert_func, records the faulting PC, panic reason/assert
 * strings in RTC no-init memory and lets the regular panic handler run.  The
 * /tf?cmd=status endpoint reports the captured data after the reboot.
 *
 * Production builds never enable this option, so no wrap symbols, RTC buffers
 * or debug strings are linked (the production image must stay clean).
 */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tf_panic_capture.h"

#if CONFIG_BIRDIEBOT_TARGET_FLASH_DEBUG_HTTP && defined(TF_PANIC_CAPTURE)

#include <esp_attr.h>

#include "esp_private/panic_internal.h"

RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint32_t s_fault_pc;
RTC_NOINIT_ATTR static uint32_t s_panic_addr;
RTC_NOINIT_ATTR static uint32_t s_panic_exc;
RTC_NOINIT_ATTR static uint32_t s_panic_core;
RTC_NOINIT_ATTR static char s_panic_desc[48];
RTC_NOINIT_ATTR static char s_panic_reason[24];
RTC_NOINIT_ATTR static char s_assert_file[64];
RTC_NOINIT_ATTR static char s_assert_expr[80];
RTC_NOINIT_ATTR static char s_abort_details[80];
RTC_NOINIT_ATTR static uint32_t s_assert_line;
#define TF_PANIC_CAPTURE_MAGIC 0x50434150U

void __real_esp_panic_handler(panic_info_t *info);
void __real_panic_abort(const char *details);
void __real___assert_func(const char *file, int line, const char *func, const char *expr);

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0U;

    if (dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    while (i + 1U < dst_len && src[i] != '\0') {
        char c = src[i];
        if (c == '"' || c == '\\') {
            c = '\'';
        } else if (c < 0x20 || c > 0x7E) {
            c = '.';
        }
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info != NULL) {
        s_magic = TF_PANIC_CAPTURE_MAGIC;
        s_panic_addr = (uint32_t)(uintptr_t)info->addr;
        s_fault_pc = panic_get_address(info->frame);
        s_panic_exc = (uint32_t)info->exception;
        s_panic_core = (uint32_t)info->core;
        copy_str(s_panic_desc, sizeof(s_panic_desc), info->description);
        copy_str(s_panic_reason, sizeof(s_panic_reason), info->reason);
    }
    __real_esp_panic_handler(info);
}

void __wrap_panic_abort(const char *details)
{
    s_magic = TF_PANIC_CAPTURE_MAGIC;
    copy_str(s_abort_details, sizeof(s_abort_details), details);
    __real_panic_abort(details);
}

void __wrap___assert_func(const char *file, int line, const char *func, const char *expr)
{
    s_magic = TF_PANIC_CAPTURE_MAGIC;
    s_assert_line = (uint32_t)line;
    copy_str(s_assert_file, sizeof(s_assert_file), file);
    copy_str(s_assert_expr, sizeof(s_assert_expr), expr);
    __real___assert_func(file, line, func, expr);
}

void tf_panic_capture_get(tf_panic_capture_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_magic != TF_PANIC_CAPTURE_MAGIC) {
        return;
    }
    out->valid = true;
    out->fault_pc = s_fault_pc;
    out->panic_addr = s_panic_addr;
    out->exception = (int)s_panic_exc;
    out->core = (int)s_panic_core;
    out->assert_line = (uint32_t)s_assert_line;
    memcpy(out->panic_desc, s_panic_desc, sizeof(out->panic_desc));
    memcpy(out->panic_reason, s_panic_reason, sizeof(out->panic_reason));
    memcpy(out->assert_file, s_assert_file, sizeof(out->assert_file));
    memcpy(out->assert_expr, s_assert_expr, sizeof(out->assert_expr));
    memcpy(out->abort_details, s_abort_details, sizeof(out->abort_details));
}

void tf_panic_capture_clear(void)
{
    s_magic = 0U;
}

#else /* no capture in this build */

void tf_panic_capture_get(tf_panic_capture_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

void tf_panic_capture_clear(void)
{
}

#endif
