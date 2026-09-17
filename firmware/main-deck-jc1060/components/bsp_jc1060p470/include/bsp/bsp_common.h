/**
 * @file bsp_common.h
 * @brief Common BSP functions for JC1060P470C_I_W_Y
 */

#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize board-level peripherals
 * 
 * This function initializes:
 * - GPIO configuration
 * - Power management (LDOs for display, codec)
 * - System clocks
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_board_init(void);

/**
 * @brief Deinitialize board peripherals
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_board_deinit(void);

/**
 * @brief Get board information
 * 
 * @return Board name string
 */
const char* bsp_board_get_name(void);

/**
 * @brief Initialize I2C bus (software bit-banged to avoid GPIO conflicts)
 * 
 * @param port I2C port number
 * @param sda_gpio SDA GPIO pin
 * @param scl_gpio SCL GPIO pin
 * @param clk_speed_hz Clock speed in Hz
 * @param i2c_handle Output handle
 * @return ESP_OK on success
 */
esp_err_t bsp_i2c_init_sw(i2c_port_t port, int sda_gpio, int scl_gpio, 
                          uint32_t clk_speed_hz, i2c_master_bus_handle_t* i2c_handle);

/**
 * @brief Deinitialize I2C bus
 * 
 * @param i2c_handle I2C handle to deinitialize
 * @return ESP_OK on success
 */
esp_err_t bsp_i2c_deinit(i2c_master_bus_handle_t i2c_handle);

/**
 * @brief Configure backlight PWM
 * 
 * @param duty_percent Brightness in percent (0-100)
 * @return ESP_OK on success
 */
esp_err_t bsp_backlight_set_brightness(uint8_t duty_percent);

/**
 * @brief Turn on backlight
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_backlight_on(void);

/**
 * @brief Turn off backlight
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_backlight_off(void);

/**
 * @brief Read board revision/variant (if available)
 * 
 * @return Revision string or NULL if not available
 */
const char* bsp_board_get_revision(void);

#ifdef __cplusplus
}
#endif
