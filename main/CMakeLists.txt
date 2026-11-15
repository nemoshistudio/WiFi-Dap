# main/ holds app_main() plus the M3 DAP-over-TCP executor bridge
# (main/dap_tcp_bridge.c).  The glob is intentional; note that ESP-IDF's
# component script mode rejects CONFIGURE_DEPENDS, so adding a source file here
# is picked up on a reconfigure (touching this file or a clean build does that).
file(GLOB SOURCES *.c)

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
)
