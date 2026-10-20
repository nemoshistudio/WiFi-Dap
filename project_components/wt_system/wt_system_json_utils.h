/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WT_SYSTEM_JSON_UTILS_H_GUARD
#define WT_SYSTEM_JSON_UTILS_H_GUARD

#include "wt_system_api.h"
#include "wt_system.h"
#include <cJSON.h>


cJSON *wt_sys_json_ser_fm_info(wt_fm_info_t *info);

/**
 * @brief Serialize SYSTEM/GET_SYS_INFO (cmd 3): sn / hw_ver / mf_date.
 */
cJSON *wt_sys_json_ser_sys_info(wt_sys_info_t *info);

#endif //WT_SYSTEM_JSON_UTILS_H_GUARD
