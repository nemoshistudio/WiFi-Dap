file(GLOB SOURCES
        web_server.c
        web_uri_module.c
        web_error_json.c
        web_host_guard.c
        uri_modules/uri_ws.c
        uri_modules/uri_api.c
        uri_modules/uri_html_base.c
        uri_modules/uri_bin_router.c
        uri_modules/uri_ota.c
        )

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "." "include"
        REQUIRES esp_http_server
        # Merged dependency set (Lead resolution):
        # - WP-123 (web): ws_router, wt_common
        # - WP-204 (uart/dataflow): uart_service, dataflow (uri_bin_router.c)
        # - WP-206 (OTA upload): wt_ota (uri_ota.c)
        # - web_host_guard (browser Host/Origin safety): no extra dependency
        PRIV_REQUIRES request_runner api_router json memory_pool utils html SSDP ws_router wt_common uart_service dataflow wt_ota
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
