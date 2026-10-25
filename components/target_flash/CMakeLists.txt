# WiFi Dap - target flash engine (Web-side direct flashing)
#
# Streams a target firmware image from the Web layer to an STM32 (F0/F1/F2/F3/
# F4/F7/G0/G4/H7/L4/WB/WL) over SWD by driving the CMSIS-DAP command engine
# (components/DAP) inline.
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
#
# Slim build (H3, 2026-10-26): the P1-19 nearby-device discovery (mDNS
# _http._tcp query + JSON formatter) is compiled out by default
# (CONFIG_BIRDIEBOT_WEB_DISCOVER_ENABLE=n in project_components/web_server/
# Kconfig).  The sources stay in the tree for a full-feature restore; the
# pure-formatter host test (test/host/test_discover.c) still compiles
# tf_discover_core.c directly and is unaffected.  docs/feature-flags.md H3.

set(priv_requires
    DAP
    dap_proxy
    mbedtls
    esp_timer
    esp_wifi      # P0-8 link diagnostics: read-only esp_wifi_sta_get_ap_info()
    # P1-19 discovery dependencies.  Kept UNCONDITIONAL on purpose: ESP-IDF
    # expands component requirements in a CMake pass that runs before sdkconfig
    # exists on a fresh configure, so a CONFIG_ test around PRIV_REQUIRES is not
    # reliable.  Only the source list below is CONFIG-conditional; with the
    # feature off no discover object file is built, so these include
    # paths/dependencies cost nothing in the image.
    mdns          # mdns_query_ptr("_http","_tcp")
    esp_netif     # esp_ip4_addr_t formatting from mDNS A records
)

set(srcs
    "src/wt_target_flash.c"
    "src/tf_dap.c"
    "src/tf_swd.c"
    "src/tf_stm32_id.c"
    "src/tf_stm32_geom.c"
    "src/tf_stm32f1.c"
    "src/tf_stm32f4.c"
    "src/tf_stm32h7.c"
    "src/tf_stm32_page.c"
)

if(CONFIG_BIRDIEBOT_WEB_DISCOVER_ENABLE)
    # P1-19 discovery sources only; requirements are unconditional above.
    list(APPEND srcs "src/tf_discover.c" "src/tf_discover_core.c")
endif()


idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${priv_requires}
)

