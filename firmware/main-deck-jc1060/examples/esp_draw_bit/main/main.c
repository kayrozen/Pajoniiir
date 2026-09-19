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

static const char* TAG = "draw_bit";

#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_WHITE     0xFFFF
#define COLOR_BLACK     0x0000
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF
#define COLOR_MAGENTA   0xF81F

static esp_lcd_panel_handle_t g_panel = NULL;
/**
 * @brief Fill rectangle with color
 */
static void fill_rect(esp_lcd_panel_handle_t panel, 
                      int x, int y, int w, int h, 
                      uint16_t color)
{
    size_t pixel_count = w * h;
    uint16_t* color_line = heap_caps_malloc(pixel_count * sizeof(uint16_t), 
                                             MALLOC_CAP_DMA);
    if (color_line == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return;
    }

    // Fill buffer with color
    for (size_t i = 0; i < pixel_count; i++) {
        color_line[i] = color;
    }

    // Draw to display
    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, color_line);

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
    uint16_t colors[] = {
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
 * @brief Draw gradient test
 */
static void draw_gradient(esp_lcd_panel_handle_t panel, bool horizontal)
{
    int h_res = BSP_LCD_H_RES;
    int v_res = BSP_LCD_V_RES;

    ESP_LOGI(TAG, "Drawing %s gradient", horizontal ? "horizontal" : "vertical");

    for (int y = 0; y < v_res; y++) {
        for (int x = 0; x < h_res; x++) {
            uint16_t color;
            if (horizontal) {
                // Horizontal gradient: left to right
                uint8_t r = (x * 31) / h_res;
                uint8_t g = ((x % 256) * 63) / 256;
                uint8_t b = ((x % 32) * 31) / 32;
                color = ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F);
            } else {
                // Vertical gradient: top to bottom
                uint8_t r = (y * 31) / v_res;
                uint8_t g = ((y % 256) * 63) / 256;
                uint8_t b = ((y % 32) * 31) / 32;
                color = ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F);
            }

            esp_lcd_panel_draw_bitmap(panel, x, y, x + 1, y + 1, &color);
        }
    }
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
            uint16_t white = COLOR_WHITE;
            int radius = 20;
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    if (dx*dx + dy*dy <= radius*radius) {
                        int px = x + dx;
                        int py = y + dy;
                        if (px >= 0 && px < BSP_LCD_H_RES && 
                            py >= 0 && py < BSP_LCD_V_RES) {
                            esp_lcd_panel_draw_bitmap(g_panel, px, py, 
                                                       px + 1, py + 1, &white);
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
        .bits_per_pixel = 16,
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
    } else {
        ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(audio_ret));
    }

    // Turn on backlight
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Backlight on");

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
