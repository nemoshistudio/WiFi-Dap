file(GLOB SOURCES *.c)

# board: status LED pins for dap_led.c (WP-006/WP-112)
# DAP / USBIP: CMSIS-DAP core and USB descriptor/control handling
# esp_ringbuf / mbedtls: DAP ring buffers and the WebSocket-DAP handshake (SHA1)
idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        PRIV_REQUIRES DAP USBIP esp_ringbuf mbedtls board
)
