/**
 * @file sd.h
 * @brief SDMMC (microSD) interface for JC1060P470C_I_W_Y
 */

#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount the microSD card (FAT, slot 0, 4-bit, powered by internal LDO
 *        channel 4 @ 2.7 V per the official board pin map)
 *
 * @param mount_point VFS mount point, e.g. "/sdcard"
 * @param out_card    Receives the card handle (may be NULL if not needed)
 * @return ESP_OK on success
 */
esp_err_t bsp_sd_mount(const char* mount_point, sdmmc_card_t** out_card);

/**
 * @brief Unmount the microSD card
 */
esp_err_t bsp_sd_unmount(void);

#ifdef __cplusplus
}
#endif
