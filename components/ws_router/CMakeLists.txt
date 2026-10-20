# ws_router: WebSocket binary frame router contract component.
# Public header frozen by the Lead (components/ws_router/include/wt_ws_router.h,
# docs/ws-binary-router.md); implementation is Wave 1 WP-123 (web-dev), later
# extended by WP-204 (dataflow/uart_service).
idf_component_register(
        SRCS "src/wt_ws_router.c"
        INCLUDE_DIRS "include"
        REQUIRES freertos
        PRIV_REQUIRES memory_pool wt_common
)
