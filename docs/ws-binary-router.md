# WebSocket 二进制数据路由契约（wt_ws_router）

> 冻结草案 v1（2026-10-20，Wave 0）。实现：WP-204（`components/dataflow` + `project_components/web_server/uri_modules/uri_bin_router.c`）；
> 挂接：WP-123（`uri_ws.c` 收到完整二进制帧后调用 router）；UART 服务：WP-203（`components/uart_service`）。
> 头文件：`components/ws_router/include/wt_ws_router.h`（Lead 维护，Wave 0 冻结后任何改动需 Lead 批准并广播）。

## 1. 数据流

```
浏览器 WS 二进制帧 (4B 头 + payload)
        │  uri_ws.c（FIN=1 完整帧，长度校验后）
        ▼
wt_ws_router_rx(data_type, module, sub_mod, payload, len)   ← router.c 分发
        │  按 module 查表
        ▼
注册的回调（UART 模块 → uart_service 写 TX；其它模块按契约处理）
        ▲
        │
wt_ws_router_broadcast(data_type, module, sub_mod, payload, len)
        ▲
uart_service RX 事件 / 其它数据源（任意 FreeRTOS 任务上下文）
        │  内部复制到消息队列 → WS 发送任务/async 发送
        ▼
所有 WS 客户端（binary frame）
```

## 2. API（冻结）

```c
#include <stdint.h>
#include <stddef.h>

#define WT_WS_ROUTER_MAX_PAYLOAD 1792   /* 2048 - 内部头预留；实际以 memory_pool 为准 */
#define WT_WS_MODULE_MAX         6      /* 0..5，模块号见 protocol.md */

typedef int (*wt_ws_rx_cb_t)(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                             const uint8_t *payload, size_t len);

/* 注册某模块的入站二进制处理回调。返回 0 成功；同模块重复注册返回 -1。
 * 回调在 httpd/WS 任务上下文执行，必须快速返回；耗时操作请自行入队。 */
int wt_ws_router_register_rx(uint8_t module, wt_ws_rx_cb_t cb);

/* 广播二进制帧给所有已连接 WS 客户端。payload 会被复制（调用方无需保留）。
 * 任一线程可调用；队列满时返回 -1（调用方决定丢弃/计数）。 */
int wt_ws_router_broadcast(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                           const uint8_t *payload, size_t len);

/* 当前可用 payload 上限（由 memory_pool 初始化后动态给出；未初始化返回 0）。 */
size_t wt_ws_router_max_payload(void);

/* 可选：设置发送队列深度（默认 8），在 start_webserver 之前调用。 */
int wt_ws_router_set_queue_depth(uint8_t depth);
```

## 3. 约定与约束

1. **谁实现**：`components/ws_router/src/wt_ws_router.c`（Wave1/Wave2 由 WP-204 实现）；头文件 Lead 冻结。
2. **uri_ws.c 挂接方式**：收到 `HTTPD_WS_TYPE_BINARY` 且 FIN=1 后，校验 `len>=4 && len<=wt_ws_router_max_payload()`，拆出 4B 头后调用 `wt_ws_router_rx(...)`；失败时回一个 `{"error":"Bad binary frame","code":2}` 文本帧。uri_ws 不实现业务逻辑。
3. **payload 生命周期**：入站回调中的 `payload` 只在回调期间有效；需要保留请复制。
4. **广播**：router 内部 memcpy 后入队，由 WS 发送任务异步发送；不得在 httpd 回调里直接阻塞发送。
5. **帧上限**：超过 `wt_ws_router_max_payload()` 的帧直接丢弃并回错误；不做分片聚合（只有 FIN=1 帧才进入 router）。
6. **线程安全**：注册发生在 `start_webserver()` 之前；之后只读。
7. **测试**：`wt_ws_router_rx`/`broadcast` 的纯逻辑（查表、长度校验、队列行为）应在 host 侧可单测（Wave3 WP-302）。

## 4. 模块注册约定

- UART 模块（4）：`uart_service` 注册 rx 回调；收到 `RAW` 时写入对应 UART；UART RX 通过 `wt_ws_router_broadcast(RAW_BROADCAST, 4, uart_idx, data, len)` 上报。
- 其他模块（0/1/2/5）当前只走 JSON；若未来需要二进制，按同模式注册。
- 不得在 router 内直接 include 业务组件；router 只做分发/复制/队列。

*契约版本：v1，2026-10-20，Lead 维护。*
