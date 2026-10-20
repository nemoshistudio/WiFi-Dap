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
        # Merged dependency set (Lead resolution):
        # - WP-123 (web): ws_router, wt_common
        # - WP-204 (uart/dataflow): uart_service, dataflow (uri_bin_router.c)
        PRIV_REQUIRES request_runner api_router json memory_pool utils html SSDP ws_router wt_common uart_service dataflow
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
