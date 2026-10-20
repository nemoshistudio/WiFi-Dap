# ws_router: WebSocket binary frame router contract component.
# Header frozen in Wave 0 (Lead); implementation by Wave2 WP-204.
idf_component_register(
        SRCS "src/wt_ws_router.c"
        INCLUDE_DIRS "include"
        REQUIRES freertos
)
