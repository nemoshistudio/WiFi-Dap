# WiFi Dap - C1 wired DAP: USB-NCM MVP (w6/c1ncm)
#
# The wired transport is an INDEPENDENT build variant:
#   default  : CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NONE -> only the stub in
#              src/wired_dap.c has code; app_main() never calls into this
#              component and the Wi-Fi product behaviour is unchanged.
#   wired    : CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM=y -> TinyUSB CDC-NCM +
#              a minimal esp_netif/lwIP Ethernet driver (src/ncm_netif.c).
#              Built with sdkconfig.wired.defaults via scripts/build-wired.sh.
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
