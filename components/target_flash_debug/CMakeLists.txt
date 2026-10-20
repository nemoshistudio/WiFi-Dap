# WiFi Dap - optional target flash bring-up endpoint.
#
# This component only exists so the /tf HTTP endpoint can depend on
# web_server/esp_http_server without creating a dependency cycle in the
# production direction (web_server -> target_flash once the Web integration
# lands).  It contains no code unless CONFIG_BIRDIEBOT_TARGET_FLASH_DEBUG_HTTP
# is enabled (Kconfig default n), so production builds pay nothing.
#
# The URI module registers itself through the web_server constructor mechanism
# (same pattern as components/wt_ota), therefore the whole archive is linked.

idf_component_register(
    SRCS "src/tf_debug_http.c"
    PRIV_REQUIRES target_flash web_server esp_http_server
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
