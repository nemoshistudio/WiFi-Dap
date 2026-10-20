# CMSIS-DAP core (SWD/JTAG/vendor). Ported from the wireless-esp8266-dap fork
# (A tree) onto the IDF5 wireless-esp32-tools layout (C tree).
#
# Dependencies (WP-101):
#   board     - single source of truth for SWD/JTAG/LED pins
#               (components/board/include/birdiebot_board.h)
#   esp_timer - TIMESTAMP_GET() (1 us monotonic timestamp for DAP_SWJ_Pins)
#
# The DAP component is intentionally not REQUIRED by anything except
# dap_proxy/elaphureLink, all other dependencies stay private.
file(GLOB SOURCES
        ./cmsis-dap/source/*.c
        )

# board/esp_timer must be PUBLIC dependencies: DAP_config.h is a public header
# (INCLUDE_DIRS ".") and is included by components/dap_proxy (DAP task / LED),
# which then needs the board pin header and esp_timer.h include paths.
idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        REQUIRES board esp_timer
)
