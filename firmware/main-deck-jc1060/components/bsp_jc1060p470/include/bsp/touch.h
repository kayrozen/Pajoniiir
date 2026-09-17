/**
 * @file touch.h
 * @brief Touch screen interface for JC1060P470C_I_W_Y (GT911)
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Touch configuration structure
 */
typedef struct {
    uint16_t x_max;                  ///< Max X coordinate
    uint16_t y_max;                  ///< Max Y coordinate
    bool swap_xy;                    ///< Swap X and Y axes
    bool mirror_x;                   ///< Mirror X axis
    bool mirror_y;                   ///< Mirror Y axis
} bsp_touch_config_t;

/**
 * @brief Initialize touch screen
 * 
 * @param config Touch configuration
 * @param ret_touch Output touch handle
 * @return ESP_OK on success
 */
esp_err_t bsp_touch_new(const bsp_touch_config_t* config, 
                        esp_lcd_touch_handle_t* ret_touch);

/**
 * @brief Initialize touch with default configuration
 * 
 * @return Pointer to touch handle, or NULL on failure
 */
esp_lcd_touch_handle_t bsp_touch_start(void);

/**
 * @brief Read touch coordinates
 * 
 * @param tp Touch handle
 * @param x Output X coordinate
 * @param y Output Y coordinate
 * @param strength Output touch strength (optional)
 * @param count Output number of touches
 * @param max_count Maximum number of touches to read
 * @return true if touch detected
 */
bool bsp_touch_read_coordinates(esp_lcd_touch_handle_t tp,
                                 uint16_t* x, uint16_t* y,
                                 uint16_t* strength, uint8_t* count,
                                 uint8_t max_count);

/**
 * @brief Deinitialize touch screen
 * 
 * @param tp Touch handle
 * @return ESP_OK on success
 */
esp_err_t bsp_touch_del(esp_lcd_touch_handle_t tp);

#ifdef __cplusplus
}
#endif
