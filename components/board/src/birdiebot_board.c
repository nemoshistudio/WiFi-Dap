/**
 * @file birdiebot_board.c
 * @brief Board identity strings + board/target consistency check (WP-006).
 *
 * This translation unit exists so that the board header selected through
 * Kconfig is compile-checked by every target build (esp32s3 / esp32c3 /
 * esp32), even before any driver includes it. The exported strings are meant
 * for boot logging and for the CMSIS-DAP DAP_Info board strings (WP-101).
 */
#include "birdiebot_board.h"

const char birdiebot_board_id[]   = BIRDIEBOT_BOARD_ID;
const char birdiebot_board_name[] = BIRDIEBOT_BOARD_NAME;

/*
 * Board / IDF target consistency check.
 *
 * Kept as a warning (not #error) so that compile-only bring-up of the
 * ESP32-C3 / ESP32 targets is not blocked by a forgotten Kconfig selection;
 * the Kconfig defaults already pick the matching generic board per target.
 * A mismatch here almost always means the wrong board was selected for the
 * current `idf.py set-target`, and the runtime pin map will be wrong.
 */
#if defined(CONFIG_IDF_TARGET_ESP32S3) && (BIRDIEBOT_BOARD_TARGET_ID != 3)
#warning "birdiebot_board: selected board does not match IDF target esp32s3 (check 'WiFi Dap board configuration -> Target board')"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C3) && (BIRDIEBOT_BOARD_TARGET_ID != 2)
#warning "birdiebot_board: selected board does not match IDF target esp32c3 (check 'WiFi Dap board configuration -> Target board')"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) && (BIRDIEBOT_BOARD_TARGET_ID != 1)
#warning "birdiebot_board: selected board does not match IDF target esp32 (check 'WiFi Dap board configuration -> Target board')"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) && (BIRDIEBOT_BOARD_TARGET_ID != 4)
#warning "birdiebot_board: selected board does not match IDF target esp32c5 (check 'WiFi Dap board configuration -> Target board')"
#endif
