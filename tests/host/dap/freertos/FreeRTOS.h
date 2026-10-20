/*
 * PROVENANCE (WP-302 host regression, 2026-10-20)
 * --------------------------------------------------------------------------
 * Integrated from the dap-dev ASan host harness at /tmp/dap_host (stub
 * DAP_config.h + FreeRTOS shims, verified 36/36 at the time, 51/51 after the
 * batch-path cases were added; pre-fix DAP.c reproduces N1 as ASan
 * stack-overflow and N9 as ASan heap-buffer-overflow READ of size 1).
 * Source review: docs/security-review-dap.md (N1 P0 / N6 P2 / N9 P2/P3);
 * fixes: commit 666df38 (N1/N9/N6) merged by 89bfd0d on feature/birdiebot-v1.
 * This copy keeps the test code unchanged on purpose; only this provenance
 * header and the build paths in the instructions below were adapted.
 * It is compiled against the *current* repo DAP.c, so it is a live regression.
 */

#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <stdint.h>
#define CONFIG_FREERTOS_HZ 1000
#define pdMS_TO_TICKS(x) ((uint32_t)(x))
#endif
