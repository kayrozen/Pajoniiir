#include <stdio.h>

#include "lvgl.h"
#include "lv_demos.h"
#include "esp_lv_adapter.h"
#include "lcd.h"

static lv_display_t *s_disp = NULL;
static lv_indev_t *s_touch = NULL;
void app_main(void)
{
    lcd_backlight_init();
    lcd_backlight_on();
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI;

    lcd_init(&panel, &io, tear_avoid_mode, rotation);

    const esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    esp_lv_adapter_init(&adapter_config);

    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
                                                         panel, io, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
    s_disp = esp_lv_adapter_register_display(&display_config);

    esp_lcd_touch_handle_t touch_handle = NULL;
    touch_init(&touch_handle, rotation);

    const esp_lv_adapter_touch_config_t touch_cfg = {
        .disp = s_disp,
        .handle = touch_handle,
        .scale = { .x = 1.0f, .y = 1.0f },
    };
    s_touch = esp_lv_adapter_register_touch(&touch_cfg);
    esp_lv_adapter_start();

    if(esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_demo_widgets();
        esp_lv_adapter_unlock();
    }

}
