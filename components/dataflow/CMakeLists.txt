# DATA_FLOW module (WP-204): minimal module 2 JSON implementation.
#
# Contents:
#   src/dataflow.c              - instance/attach state (protocol.md §2.3)
#   src/dataflow_api_json.c     - JSON module 2, API_JSON_MODULE_REGISTER
#
# The binary data plane lives in components/uart_service (module 4 owner) and
# project_components/web_server/uri_modules/uri_bin_router.c (WS binary path).
#
# api_router/json: JSON module self-registration
# uart_service:    the single DATA_FLOW instance is the UART instance
#
# WHOLE_ARCHIVE: keep the self-registering constructor object linked.

file(GLOB SOURCES src/*.c)

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "include"
        PRIV_REQUIRES api_router json uart_service freertos
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
