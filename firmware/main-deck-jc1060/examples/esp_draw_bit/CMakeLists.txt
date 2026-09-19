# Top-level CMakeLists.txt for esp_draw_bit example
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Include the board support component located in the target tree.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

project(esp_draw_bit)
