/**
 * @file display.h
 * @brief Display interface for JC1060P470C_I_W_Y (JD9165)
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_types.h"
#include "driver/gpio.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display configuration structure
 */
typedef struct {
    uint32_t h_res;                  ///< Horizontal resolution
    uint32_t v_res;                  ///< Vertical resolution
    uint8_t bits_per_pixel;          ///< Color depth (16 for RGB565)
    bool double_buffer;              ///< Enable double buffering
    uint32_t buffer_size;            ///< Draw buffer size in pixels
    struct {
        unsigned int buff_dma: 1;    ///< Allocate buffer in DMA-capable memory
        unsigned int buff_spiram: 1; ///< Allocate buffer in SPIRAM
        unsigned int sw_rotate: 1;   ///< Enable software rotation
    } flags;
} bsp_display_cfg_t;

/**
 * @brief Display handles structure
 */
typedef struct {
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;   ///< MIPI-DSI bus handle
    esp_lcd_panel_io_handle_t io;            ///< Panel IO handle (DBI interface)
    esp_lcd_panel_handle_t panel;            ///< Panel handle (DPI interface)
    esp_lcd_panel_handle_t control;          ///< Control panel handle (optional)
} bsp_lcd_handles_t;

/**
 * @brief Initialize display with configuration
 * 
 * @param cfg Display configuration
 * @return Pointer to LVGL display handle, or NULL on failure
 */
lv_display_t* bsp_display_start_with_config(const bsp_display_cfg_t* cfg);

/**
 * @brief Initialize display with default configuration
 * 
 * @return Pointer to LVGL display handle, or NULL on failure
 */
lv_display_t* bsp_display_start(void);

/**
 * @brief Create new display panel (low-level)
 * 
 * @param config Display configuration
 * @param ret_panel Output panel handle
 * @param ret_io Output IO handle
 * @return ESP_OK on success
 */
esp_err_t bsp_display_new(const bsp_display_cfg_t* config, 
                          esp_lcd_panel_handle_t* ret_panel,
                          esp_lcd_panel_io_handle_t* ret_io);

/**
 * @brief Create new display panel with all handles
 * 
 * @param config Display configuration
 * @param ret_handles Output structure with all handles
 * @return ESP_OK on success
 */
esp_err_t bsp_display_new_with_handles(const bsp_display_cfg_t* config,
                                        bsp_lcd_handles_t* ret_handles);

/**
 * @brief Initialize backlight
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_display_brightness_init(void);

/**
 * @brief Set backlight brightness
 * 
 * @param brightness_percent Brightness in percent (0-100)
 * @return ESP_OK on success
 */
esp_err_t bsp_display_brightness_set(int brightness_percent);

/**
 * @brief Turn on backlight
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief Turn off backlight
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_display_backlight_off(void);

/**
 * @brief Rotate display
 * 
 * @param disp LVGL display handle
 * @param rotation Rotation angle (0, 90, 180, 270)
 */
void bsp_display_rotate(lv_display_t* disp, lv_display_rotation_t rotation);

/**
 * @brief Lock LVGL display for rendering
 * 
 * @param timeout_ms Timeout in milliseconds (-1 for infinite)
 * @return true if lock acquired
 */
bool bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief Unlock LVGL display
 */
void bsp_display_unlock(void);

/**
 * @brief Get LVGL input device (touch)
 * 
 * @return Pointer to LVGL input device
 */
lv_indev_t* bsp_display_get_input_dev(void);

/**
 * @brief Flush callback for LVGL
 * 
 * @param disp LVGL display
 * @param area Area to flush
 * @param color_map Color data
 */
void bsp_display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_map);

#ifdef __cplusplus
}
#endif
