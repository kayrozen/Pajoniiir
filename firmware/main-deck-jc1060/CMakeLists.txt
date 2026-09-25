# Pajoniiir main deck firmware (ESP32-P4 JC1060P470C_I_W_Y)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# BSP shared with the bring-up examples. v118: firmware/common components
# (N1 migration) shared with main-deck-p4 the same way.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "${CMAKE_CURRENT_LIST_DIR}/../common/firmware_health"
    "${CMAKE_CURRENT_LIST_DIR}/../common/ota_manifest"
    "${CMAKE_CURRENT_LIST_DIR}/../common/control_state_reconciler")

# Bump on every OTA-worthy change (same-version images are skipped by OTA).
set(PROJECT_VER "240")

project(pajoniiir_deck)

# v155: patch the pinned esp-usb fork exactly like upstream main-deck-p4 -
# indexed idle-recovery + HS/FS FIFO split + MSC teardown. Must run AFTER
# project() so the component manager has materialized the fork components.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/apply_espressif_usb_fifo_patch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/apply_espressif_usb_idle_recovery_patch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/apply_usb_host_msc_teardown_patch.cmake")
