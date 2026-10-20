/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WT_SYSTEM_API_H_GUARD
#define WT_SYSTEM_API_H_GUARD

/* Kconfig value CONFIG_BIRDIEBOT_ENABLE_CRASH_CMD must be visible before the
 * WT_SYSTEM_ENABLE_CRASH_CMD mapping below; include sdkconfig explicitly so
 * the macro works in every includer (and skip it for host builds). */
#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

#define SYSTEM_MODULE_ID 0

/*
 * SYSTEM/DO_CRASH (cmd 200) is debug-only: Kconfig option
 * CONFIG_BIRDIEBOT_ENABLE_CRASH_CMD defaults to n, so release builds do not
 * compile/register the handler (docs/protocol.md §2.1). Debug builds enable it
 * with `idf.py menuconfig` -> "WiFi Dap system module" -> DO_CRASH, or by
 * adding CONFIG_BIRDIEBOT_ENABLE_CRASH_CMD=y to sdkconfig.defaults.*. The
 * fallback keeps host builds (no sdkconfig.h) compiling with the command off.
 */
#if defined(CONFIG_BIRDIEBOT_ENABLE_CRASH_CMD)
#define WT_SYSTEM_ENABLE_CRASH_CMD 1
#else
#define WT_SYSTEM_ENABLE_CRASH_CMD 0
#endif

typedef enum wt_system_cmd_t {
	WT_SYS_GET_FM_INFO = 1,
	WT_SYS_REBOOT = 2,
	WT_SYS_GET_SYS_INFO = 3,

	WT_SYS_DO_CRASH = 200,
} wt_system_cmd_t;


#endif //WT_SYSTEM_API_H_GUARD
