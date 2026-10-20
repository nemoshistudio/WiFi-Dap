/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wt_system_json_utils.h"

static void wt_sys_json_add_header(cJSON *root, wt_system_cmd_t cmd)
{
	cJSON_AddNumberToObject(root, "cmd", cmd);
	cJSON_AddNumberToObject(root, "module", SYSTEM_MODULE_ID);
}

cJSON *wt_sys_json_ser_fm_info(wt_fm_info_t *info)
{
	cJSON *root = cJSON_CreateObject();
	wt_sys_json_add_header(root, WT_SYS_GET_FM_INFO);
	cJSON_AddStringToObject(root, "fm_ver", info->fm_ver);
	cJSON_AddStringToObject(root, "upd_date", info->upd_date);
	/* docs/protocol.md §2.1 / versioning.md §3.4: always an integer. */
	cJSON_AddNumberToObject(root, "protocol_version", info->protocol_version);
	return root;
}

cJSON *wt_sys_json_ser_sys_info(wt_sys_info_t *info)
{
	cJSON *root = cJSON_CreateObject();
	wt_sys_json_add_header(root, WT_SYS_GET_SYS_INFO);
	cJSON_AddStringToObject(root, "sn", info->sn);
	cJSON_AddStringToObject(root, "hw_ver", info->hw_ver);
	cJSON_AddStringToObject(root, "mf_date", info->mf_date);
	return root;
}
