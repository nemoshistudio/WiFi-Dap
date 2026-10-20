cmake_minimum_required(VERSION 3.5)
# Colored  Compiler Output with ninja
add_compile_options (-fdiagnostics-color=always)

# Firmware version: git describe > version.txt > fallback, injected as
# PROJECT_VER before project() so esp_app_desc.version is correct even on
# shallow clones / source archives without .git (docs/versioning.md §2.2).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
set(EXTRA_COMPONENT_DIRS project_components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(birdiebot_wifi_dap)
