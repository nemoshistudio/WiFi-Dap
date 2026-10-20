# WiFi Dap - OTA core component (WP-205)
#
# Implements the firmware half of docs/ota-local-upload.md:
#   * upload session state machine (IDLE/RECEIVING/VERIFYING/READY/APPLYING/ERROR)
#   * esp_ota_begin/write/end/abort/set_boot_partition wrapper into the inactive
#     OTA slot selected with esp_ota_get_next_update_partition()
#   * incremental SHA-256 of the received image (X-SHA256 comparison + display)
#   * thread-safe C API consumed by project_components/web_server/uri_ota.c
#     (POST /api/ota/upload, WP-206)
#   * rollback confirmation helpers (wt_ota_mark_app_valid / _invalid)
#
# The component is registered with WHOLE_ARCHIVE so the JSON module 5
# constructor (API_JSON_MODULE_REGISTER) and the global module init run.
#
# See components/wt_ota/README.md for the API/HTTP mapping and main() rollback
# integration contract.

idf_component_register(
    SRCS
        "src/wt_ota.c"
        "src/wt_ota_json.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        app_update
        esp_partition
        esp_app_format
        mbedtls
        json
        api_router
        global_resource
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
