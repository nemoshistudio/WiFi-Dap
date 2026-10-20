file(GLOB SOURCES *.c)

# driver: UART driver (uart_param_config/uart_driver_install/uart_set_pin)
# board:  board pin contract (UART instance + TX/RX pins + port/baud defaults)
idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        PRIV_REQUIRES driver board
)
