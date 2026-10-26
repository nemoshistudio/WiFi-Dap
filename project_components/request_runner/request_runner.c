/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "request_runner.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <sys/cdefs.h>

static QueueHandle_t long_run_queue = NULL;
static QueueHandle_t send_out_queue = NULL;

_Noreturn static void req_long_task(void *arg);

_Noreturn static void req_send_out_task(void *arg);

_Noreturn void req_long_task(void *arg)
{
	req_task_cb_t *req;
	while (1) {
		if (unlikely(xQueueReceive(long_run_queue, &req, portMAX_DELAY) != pdTRUE)) {
			continue;
		}
		req->status = req->module.cb(req->module.arg);

		/* if send out queue is busy, set status and let the cb to cancel send out
		 * */
		if (req_queue_push_send_out(req, pdMS_TO_TICKS(20)) != 0) {
			req->status = -1;
			req->send_out.cb(req->send_out.arg, req->status);
		}
	}
}

_Noreturn void req_send_out_task(void *arg)
{
	req_task_cb_t *req;
	while (1) {
		if (likely(xQueueReceive(send_out_queue, &req, portMAX_DELAY))) {
			req->send_out.cb(req->send_out.arg, req->status);
		}
	}
}


/* Web request workers are low-frequency tasks; on the S3 N16R8 PSRAM build
 * their 4 KiB stacks live in PSRAM to relieve the internal heap.  The tasks are
 * never deleted (they live for the whole app), so no delete counterpart is
 * needed; keep the internal-RAM fallback.  C5 behaviour is unchanged (the C5
 * build keeps its internal stacks for now). */
static BaseType_t req_create_task(TaskFunction_t fn, const char *name,
                                  uint32_t stack_depth, UBaseType_t prio)
{
#if defined(CONFIG_SPIRAM) && defined(CONFIG_IDF_TARGET_ESP32S3)
	BaseType_t res = xTaskCreateWithCaps(fn, name, stack_depth, NULL, prio,
	                                     NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (res == pdPASS) {
		return res;
	}
	ESP_LOGW("request_runner", "%s: PSRAM stack unavailable, using internal RAM",
	         name);
#endif
	return xTaskCreate(fn, name, stack_depth, NULL, prio, NULL);
}

int request_runner_init()
{
	BaseType_t res;
	if (long_run_queue != NULL || send_out_queue != NULL) {
		return 0;
	}

	long_run_queue = xQueueCreate(2, sizeof(req_task_cb_t *));
	send_out_queue = xQueueCreate(4, sizeof(req_task_cb_t *));
	assert(long_run_queue != NULL && send_out_queue != NULL);

	res = req_create_task(req_long_task, "Ltask", 4 * 1024, 8);
	res &= req_create_task(req_send_out_task, "send out task", 4 * 1024, 9);
	assert(res == pdPASS);
	return 0;
}

int req_queue_push_long_run(req_task_cb_t *req, uint32_t delay)
{
	if (unlikely(xQueueSend(long_run_queue, &req, delay) != pdTRUE)) {
		return 1;
	}

	return 0;
}

int req_queue_push_send_out(req_task_cb_t *req, uint32_t delay)
{
	if (unlikely(xQueueSend(send_out_queue, &req, delay) != pdTRUE)) {
		return 1;
	}

	return 0;
}
