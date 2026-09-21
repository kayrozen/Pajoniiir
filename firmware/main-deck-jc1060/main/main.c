/**
 * @file main.c
 * @brief Pajoniiir main deck application (ESP32-P4 JC1060P470C).
 *
 * Real application skeleton: brings up all validated hardware (display,
 * touch, audio codec, SD, Wi-Fi console, USB host + DDJ MIDI, Ethernet,
 * OTA) and runs the UI loop. Deck logic grows from here.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "bsp/board.h"
#include "bsp/display.h"
#include "log_screen.h"
#include "bsp/touch.h"
#include "bsp/audio.h"
#include "bsp/sd.h"
#include "usb_tu_app.h"
#include "eth_bringup.h"
#include "wifi_console.h"
#include "ota_update.h"
#include "esp_app_desc.h"

static const char* TAG = "deck";

void app_main(void)
{
    ESP_LOGI(TAG, "Pajoniiir main deck v%s", esp_app_get_description()->version);

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_LOGI(TAG, "Board: %s", bsp_board_get_name());

    /* Display + LVGL. */
#if 0 /* v101 differential test: display OFF (DSI/LDO/MIPI-PLL vs EMAC) */
    bsp_display_cfg_t disp_cfg = {
        .h_res = BSP_LCD_H_RES,
        .v_res = BSP_LCD_V_RES,
        .bits_per_pixel = 24,
        .double_buffer = false,
        .buffer_size = BSP_LCD_H_RES * 50,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    lv_display_t* disp = bsp_display_start_with_config(&disp_cfg);
    if (disp != NULL) {
        ESP_LOGI(TAG, "Display initialized");
    } else {
        ESP_LOGE(TAG, "Display init failed");
    }
#endif /* v101 display OFF for differential test */

    /* On-screen verbose log (teed). */
    log_screen_start();
    ESP_LOGI(TAG, "log screen test line");

    /* Audio codec (internal outs; DDJ audio route comes next). */
#if 0 /* v102 differential: audio OFF (I2S/ES8311 vs EMAC) */
    bsp_audio_config_t audio_cfg = {
        .sample_rate = 44100,
        .bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .channel_format = I2S_SLOT_MODE_STEREO,
        .volume = -20,
    };
    if (bsp_audio_init(&audio_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Audio initialized");
    } else {
        ESP_LOGW(TAG, "Audio init failed");
    }
#endif /* v102 audio OFF */

    bsp_display_backlight_on();

    /* NVS + Wi-Fi console via C6 co-processor (before SD mount, see bsp_sd). */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_ret));
    }
#if 0 /* v89: Wi-Fi parked - RPC hangs under IDF 6.0.2 (never tested upstream,
           CI = 5.3-5.5 only). Full investigation in docs/recherche/. */
    if (!wifi_console_start()) {
        ESP_LOGW(TAG, "Wi-Fi console not available");
    }
    /* The Wi-Fi console installs its own log vprintf and drops ours - rehook. */
    log_screen_rehook();
    ota_update_start();
#endif

    /* v97: TCP console over Ethernet only (Wi-Fi parked). NOTE: do NOT call
     * log_screen_rehook() after console_tcp_start() - console chains into
     * ls via its captured prev, and re-capturing would create an
     * ls->console->ls recursion (stack overflow). */
    if (!console_tcp_start()) {
        ESP_LOGW(TAG, "TCP console not available");
    }

    /* SD card (music library). */
    sdmmc_card_t* card = NULL;
    if (bsp_sd_mount("/sdcard", &card) == ESP_OK) {
        ESP_LOGI(TAG, "SD mounted");
    } else {
        ESP_LOGW(TAG, "SD card not available");
    }

    /* v96 differential test: USB OFF - the TinyUSB UTMI PHY may share
     * clock/power resources with the EMAC. */
    /* usb_tu_start(); */
    eth_bringup_start();

    ESP_LOGI(TAG, "Bring-up complete - entering UI loop");

    /* UI loop placeholder: log screen refreshes here (full deck UI replaces
     * this). Rendering itself is done by esp_lvgl_port's task. */
    while (1) {
        /* v53 bisect: DSI refresh back ON (clamp stays reverted). If this
         * re-introduces glitches, the DSI log flush is the root cause. */
        log_screen_task();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
