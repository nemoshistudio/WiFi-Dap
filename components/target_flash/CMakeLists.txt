# WiFi Dap - target flash engine (Web-side direct flashing)
#
# Streams a target firmware image from the Web layer to an STM32 (F4/H7) over
# SWD by driving the CMSIS-DAP command engine (components/DAP) inline.
#
# Dependencies:
#   DAP        - DAP_ExecuteCommand() (CMSIS-DAP command engine)
#   dap_proxy  - dap_handle_request_reset()/dap_handle_wait_reset() DAP hand-off
#   mbedtls    - SHA-256 of the streamed image + flash read-back verification
#   esp_timer  - per-phase timeouts and timing report
# The optional /tf bring-up HTTP endpoint lives in the separate
# components/target_flash_debug component (Kconfig default n) so this engine
# keeps its minimal dependency set and no dependency cycle with web_server.
#
# The component never owns the DAP engine permanently: it asks the DAP task for
# an exclusive hand-off for the duration of one operation and restores the
# normal USBIP/WS/elaphureLink data path afterwards.

set(priv_requires
    DAP
    dap_proxy
    mbedtls
    esp_timer
)

set(srcs
    "src/wt_target_flash.c"
    "src/tf_dap.c"
    "src/tf_swd.c"
    "src/tf_stm32f4.c"
    "src/tf_stm32h7.c"
)


idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${priv_requires}
)

