# UART service (WP-203): single owner of the target UART.
#
# Contents:
#   src/uart_service.c          - UART driver owner, RX fan-out, TX queue
#   src/uart_service_api_json.c - JSON module 4 (UART), API_JSON_MODULE_REGISTER
#
# driver:  UART driver API (uart_param_config/uart_driver_install/...)
# board:   board pin contract (UART instance + TX/RX pins + default baud)
# api_router/json: JSON module 4 self-registration
#
# WHOLE_ARCHIVE: the JSON module registers itself from a global constructor;
# the objects must be kept even if no symbol is referenced directly (same
# pattern as wifi_manager / wt_system in the C tree).

file(GLOB SOURCES src/*.c)

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "include"
        REQUIRES driver
        PRIV_REQUIRES board api_router json
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
