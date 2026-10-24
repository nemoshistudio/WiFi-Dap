# WiFi Dap - C1 wired DAP: USB-NCM MVP (w6/c1ncm)
#
# On ESP32-S3 the default build enables NCM + Wi-Fi in one image
# (CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM=y in sdkconfig.defaults.esp32s3):
#   default : TinyUSB CDC-NCM + minimal esp_netif/lwIP driver + DHCP server,
#             while Wi-Fi AP/STA remains active.
#   C3/ESP32: CONFIG_NONE -> only the stub in src/wired_dap.c is compiled.
# The old scripts/build-wired.sh + sdkconfig.wired.defaults remain as a
# compatibility variant.
#
# NOTE on dependencies (IDF build-system rule): REQUIRES/PRIV_REQUIRES must not
# depend on CONFIG_xxx choices (requirements are expanded before the project
# configuration is loaded), so the component requirements below are static.
# The esp_tinyusb managed dependency is injected by the component manager from
# components/wired_dap/idf_component.yml (target rule: esp32s3 only).
# src/ncm_netif.c is compiled for every target but its body is fully guarded by
# "#if CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM && IDF_TARGET_ESP32S3", so the
# default build compiles an empty translation unit and keeps Wi-Fi behaviour.
idf_component_register(
    SRCS "src/wired_dap.c" "src/ncm_netif.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip
)
