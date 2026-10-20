file(GLOB SOURCES *.c)

# board:        BIRDIEBOT_UART_BRIDGE_PORT (1346) and Kconfig defaults
# uart_service: single owner of the target UART (WP-203); the bridge is now
#               just a TCP client of it (no direct UART driver usage here)
idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        PRIV_REQUIRES board uart_service
)
