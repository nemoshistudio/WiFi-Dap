/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STATIC_BUFFER_H_GUARD
#define STATIC_BUFFER_H_GUARD

#include <stdint.h>

int memory_pool_init();

void *memory_pool_get(uint32_t tick_wait);

void memory_pool_put(void *ptr);

uint32_t memory_pool_get_buf_size();

/**
 * @brief Whether memory_pool_init() completed successfully.
 *
 * Used by components that must report a dynamic buffer limit before the pool
 * is usable (e.g. wt_ws_router_max_payload(), docs/ws-binary-router.md).
 *
 * @return 1 initialized, 0 not initialized
 */
int memory_pool_is_initialized(void);


#endif //STATIC_BUFFER_H_GUARD
