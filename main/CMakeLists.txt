file(GLOB SOURCES *.c)


idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        PRIV_REQUIRES dap_proxy DAP global_resource web_server memory_pool request_runner uart_tcp_bridge wifi_manager wt_mdns wt_storage wt_ota wt_common board esp_app_format esp_event esp_netif esp_timer
)