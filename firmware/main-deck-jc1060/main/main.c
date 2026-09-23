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
#include "usb_storage.h"
#include "controller_bootstrap.h"
#include "usb_spike.h"
#include "esp_app_desc.h"

static const char* TAG = "deck";

void app_main(void)
{
    ESP_LOGI(TAG, "Pajoniiir main deck v%s", esp_app_get_description()->version);

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_LOGI(TAG, "Board: %s", bsp_board_get_name());

    /* Display + LVGL. */
#if 1 /* v106: display back ON (differential tests over) */
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
#endif /* v106 display ON */

    /* On-screen verbose log (teed). */
    log_screen_start();
    ESP_LOGI(TAG, "log screen test line");

    /* Audio codec (internal outs; DDJ audio route comes next). */
#if 1 /* v109 bisect: audio ON only */
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
#endif /* v109 audio ON */

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
#endif
    /* v121: OTA pull over Ethernet - the console TCP client (:2333) IP becomes
     * the OTA host (ota_update_set_host), serving <project>.bin on :8080. */
    ota_update_start();

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
    /* v136: TinyUSB DISABLED - diagnostic. usb_tu_start() claims OTG_HS via
     * its own DWC2 driver; the proven dual-host configuration is ONE
     * usb_host_install owning BOTH ports (esp-usb PR #402, issue #396).
     * If the drive enumerates with TinyUSB off, the cohabitation is the
     * culprit. */
    eth_bringup_start(); /* v111 bisect: ETH ON - flicker suspect */

    /* v157: back to the ported upstream stack (manager DUAL, override
     * FS PHY0, MSC root 0), with the USB stack debug logs unlocked
     * (CONFIG_LOG_MAXIMUM_LEVEL=4) so HCD/hub/USBH port transitions are
     * finally visible. TinyUSB path confirmed working (v156). */
    esp_log_level_set("USB HOST", ESP_LOG_DEBUG);
    esp_log_level_set("USBH", ESP_LOG_DEBUG);
    esp_log_level_set("HUB", ESP_LOG_DEBUG);
    esp_log_level_set("usb_phy", ESP_LOG_DEBUG);
    esp_log_level_set("usb_host_mgr", ESP_LOG_DEBUG);
    esp_log_level_set("usb_storage", ESP_LOG_DEBUG);
    esp_log_level_set("USB_MSC", ESP_LOG_DEBUG);
    esp_log_level_set("USB_MSC_SCSI", ESP_LOG_DEBUG);
    esp_log_level_set("MSC VFS", ESP_LOG_DEBUG);
    esp_log_level_set("diskio_usb", ESP_LOG_DEBUG);
    /* v160: upstream-faithful USB stack only - usb_storage (upstream copy)
     * + usb_host_manager DUAL. TinyUSB removed from the product (v156 was
     * diagnostic only). USB_SERIAL_JTAG no longer claims PHY0: console is
     * now UART0 (BOOT-mode flashing unaffected). The v158 manager crash
     * during FS init was the USB-JTAG driver owning the PHY - gone. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_storage_init(NULL));
    /* v163: controller path (MIDI + UAC) on the HS root - upstream
     * p4_local_controller flow (manager ready -> controller_usb_host_init).
     * Profile/runtime layers not ported yet: MIDI/connection logged. */
    controller_bootstrap_start();

    /* v149 J0 spike: usb_storage DISABLED (it would install a second host
     * lib). usb_spike owns BOTH ports in ONE usb_host_install(BIT0|BIT1)
     * and logs DEV_NEW VID:PID - verifies dual-port ownership + where the
     * DDJ and the stick enumerate. */

    /* v129: log the reset reason - distinguishes a PANIC/WDT crash from a
     * brownout (power) reboot on the TCP console, since UART is gone. */
    ESP_LOGW(TAG, "boot: reset reason=%d, running v%s",
             (int)esp_reset_reason(), esp_app_get_description()->version);
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
