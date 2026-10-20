/**
 * @file wt_ws_router.h
 * @brief WiFi Dap — WebSocket 二进制帧路由契约（冻结 v1，2026-10-20）
 *
 * 头文件由 Lead 维护；实现由 Wave2 WP-204 完成（components/ws_router/src/wt_ws_router.c）。
 * 契约文档：docs/ws-binary-router.md、docs/protocol.md。
 */
#ifndef WT_WS_ROUTER_H_GUARD
#define WT_WS_ROUTER_H_GUARD

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WT_WS_ROUTER_MAX_PAYLOAD 1792 /* 2048 - 内部头预留；最终以 memory_pool 为准 */
#define WT_WS_MODULE_MAX         6    /* 模块号 0..5，见 docs/protocol.md */

typedef int (*wt_ws_rx_cb_t)(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                             const uint8_t *payload, size_t len);

/** 注册模块的入站二进制回调；成功 0，同模块重复注册 -1。回调须快速返回。 */
int wt_ws_router_register_rx(uint8_t module, wt_ws_rx_cb_t cb);

/** WS 发送端口：由 uri_ws.c 在 start_webserver 前注册；router 广播时调用。
 *  frame 指向完整 WS 二进制帧缓冲（4B 头 + payload），返回 0 成功。 */
typedef int (*wt_ws_tx_port_t)(const uint8_t *frame, size_t len);
int wt_ws_router_set_tx_port(wt_ws_tx_port_t port);

/** 入站二进制帧入口：uri_ws 收到 FIN=1 完整帧并校验长度后调用。
 *  返回 0 已分发；-1 无处理者/参数错误（uri_ws 回错误帧）。 */
int wt_ws_router_rx(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                    const uint8_t *payload, size_t len);

/** 广播二进制帧给所有 WS 客户端；payload 被复制；任一线程可调用；队列满 -1。 */
int wt_ws_router_broadcast(uint8_t data_type, uint8_t module, uint8_t sub_mod,
                           const uint8_t *payload, size_t len);

/** 当前 payload 上限（未初始化为 0）。 */
size_t wt_ws_router_max_payload(void);

/** 发送队列深度（默认 8，需在 start_webserver() 之前调用）。 */
int wt_ws_router_set_queue_depth(uint8_t depth);

#ifdef __cplusplus
}
#endif

#endif /* WT_WS_ROUTER_H_GUARD */
