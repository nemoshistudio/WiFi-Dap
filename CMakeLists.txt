cmake_minimum_required(VERSION 3.5)
# Colored  Compiler Output with ninja
add_compile_options (-fdiagnostics-color=always)

# Firmware version: git describe > version.txt > fallback, injected as
# PROJECT_VER before project() so esp_app_desc.version is correct even on
# shallow clones / source archives without .git (docs/versioning.md §2.2).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# Startup version log (docs/versioning.md §2.4) needs esp_app_desc.h from IDF.
if(EXISTS "${IDF_PATH}/components/esp_app_format/include")
    include_directories("${IDF_PATH}/components/esp_app_format/include")
endif()
set(EXTRA_COMPONENT_DIRS project_components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One component-manager lock file per IDF target.
#
# The unified baseline is ESP-IDF v5.5.6 for every target, but the per-target
# lock (dependencies.lock.${IDF_TARGET}) is kept: IDF v5.5 writes lock format v2
# while older v5.2/v5.3 images write format v1, and a shared "dependencies.lock"
# makes the other toolchain fail to parse it ("Cannot parse components lock
# file"). This keeps single-target image overrides/rollbacks working without
# corrupting another target's lock. IDF documents DEPENDENCIES_LOCK for exactly
# this use case (build-system docs). The files are gitignored.
if(NOT DEFINED IDF_TARGET OR IDF_TARGET STREQUAL "")
    set(_birdiebot_lock_suffix "default")
else()
    set(_birdiebot_lock_suffix "${IDF_TARGET}")
endif()
idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.${_birdiebot_lock_suffix}")

project(birdiebot_wifi_dap)
