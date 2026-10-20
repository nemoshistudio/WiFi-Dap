/**
 * @file wt_ws_router.c
 * @brief WebSocket 二进制帧路由实现（冻结契约 v1，docs/ws-binary-router.md）。
 *
 * 职责（只做分发/复制/队列，不做业务逻辑）：
 *   入站  uri_ws.c（FIN=1 完整帧、长度校验后）→ wt_ws_router_rx()
 *           → 按 module 查表调用注册的 wt_ws_rx_cb_t（httpd/WS 任务上下文，
 *             回调必须快速返回）。
 *   出站  wt_ws_router_broadcast()（任意 FreeRTOS 任务上下文）
 *           → memcpy 4B 头 + payload 到内部静态 slot → 投递到发送队列
 *           → 发送任务调用 WS 传输层注册的 tx port（uri_ws.c）异步发给所有
 *             WS 客户端；httpd 回调里绝不直接阻塞发送。
 *
 * 线程安全：
 *   - register_rx 发生在 start_webserver() 之前（之后只读）；写操作仍用
 *     portMUX 保护，避免误用时的数据竞争。
 *   - broadcast 任意任务可调用，队列满返回 -1（调用方自行丢弃/计数），不阻塞。
 *   - tx port 由 uri_ws.c 在 WS 初始化时注册，发送任务只读快照。
 *
 * 不依赖 httpd 类型：tx port 的入参是“完整二进制帧（4B 头 + payload）”，
 * 由传输层自行构造 httpd_ws_frame_t。
 *
 * 内存：发送队列深度默认 8（契约）；每槽 ((4B 头 + WT_WS_ROUTER_MAX_PAYLOAD)
 * 字节静态内存，共约 14 KiB BSS，避免在广播路径上做动态分配。
 */
#include "wt_ws_router.h"
#include "wt_data_def.h"
#include "memory_pool.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <string.h>

#define TAG "ws_router"

#define ROUTER_QUEUE_DEPTH_DEFAULT 8 /* docs/ws-binary-router.md §2 */
#define ROUTER_QUEUE_DEPTH_MAX     8
#define ROUTER_FRAME_MAX           (sizeof(wt_bin_data_hdr_t) + WT_WS_ROUTER_MAX_PAYLOAD)
#define ROUTER_TX_TASK_STACK       4096
#define ROUTER_TX_TASK_PRIO        4

/* ------------------------------------------------------------------ */
/* Inbound dispatch table (modules 0..WT_WS_MODULE_MAX-1)             */
/* ------------------------------------------------------------------ */
static wt_ws_rx_cb_t rx_cbs[WT_WS_MODULE_MAX];
static portMUX_TYPE rx_lock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* Outbound queue + WS transport tx port                              */
/* ------------------------------------------------------------------ */
static volatile wt_ws_tx_port_t ws_tx_port;
static volatile uint8_t tx_depth = ROUTER_QUEUE_DEPTH_DEFAULT;
static volatile bool tx_ready;

/* Static slot pool: one queued frame per slot, no dynamic allocation. */
static uint8_t tx_slot_frame[ROUTER_QUEUE_DEPTH_MAX][ROUTER_FRAME_MAX];
static size_t tx_slot_len[ROUTER_QUEUE_DEPTH_MAX];

/* free slots (producer -> consumer rotation) and work slots. */
static QueueHandle_t tx_free_queue;
static QueueHandle_t tx_work_queue;
static StaticQueue_t tx_free_queue_buf;
static StaticQueue_t tx_work_queue_buf;
static uint8_t tx_free_queue_storage[ROUTER_QUEUE_DEPTH_MAX];
static uint8_t tx_work_queue_storage[ROUTER_QUEUE_DEPTH_MAX];
static TaskHandle_t tx_task_handle;

static _Noreturn void router_tx_task(void *arg);

/**
 * @brief Create the send queues and the send task (idempotent).
 *
 * Called from wt_ws_router_set_tx_port() (WS init, before frames flow) and
 * from wt_ws_router_broadcast() when needed. Static queues only; the only
 * dynamic allocation is the task stack, done once.
 */
static void router_tx_start(void)
{
	if (tx_ready) {
		return;
	}

	tx_free_queue = xQueueCreateStatic(tx_depth, sizeof(uint8_t),
	                                   tx_free_queue_storage, &tx_free_queue_buf);
	tx_work_queue = xQueueCreateStatic(tx_depth, sizeof(uint8_t),
	                                   tx_work_queue_storage, &tx_work_queue_buf);
	if (tx_free_queue == NULL || tx_work_queue == NULL) {
		ESP_LOGE(TAG, "failed to create static queues");
		return;
	}

	for (uint8_t i = 0; i < tx_depth; ++i) {
		xQueueSend(tx_free_queue, &i, 0);
	}

	if (xTaskCreate(router_tx_task, "ws_router_tx", ROUTER_TX_TASK_STACK, NULL,
	                ROUTER_TX_TASK_PRIO, &tx_task_handle) != pdPASS) {
		ESP_LOGE(TAG, "failed to create ws_router tx task");
		tx_task_handle = NULL;
		return;
	}

	tx_ready = true;
	ESP_LOGD(TAG, "tx queue ready, depth=%u", (unsigned) tx_depth);
}

static _Noreturn void router_tx_task(void *arg)
{
	(void) arg;

	while (1) {
		uint8_t slot;
		if (xQueueReceive(tx_work_queue, &slot, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		wt_ws_tx_port_t port = ws_tx_port;
		if (port != NULL) {
			int err = port(tx_slot_frame[slot], tx_slot_len[slot]);
			if (err != 0) {
				ESP_LOGW(TAG, "tx port error %d (frame len %u)",
				         err, (unsigned) tx_slot_len[slot]);
			}
		}

		if (xQueueSend(tx_free_queue, &slot, portMAX_DELAY) != pdTRUE) {
			/* Cannot happen with a balanced queue pair; keep the task alive. */
			ESP_LOGE(TAG, "failed to recycle tx slot %u", slot);
		}
	}
}

int wt_ws_router_register_rx(uint8_t module, wt_ws_rx_cb_t cb)
{
	if (module >= WT_WS_MODULE_MAX || cb == NULL) {
		return -1;
	}

	int err = 0;
	portENTER_CRITICAL(&rx_lock);
	if (rx_cbs[module] != NULL) {
		err = -1; /* same-module re-registration is rejected (contract §2) */
	} else {
		rx_cbs[module] = cb;
	}
	portEXIT_CRITICAL(&rx_lock);

	if (err == 0) {
		ESP_LOGD(TAG, "module %u rx callback registered", (unsigned) module);
	}
	return err;
}

int wt_ws_router_rx(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                    const uint8_t *payload, size_t len)
{
	if (module >= WT_WS_MODULE_MAX) {
		return -1;
	}
	if (payload == NULL && len > 0) {
		return -1;
	}
	if (len > WT_WS_ROUTER_MAX_PAYLOAD) {
		return -1;
	}

	/* Direction rules (docs/protocol.md §3): client -> device only carries RAW;
	 * CMD is reserved (control commands use JSON) and every other data_type is
	 * rejected so uri_ws.c can answer with MSG_BINARY_ERROR. */
	if (data_type != WT_DATA_RAW) {
		return -1;
	}

	/* Registration happens before start_webserver(); this read is lock-free by
	 * contract. The callback runs in the httpd/WS task context. */
	wt_ws_rx_cb_t cb = rx_cbs[module];
	if (cb == NULL) {
		return -1;
	}

	return cb(data_type, module, sub_mod, payload, len) == 0 ? 0 : -1;
}

int wt_ws_router_set_tx_port(wt_ws_tx_port_t port)
{
	router_tx_start();
	ws_tx_port = port;
	return 0;
}

int wt_ws_router_broadcast(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                           const uint8_t *payload, size_t len)
{
	if (payload == NULL && len > 0) {
		return -1;
	}
	if (len > WT_WS_ROUTER_MAX_PAYLOAD) {
		return -1;
	}
	if (module >= WT_WS_MODULE_MAX) {
		return -1;
	}

	/* No WS transport attached yet (or webserver torn down): nothing to send.
	 * Drop silently instead of failing the data source. */
	if (!tx_ready || ws_tx_port == NULL) {
		return 0;
	}

	uint8_t slot;
	if (xQueueReceive(tx_free_queue, &slot, 0) != pdTRUE) {
		/* Queue full: contract says return -1 and let the caller decide
		 * (UART RX path drops the oldest chunk and bumps an overflow counter). */
		return -1;
	}

	/* 4-byte binary header (wt_bin_data_hdr_t) followed by the payload. */
	tx_slot_frame[slot][0] = data_type;
	tx_slot_frame[slot][1] = module;
	tx_slot_frame[slot][2] = sub_mod;
	tx_slot_frame[slot][3] = 0;
	if (len > 0) {
		memcpy(&tx_slot_frame[slot][sizeof(wt_bin_data_hdr_t)], payload, len);
	}
	tx_slot_len[slot] = sizeof(wt_bin_data_hdr_t) + len;

	if (xQueueSend(tx_work_queue, &slot, 0) != pdTRUE) {
		/* Slot was taken but the work queue is momentarily full (only possible
		 * when the tx task holds a slot outside both queues). */
		xQueueSend(tx_free_queue, &slot, 0);
		return -1;
	}

	return 0;
}

size_t wt_ws_router_max_payload(void)
{
	/* Dynamic limit comes from memory_pool once it is initialized; before that
	 * the contract says return 0 (docs/ws-binary-router.md §2). */
	if (!memory_pool_is_initialized()) {
		return 0;
	}

	uint32_t buf_size = memory_pool_get_buf_size();
	/* Reserve room for the WS message bookkeeping (ws_msg_t) and control
	 * overhead inside the memory_pool buffer: 2048 - 256 = 1792. */
	const uint32_t reserve = 256;
	if (buf_size <= reserve) {
		return 0;
	}

	size_t limit = buf_size - reserve;
	if (limit > WT_WS_ROUTER_MAX_PAYLOAD) {
		limit = WT_WS_ROUTER_MAX_PAYLOAD;
	}
	return limit;
}

int wt_ws_router_set_queue_depth(uint8_t depth)
{
	if (depth == 0 || depth > ROUTER_QUEUE_DEPTH_MAX) {
		return -1;
	}
	if (tx_ready) {
		/* Queues are already created; the contract requires this to be called
		 * before start_webserver(). */
		ESP_LOGW(TAG, "queue depth must be set before the router starts");
		return -1;
	}

	tx_depth = depth;
	return 0;
}
