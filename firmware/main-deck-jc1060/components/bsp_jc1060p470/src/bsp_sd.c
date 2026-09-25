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
#include "esp_idf_version.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "ff.h"
#include "esp_vfs_fat.h"

/*
 * Coexistence workaround (esp_hosted example `host_sdcard_with_hosted`):
 * with ESP-Hosted on SDIO (IDF >= 6.0), the SDMMC host controller is already
 * initialised by ESP-Hosted - the SD card code must not init/deinit it again.
 */
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 1
#else
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 0
#endif

static const char* TAG = "bsp_sd";

static sdmmc_card_t s_card;
static sd_pwr_ctrl_handle_t s_ldo_handle = NULL;
static bool s_mounted = false;

/* v233: failed mount attempts so far; retries skip the high-speed switch. */
static int s_mount_failures = 0;

#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
/* The SDMMC host controller is already managed by ESP-Hosted. */
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }

/*
 * v233: slot-only teardown for mount retries. ESP-Hosted's SDIO slot and the
 * microSD slot share one SDMMC controller. sdmmc_host_deinit_slot() removes a
 * slot and then deletes the controller when no slot is left, without
 * clearing the legacy driver's controller pointer; the next dummy init then
 * adds the slot to a freed controller ("slot is not available" on every
 * cold-boot retry). s_ctlr_live guards every read of that pointer and
 * s_host_owned switches to a real sdmmc_host_init() once the controller is
 * gone.
 */
static bool s_host_owned = false;
static bool s_ctlr_live = true;  /* ESP-Hosted's constructor created it */

/* Returns -1 when there is no live controller. */
static int registered_slots(void)
{
    sdmmc_host_state_t st = {0};
    if (!s_ctlr_live || sdmmc_host_get_state(&st) != ESP_OK ||
        !st.host_initialized) {
        return -1;
    }
    return st.num_of_init_slots;
}

static esp_err_t sdmmc_host_init_shared(void)
{
    if (!s_host_owned) {
        return ESP_OK;
    }
    const esp_err_t ret = sdmmc_host_init();
    s_ctlr_live = (ret == ESP_OK);
    return ret;
}

/* Also the deinit_p hook the FATFS helper calls on a failed mount/unmount.
 * Idempotent: a slot that is not registered is left alone. */
static esp_err_t sdmmc_host_release_slot(int slot)
{
    const int slots = registered_slots();
    if (slots <= 0) {
        return ESP_OK;
    }
    const esp_err_t ret = sdmmc_host_deinit_slot(slot);
    if (ret == ESP_OK && slots == 1) {
        /* It was the last slot: the driver deleted the controller too. */
        s_host_owned = true;
        s_ctlr_live = false;
        ESP_LOGW(TAG, "SDMMC controller released with slot %d "
                      "(ESP-Hosted slot absent); next attempt re-creates it",
                 slot);
    }
    return ret;
}
#endif

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
    if (s_mount_failures > 0) {
        /* v233: cold-boot CMD6 high-speed switch timed out (0x107); retry at
         * default speed, which skips CMD6. */
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    }
    host.pwr_ctrl_handle = s_ldo_handle;      /* IDF6: power control lives on the host */

#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
    /* ESP-Hosted (SDIO slot 1) has already initialised the SDMMC host
     * controller - skip its init/deinit here (see esp_hosted example
     * `host_sdcard_with_hosted`). */
    if (!s_host_owned && registered_slots() < 0) {
        /* v233: no controller to share; create our own. */
        s_host_owned = true;
        s_ctlr_live = false;
    }
    host.init = &sdmmc_host_init_shared;
    host.deinit = &sdmmc_host_deinit_dummy;
    host.deinit_p = &sdmmc_host_release_slot;
#endif

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
        /* v233: the FATFS helper already released the slot through
         * host.deinit_p (sdmmc_host_release_slot), so the caller can retry. */
        sd_pwr_ctrl_del_on_chip_ldo(s_ldo_handle);
        s_ldo_handle = NULL;
        ++s_mount_failures;
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
