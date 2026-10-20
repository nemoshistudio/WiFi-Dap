# WiFi Dap - board pin abstraction (WP-006)
#
# Header-only pin/board definitions plus a tiny identity source file.
# The source file is intentional: it forces the currently selected board
# header (Kconfig BIRDIEBOT_BOARD) to be compile-checked in every target
# build (esp32s3 / esp32c3 / esp32).
#
# Consumers: add "board" to REQUIRES/PRIV_REQUIRES and include
#   #include "birdiebot_board.h"
#
# See docs/board-pins.md for the pin tables and the selection contract.

idf_component_register(
    SRCS "src/birdiebot_board.c"
    INCLUDE_DIRS "include"
)
