#ifndef __LCD_H__
#define __LCD_H__ 

#ifdef __cplusplus
extern "C" {
#endif 

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lv_adapter_display.h"
#include "esp_lcd_jd9165.h"


#define JC1060P470      0
#define JC8012P4A1      1
#define JC4880P443      0


#if JC1060P470
#define HW_LCD_V_RES    600
#define HW_LCD_H_RES    1024

#elif JC8012P4A1
#define HW_LCD_V_RES    1280
#define HW_LCD_H_RES    800

#elif JC4880P443
#define HW_LCD_V_RES    800
#define HW_LCD_H_RES    480

#endif

void lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_panel_io_handle_t *io_handle, esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode, esp_lv_adapter_rotation_t rotation);
void touch_init(esp_lcd_touch_handle_t *ret_touch, esp_lv_adapter_rotation_t rotation);

void lcd_backlight_init(void);
void lcd_backlight_on(void);
void lcd_backlight_off(void);

//level 0-100
void lcd_backlight_set_level(int brightness_percent);


#ifdef __cplusplus
}
#endif

#endif