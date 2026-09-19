/**
 * @file bsp_sd.c
 * @brief SDMMC (microSD) driver for JC1060P470C_I_W_Y
 *
 * Official pin map: CLK=43, CMD=44, D0-D3=39/40/41/42 (SDMMC slot 0, 4-bit).
 * Card power comes from the ESP32-P4 internal LDO channel 4 at 2.7 V, managed
 * through the official sd_pwr_ctrl_on_chip_ldo helper of the SDMMC driver.
 */

#include "bsp_board_config.h"
#include "bsp/sd.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "ff.h"
#include "esp_vfs_fat.h"

static const char* TAG = "bsp_sd";

static sdmmc_card_t s_card;
static sd_pwr_ctrl_handle_t s_ldo_handle = NULL;
static bool s_mounted = false;

esp_err_t bsp_sd_mount(const char* mount_point, sdmmc_card_t** out_card)
{
    if (s_mounted) {
        if (out_card) *out_card = &s_card;
        return ESP_OK;
    }

    /* Power the card socket from the on-chip LDO channel 4 (2.7 V). */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_ldo_handle),
                        TAG, "Failed to create on-chip LDO power control (chan 4)");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = BSP_SDMMC_SLOT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* driver falls back if unsupported */
    host.pwr_ctrl_handle = s_ldo_handle;      /* IDF6: power control lives on the host */

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = BSP_SDMMC_CLK_GPIO;
    slot_config.cmd = BSP_SDMMC_CMD_GPIO;
    slot_config.d0  = BSP_SDMMC_D0_GPIO;
    slot_config.d1  = BSP_SDMMC_D1_GPIO;
    slot_config.d2  = BSP_SDMMC_D2_GPIO;
    slot_config.d3  = BSP_SDMMC_D3_GPIO;
    slot_config.width = 4;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t* card_ptr = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,
                                            &mount_config, &card_ptr);
    if (ret == ESP_OK) {
        s_card = *card_ptr;
    }
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem (no FAT partition?)");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (inserted? %s)",
                     esp_err_to_name(ret));
        }
        sd_pwr_ctrl_del_on_chip_ldo(s_ldo_handle);
        s_ldo_handle = NULL;
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, &s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    if (out_card) *out_card = &s_card;
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdcard_unmount("/sdcard", &s_card),                        TAG, "Failed to unmount");
    s_mounted = false;
    if (s_ldo_handle != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_ldo_handle);
        s_ldo_handle = NULL;
    }
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}
