/**
 * @file main.c
 * @brief Draw bitmap test for JC1060P470C_I_W_Y
 * 
 * This example tests:
 * - Display initialization (JD9165)
 * - Touch screen (GT911)
 * - Basic graphics rendering
 * 
 * Shows colored rectangles cycling on the display.
 * Touch events are logged to serial.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"

#include "bsp/board.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "bsp/audio.h"
#include "bsp/sd.h"
#include "usb_host_bringup.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

static const char* TAG = "draw_bit";

/* RGB888 (24-bit) colors - the JD9165 factory config uses PIXEL_RGB888 */
#define COLOR_RED       0xFF0000U
#define COLOR_GREEN     0x00FF00U
#define COLOR_BLUE      0x0000FFU
#define COLOR_WHITE     0xFFFFFFU
#define COLOR_BLACK     0x000000U
#define COLOR_YELLOW    0xFFFF00U
#define COLOR_CYAN      0x00FFFFU
#define COLOR_MAGENTA   0xFF00FFU

static esp_lcd_panel_handle_t g_panel = NULL;

static void fill_rect(esp_lcd_panel_handle_t panel,
                      int x, int y, int w, int h,
                      uint32_t color)
{
    // Row-by-row fill with a single line buffer (RGB888: 3 bytes per pixel).
    size_t line_bytes = (size_t)w * 3;
    uint8_t* color_line = heap_caps_malloc(line_bytes, MALLOC_CAP_DMA);
    if (color_line == NULL) {
        ESP_LOGE(TAG, "Failed to allocate fill line buffer");
        return;
    }

    color_line[0] = (uint8_t)((color >> 16) & 0xFF);  /* R */
    color_line[1] = (uint8_t)((color >> 8) & 0xFF);   /* G */
    color_line[2] = (uint8_t)(color & 0xFF);          /* B */
    for (size_t i = 1; i < (size_t)w; i++) {
        memcpy(&color_line[i * 3], color_line, 3);
    }
    for (int yy = y; yy < y + h; yy++) {
        esp_lcd_panel_draw_bitmap(panel, x, yy, x + w, yy + 1, color_line);
    }

    heap_caps_free(color_line);
}

/**
 * @brief Draw color bars test pattern
 */
static void draw_color_bars(esp_lcd_panel_handle_t panel)
{
    int h_res = BSP_LCD_H_RES;
    int v_res = BSP_LCD_V_RES;
    int bar_width = h_res / 8;

    ESP_LOGI(TAG, "Drawing color bars: %dx%d", h_res, v_res);

    // Draw 8 color bars
    uint32_t colors[] = {
        COLOR_BLACK,   // 0
        COLOR_BLUE,    // 1
        COLOR_GREEN,   // 2
        COLOR_CYAN,    // 3
        COLOR_RED,     // 4
        COLOR_MAGENTA, // 5
        COLOR_YELLOW,  // 6
        COLOR_WHITE,   // 7
    };

    for (int i = 0; i < 8; i++) {
        fill_rect(panel, i * bar_width, 0, bar_width, v_res, colors[i]);
    }
}

/**
 * @brief Draw gradient test, row-by-row (fast, keeps CPU0 responsive)
 *
 * Both variants draw full-screen line strips with a bounded number of
 * draw_bitmap calls (<= 600), avoiding thousands of 1-pixel transfers and
 * giving main() regular yields so the task watchdog is never starved.
 */
static void draw_gradient(esp_lcd_panel_handle_t panel, bool horizontal)
{
    int h_res = BSP_LCD_H_RES;
    int v_res = BSP_LCD_V_RES;

    ESP_LOGI(TAG, "Drawing %s gradient", horizontal ? "horizontal" : "vertical");

    size_t line_bytes = (size_t)h_res * 3;
    uint8_t* line = heap_caps_malloc(line_bytes, MALLOC_CAP_DMA);
    if (line == NULL) {
        ESP_LOGE(TAG, "Failed to allocate gradient line buffer");
        return;
    }

    for (int y = 0; y < v_res; y++) {
        for (int x = 0; x < h_res; x++) {
            uint8_t r, g, b;
            if (horizontal) {
                // Horizontal gradient: left to right per row.
                r = (uint8_t)((x * 255) / h_res);
                g = (uint8_t)((x * 127) / h_res);
                b = (uint8_t)(255 - (x * 255) / h_res);
            } else {
                // Vertical gradient: top to bottom, each row uniform.
                r = (uint8_t)((y * 255) / v_res);
                g = (uint8_t)((y * 127) / v_res);
                b = (uint8_t)(255 - (y * 255) / v_res);
            }
            line[x * 3 + 0] = r;
            line[x * 3 + 1] = g;
            line[x * 3 + 2] = b;
        }
        esp_lcd_panel_draw_bitmap(panel, 0, y, h_res, y + 1, line);
        // Keep CPU0 alive so the task watchdog does not fire during long fills.
        if ((y & 0x3F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    heap_caps_free(line);
}

/**
 * @brief Touch monitoring task
 */
static void touch_monitor_task(void* pvParameters)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)pvParameters;
    
    while (1) {
        uint16_t x, y, strength;
        uint8_t count;

        if (bsp_touch_read_coordinates(tp, &x, &y, &strength, &count, 1)) {
            ESP_LOGI(TAG, "Touch: (%d, %d) strength=%d", x, y, strength);
            
            // Draw circle at touch position
            const uint8_t white[3] = {0xFF, 0xFF, 0xFF};
            int radius = 20;
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    if (dx*dx + dy*dy <= radius*radius) {
                        int px = x + dx;
                        int py = y + dy;
                        if (px >= 0 && px < BSP_LCD_H_RES && 
                            py >= 0 && py < BSP_LCD_V_RES) {
                            esp_lcd_panel_draw_bitmap(g_panel, px, py, 
                                                       px + 1, py + 1, white);
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // 20 Hz polling
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "JC1060P470C Draw Bitmap Test");
    ESP_LOGI(TAG, "============================");

    // Initialize board
    ESP_ERROR_CHECK(bsp_board_init());
    ESP_LOGI(TAG, "Board: %s", bsp_board_get_name());

    // Initialize display
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
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }
    ESP_LOGI(TAG, "Display initialized");

    // Get panel handle for direct drawing
    // (In production, use LVGL only)
    g_panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    // Initialize touch
    esp_lcd_touch_handle_t tp = bsp_touch_start();
    if (tp != NULL) {
        ESP_LOGI(TAG, "Touch initialized");
        
        // Start touch monitoring task
        xTaskCreate(touch_monitor_task, "touch_mon", 4096, tp, 5, NULL);
    } else {
        ESP_LOGW(TAG, "Touch not available");
    }

    // Initialize audio (optional test)
    bsp_audio_config_t audio_cfg = {
        .sample_rate = 44100,
        .bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .channel_format = I2S_SLOT_MODE_STEREO,
        .volume = -20,
    };
    esp_err_t audio_ret = bsp_audio_init(&audio_cfg);
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "Audio initialized");
        bsp_audio_test_tone();
    } else {
        ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(audio_ret));
    }

    // Turn on backlight
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Backlight on");

    // SD card bring-up test (non-fatal if no card inserted)
    sdmmc_card_t* card = NULL;
    if (bsp_sd_mount("/sdcard", &card) == ESP_OK) {
        FILE* f = fopen("/sdcard/bringup.txt", "w");
        if (f != NULL) {
            fprintf(f, "JC1060P470C bring-up %s\n", "OK");
            fclose(f);
            ESP_LOGI(TAG, "Wrote /sdcard/bringup.txt");
        } else {
            ESP_LOGE(TAG, "Failed to write /sdcard/bringup.txt");
        }
    } else {
        ESP_LOGW(TAG, "SD card not available");
    }

    // USB host bring-up (enumeration of devices behind the hub)
    usb_host_bringup_start();

    // Wi-Fi bring-up via ESP32-C6 co-processor (ESP-Hosted, SDIO)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_ret));
    } else {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t wifi_ret = esp_wifi_init(&wifi_cfg);
        if (wifi_ret == ESP_OK) {
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_start());
            ESP_LOGI(TAG, "Wi-Fi started (via C6 co-processor)");
        } else {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(wifi_ret));
        }
    }

    // Main test loop
    int test_pattern = 0;
    while (1) {
        ESP_LOGI(TAG, "Test pattern %d", test_pattern);

        switch (test_pattern % 4) {
            case 0:
                // Color bars
                draw_color_bars(g_panel);
                break;

            case 1:
                // Horizontal gradient
                draw_gradient(g_panel, true);
                break;

            case 2:
                // Vertical gradient
                draw_gradient(g_panel, false);
                break;

            case 3:
                // Solid colors (fullscreen)
                fill_rect(g_panel, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, COLOR_RED);
                vTaskDelay(pdMS_TO_TICKS(1000));
                fill_rect(g_panel, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, COLOR_GREEN);
                vTaskDelay(pdMS_TO_TICKS(1000));
                fill_rect(g_panel, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, COLOR_BLUE);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }

        test_pattern++;
        vTaskDelay(pdMS_TO_TICKS(3000));  // Show each pattern for 3 seconds
    }
}
