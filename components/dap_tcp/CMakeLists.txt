# WiFi Dap - CMSIS-DAP over TCP backend (M3 skeleton, TCP 3241)
#
# Serves one OpenOCD / XRDAP client at a time with the CMSIS-DAP TCP frame
# format (OpenOCD `cmsis-dap backend tcp`): 8-byte header ("DAP\0" + u16
# little-endian length + type + reserved) followed by one CMSIS-DAP packet.
#
# Config: CONFIG_BIRDIEBOT_DAP_TCP_ENABLE (default n), port/priority/stack/core
# in Kconfig.  With the feature off, src/dap_tcp_server.c compiles to the
# dap_tcp_server_*() stubs and nothing listens on the port.
#
# Responsibilities:
#   src/dap_tcp_frame.c   pure C header codec + streaming parser (no ESP-IDF)
#   src/dap_tcp_server.c  lwIP socket session: single client, keepalive,
#                         TCP_NODELAY, frame dispatch, weak execute hook
#
# The frame codec is host-tested without ESP-IDF:
#   components/dap_tcp/test/host/run.sh
#
# Design, wiring plan and HIL checklist: docs/dap-tcp-backend.md.
#
# Dependencies:
#   lwip - TCP sockets for the server source only (the codec needs nothing)

idf_component_register(
    SRCS
        "src/dap_tcp_frame.c"
        "src/dap_tcp_server.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip
)
