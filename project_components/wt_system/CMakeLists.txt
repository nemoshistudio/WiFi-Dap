file(GLOB SOURCES *.c
        )

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "."
        PRIV_REQUIRES
        global_resource esp_app_format api_router board esp_hw_support wt_common
)

# ---------------------------------------------------------------------------
# Build metadata injection for SYSTEM/GET_FM_INFO and SYSTEM/GET_SYS_INFO.
# Contract: docs/versioning.md §2.3 (WP-125).
#
#   FW_UPD_DATE : firmware build/commit date, YYYY-MM-DD (GET_FM_INFO.upd_date)
#   FW_HW_VER   : optional hardware version override; empty = BIRDIEBOT_BOARD_NAME
#   FW_MF_DATE  : manufacturing date; empty = FW_UPD_DATE (build date), see
#                 docs/protocol.md §2.1 (mf_date is build-injected)
#
# No value may be injected as an empty string: when git metadata is unavailable
# (e.g. building a git worktree inside the IDF Docker container, where the
# worktree's .git file points outside the mounted project) the build date is
# used and a warning is printed, as required by docs/versioning.md §2.3.
# ---------------------------------------------------------------------------
set(HW_VER "" CACHE STRING "Hardware version for SYSTEM/GET_SYS_INFO (empty = board name)")
set(MF_DATE "" CACHE STRING "Manufacturing date YYYY-MM-DD for SYSTEM/GET_SYS_INFO (empty = build date)")

execute_process(
        COMMAND git show -s --format=%cd --date=format:%Y-%m-%d
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE FW_UPD_DATE
        RESULT_VARIABLE result
        ERROR_VARIABLE git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT result EQUAL 0 OR NOT FW_UPD_DATE MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
    string(TIMESTAMP FW_UPD_DATE "%Y-%m-%d" UTC)
    message(WARNING "wt_system: git commit date unavailable (${git_error}); using FW_UPD_DATE=${FW_UPD_DATE}")
endif()

if("${MF_DATE}" STREQUAL "")
    set(FW_MF_DATE "${FW_UPD_DATE}")
else()
    set(FW_MF_DATE "${MF_DATE}")
endif()

if(NOT FW_MF_DATE MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
    message(WARNING "wt_system: MF_DATE='${MF_DATE}' is not YYYY-MM-DD; GET_SYS_INFO.mf_date uses it verbatim")
endif()

message(STATUS "wt_system: upd_date=${FW_UPD_DATE} hw_ver='${HW_VER}' mf_date='${FW_MF_DATE}'")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
        FW_UPD_DATE="${FW_UPD_DATE}"
        FW_HW_VER="${HW_VER}"
        FW_MF_DATE="${FW_MF_DATE}"
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
