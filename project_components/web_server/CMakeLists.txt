file(GLOB SOURCES
        web_server.c
        web_uri_module.c
        uri_modules/uri_ws.c
        uri_modules/uri_api.c
        uri_modules/uri_html_base.c
        uri_modules/uri_bin_router.c
        )

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        REQUIRES esp_http_server
        PRIV_REQUIRES request_runner api_router json memory_pool utils html SSDP
        # WP-204 (UART/DataFlow): WS binary router hook.
        # ws_router/wt_common are also added by the WP-123 branch; duplicate
        # entries are harmless and the Lead unifies both lines on merge.
        PRIV_REQUIRES ws_router wt_common uart_service dataflow
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
