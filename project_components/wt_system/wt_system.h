/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WT_SYSTEM_H_GUARD
#define WT_SYSTEM_H_GUARD

#include "global_module.h"

/**
 * @brief SYSTEM/GET_FM_INFO (cmd 1) payload (docs/protocol.md §2.1).
 *
 * fm_ver    = esp_app_get_description()->version (= PROJECT_VER / git tag)
 * upd_date  = build/commit date injected by CMake (FW_UPD_DATE, YYYY-MM-DD)
 * protocol_version = BIRDIEBOT_PROTOCOL_VERSION (docs/versioning.md §3)
 */
typedef struct wt_fm_info_t {
	char fm_ver[32];
	char upd_date[11];
	int protocol_version;
} wt_fm_info_t;

/**
 * @brief SYSTEM/GET_SYS_INFO (cmd 3) payload (docs/protocol.md §2.1).
 *
 * sn      = WiFi STA MAC as 12 uppercase hex chars, no separators
 *           (fallback: empty string when esp_read_mac() fails)
 * hw_ver  = build-injected HW_VER, default = BIRDIEBOT_BOARD_NAME
 * mf_date = build-injected MF_DATE, default = build date (YYYY-MM-DD)
 */
typedef struct wt_sys_info_t {
	char sn[13];
	char hw_ver[32];
	char mf_date[16];
} wt_sys_info_t;

void wt_system_get_fm_info(wt_fm_info_t *fm_info);

/**
 * @brief Fill the device identity reported by SYSTEM/GET_SYS_INFO (cmd 3).
 *
 * sn = WiFi STA MAC (12 uppercase hex chars), hw_ver = build-injected HW_VER
 * (board name by default), mf_date = build-injected MF_DATE (build date by
 * default). Safe to call repeatedly; never returns an uninitialized struct.
 */
void wt_system_get_sys_info(wt_sys_info_t *sys_info);

/**
 * Trigger delayed reboot in 2 seconds
 */
void wt_system_reboot();


#endif //WT_SYSTEM_H_GUARD
