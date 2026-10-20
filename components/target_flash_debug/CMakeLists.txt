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

set(debug_srcs
    "src/tf_debug_http.c"
    "src/tf_panic_capture.c"
)

idf_component_register(
    SRCS ${debug_srcs}
    PRIV_REQUIRES target_flash web_server esp_http_server
)

# Bring-up debugging only: capture panic PC/assert details in RTC memory.
# Enabled explicitly with -DTF_PANIC_CAPTURE=ON for a debug firmware build; the
# production build command never sets it, so no wrap symbols or debug strings
# are linked there.
option(TF_PANIC_CAPTURE "Capture panic PC/assert details in RTC memory (bring-up builds)" OFF)
if(TF_PANIC_CAPTURE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_PANIC_CAPTURE)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_panic_handler"
        "-Wl,--wrap=panic_abort"
        "-Wl,--wrap=__assert_func"
    )
endif()

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
