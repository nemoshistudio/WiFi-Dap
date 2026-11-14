# Probe identity: one runtime source of truth for serial / chip / board / fw.
#
# Depends on:
#   - board        : BIRDIEBOT_BOARD_NAME (selected board id string)
#   - esp_hw_support: esp_read_mac() (factory WiFi STA MAC)
#   - esp_app_format: esp_app_get_description() (running firmware version)
idf_component_register(
        SRCS "birdiebot_probe_identity.c" "birdiebot_probe_identity_format.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES board esp_hw_support esp_app_format
)
