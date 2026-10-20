/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wt_system.h"
#include "wt_system_api.h"
#include "wt_protocol_version.h"

#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <stdio.h>

#include "birdiebot_board.h"

#define TAG "WT_SYS"

/*
 * Build metadata injected by project_components/wt_system/CMakeLists.txt
 * (docs/versioning.md §2.3). The fallbacks keep host syntax checks and
 * standalone builds compiling without the component CMake; they are never
 * empty strings, so GET_FM_INFO.upd_date / GET_SYS_INFO.mf_date stay valid.
 */
#ifndef FW_UPD_DATE
#define FW_UPD_DATE "1970-01-01"
#endif
#ifndef FW_HW_VER
#define FW_HW_VER ""
#endif
#ifndef FW_MF_DATE
#define FW_MF_DATE FW_UPD_DATE
#endif

void wt_system_get_fm_info(wt_fm_info_t *fm_info)
{
	const esp_app_desc_t *app_desc = esp_app_get_description();

	/* Single source of truth for the firmware version: esp_app_desc.version
	 * (= PROJECT_VER, computed from the git tag by the root CMake). */
	snprintf(fm_info->fm_ver, sizeof(fm_info->fm_ver), "%s", app_desc->version);
	snprintf(fm_info->upd_date, sizeof(fm_info->upd_date), "%s", FW_UPD_DATE);
	fm_info->protocol_version = BIRDIEBOT_PROTOCOL_VERSION;
}

void wt_system_get_sys_info(wt_sys_info_t *sys_info)
{
	uint8_t mac[6] = {0};
	esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

	if (err == ESP_OK) {
		/* docs/protocol.md §2.1: 12 uppercase hex chars, no separators. */
		snprintf(sys_info->sn, sizeof(sys_info->sn),
		         "%02X%02X%02X%02X%02X%02X",
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	} else {
		ESP_LOGW(TAG, "esp_read_mac(ESP_MAC_WIFI_STA) failed: %s", esp_err_to_name(err));
		sys_info->sn[0] = '\0';
	}

	/* hw_ver: build-injected HW_VER; empty means "use the selected board name". */
	snprintf(sys_info->hw_ver, sizeof(sys_info->hw_ver), "%s",
	         FW_HW_VER[0] ? FW_HW_VER : BIRDIEBOT_BOARD_NAME);

	/* mf_date: build-injected MF_DATE, build date by default (never empty). */
	snprintf(sys_info->mf_date, sizeof(sys_info->mf_date), "%s",
	         FW_MF_DATE[0] ? FW_MF_DATE : FW_UPD_DATE);
}

static void reboot_task(void *arg)
{
	ESP_LOGW(TAG, "reboot in 2 seconds");
	vTaskDelay(pdMS_TO_TICKS(2000));
	esp_restart();
}

void wt_system_reboot()
{
	xTaskCreatePinnedToCore(reboot_task, "reboot", 4096, NULL, 3, NULL, 0);
}

static int wt_system_init(void)
{
	return 0;
}

static const global_module_t module = {
	.init = wt_system_init,
	.module_id = SYSTEM_MODULE_ID
};

GLOBAL_MODULE_REGISTER(WT_SYSTEM, &module);
