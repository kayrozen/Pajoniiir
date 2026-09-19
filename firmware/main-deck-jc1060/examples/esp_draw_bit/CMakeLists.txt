# Top-level CMakeLists.txt for esp_draw_bit example
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Include the board support component located in the target tree.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Bump on every OTA-worthy change (same-version images are skipped by OTA).
set(PROJECT_VER "2")

project(esp_draw_bit)
