# cmake/version.cmake -- firmware version resolution for WiFi Dap.
#
# Contract: docs/versioning.md §2.2 (WP-141 / WP-007).
#
# Resolution order (highest priority first):
#   1. -DBIRDIEBOT_FW_VERSION=...            (CI / release script override)
#   2. git describe --tags --dirty --always --match "v[0-9]*"
#   3. <project root>/version.txt            (source archive / no .git)
#   4. BIRDIEBOT_FW_VERSION_FALLBACK (default v1.0.0) with a WARNING
#
# The resolved value is written to PROJECT_VER *before* ESP-IDF's project()
# is called, so it ends up in esp_app_desc.version (GET_FM_INFO.fm_ver /
# OTA/GET_INFO.running_ver) without relying on IDF's own git logic.
#
# Note on the regex: CMake's regex engine (cmsys) does not support the
# {n,m} repetition operator, so the contract's "[0-9a-f]{7,}" is spelled out
# as seven explicit hex digits followed by "+" (i.e. at least 7).

set(BIRDIEBOT_FW_VERSION "" CACHE STRING
    "Override firmware version (e.g. v1.0.0); highest priority")
set(BIRDIEBOT_FW_VERSION_FALLBACK "v1.0.0" CACHE STRING
    "Fallback firmware version when git metadata and version.txt are unavailable")
option(BIRDIEBOT_FW_RELEASE
       "Enforce release-quality version resolution (reject -dirty and fallback)" OFF)

# Allowed version: vMAJOR.MINOR.PATCH[-N-g<sha7+>][-dirty]  (docs/versioning.md App. A)
set(_bb_ver_re
    "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9]+-g[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]+)?(-dirty)?$")

set(_bb_ver "")
set(_bb_ver_source "")

if(NOT BIRDIEBOT_FW_VERSION STREQUAL "")
    set(_bb_ver "${BIRDIEBOT_FW_VERSION}")
    set(_bb_ver_source "BIRDIEBOT_FW_VERSION")
    message(STATUS "Version: from BIRDIEBOT_FW_VERSION=${_bb_ver}")
else()
    execute_process(
        COMMAND git describe --tags --dirty --always --match "v[0-9]*"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _bb_git_out
        ERROR_QUIET
        RESULT_VARIABLE _bb_git_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(_bb_git_rc EQUAL 0 AND NOT _bb_git_out STREQUAL "" AND _bb_git_out MATCHES "${_bb_ver_re}")
        set(_bb_ver "${_bb_git_out}")
        set(_bb_ver_source "git describe")
        message(STATUS "Version: from git describe=${_bb_ver}")
    else()
        if(_bb_git_rc EQUAL 0 AND NOT _bb_git_out STREQUAL "")
            # e.g. --always returned a bare commit hash: not a usable version.
            message(STATUS "Version: git describe='${_bb_git_out}' is not a version, trying version.txt")
        endif()

        if(EXISTS "${CMAKE_SOURCE_DIR}/version.txt")
            file(READ "${CMAKE_SOURCE_DIR}/version.txt" _bb_txt)
            string(STRIP "${_bb_txt}" _bb_txt)
            if(_bb_txt MATCHES "${_bb_ver_re}")
                set(_bb_ver "${_bb_txt}")
                set(_bb_ver_source "version.txt")
                message(WARNING "Version: git unavailable, using version.txt=${_bb_ver}")
            else()
                message(WARNING "Version: ignoring invalid version.txt content '${_bb_txt}'")
            endif()
        endif()
    endif()

    if(_bb_ver STREQUAL "")
        set(_bb_ver "${BIRDIEBOT_FW_VERSION_FALLBACK}")
        set(_bb_ver_source "fallback")
        message(WARNING "Version: git metadata unavailable, using fallback=${_bb_ver}")
    endif()
endif()

if(NOT _bb_ver MATCHES "${_bb_ver_re}")
    message(FATAL_ERROR
        "Invalid firmware version string: '${_bb_ver}' "
        "(expected vMAJOR.MINOR.PATCH[-N-g<sha>][-dirty], see docs/versioning.md)")
endif()

if(BIRDIEBOT_FW_RELEASE)
    if(_bb_ver MATCHES "-dirty$")
        message(FATAL_ERROR "Release build requires a clean worktree, got '${_bb_ver}' (-dirty)")
    endif()
    if(_bb_ver_source STREQUAL "fallback")
        message(FATAL_ERROR
            "Release build must not use the fallback version; tag the release "
            "or pass -DBIRDIEBOT_FW_VERSION (docs/versioning.md §2.2.3)")
    endif()
endif()

set(PROJECT_VER "${_bb_ver}" CACHE STRING "" FORCE)
set(BIRDIEBOT_FW_VERSION_RESOLVED "${_bb_ver}" CACHE INTERNAL
    "Firmware version resolved by cmake/version.cmake")
set(BIRDIEBOT_FW_VERSION_SOURCE "${_bb_ver_source}" CACHE INTERNAL
    "Where BIRDIEBOT_FW_VERSION_RESOLVED came from")
