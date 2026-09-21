# Pajoniiir main deck firmware (ESP32-P4 JC1060P470C_I_W_Y)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# BSP shared with the bring-up examples.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Bump on every OTA-worthy change (same-version images are skipped by OTA).
set(PROJECT_VER "88")

project(pajoniiir_deck)
