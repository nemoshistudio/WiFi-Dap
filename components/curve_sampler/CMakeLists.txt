# WiFi Dap - curve page device-local sampler (Phase 0/1)
#
# Implements docs/curve-local-sampler.md: the curve page sends its AddressExpr
# series configuration over HTTP, this component samples the STM32 over the
# local SWD link on a dedicated worker task and the Web UI polls binary CVF1
# frame batches.  No WebSocket-DAP/USBIP round-trip is used for the samples.
#
# Dependencies:
#   target_flash  - tf_swd_* local SWD access layer (link/halt/read_mem/read_reg)
#   dap_proxy     - DAP hand-off (dap_handle_request_reset/wait_reset,
#                   tcp_server_kick_session)
#   web_server    - WEB_URI_MODULE_REGISTER + web_host_guard
#   esp_http_server, json (cJSON), esp_timer
#
# The URI modules register themselves through web_server's constructor
# mechanism (same pattern as components/target_flash_debug), therefore the
# whole archive is linked.
set(srcs
    "src/curve_engine.c"
    "src/curve_expr.c"
    "src/curve_json.c"
    "src/curve_ring.c"
    "src/curve_http.c"
)

idf_component_register(
    SRCS ${srcs}
    PRIV_REQUIRES
        target_flash
        dap_proxy
        web_server
        esp_http_server
        json
        esp_timer
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
