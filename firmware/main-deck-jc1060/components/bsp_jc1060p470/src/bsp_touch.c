/**
 * @file bsp_touch.c
 * @brief Touch screen driver for JC1060P470C_I_W_Y (GT911)
 */

#include "bsp_board_config.h"
#include "bsp/touch.h"
#include "bsp_common.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

static const char* TAG = "bsp_touch";

static i2c_master_bus_handle_t g_touch_i2c_handle = NULL;
static esp_lcd_touch_handle_t g_touch_handle = NULL;

esp_err_t bsp_touch_new(const bsp_touch_config_t* config,
                        esp_lcd_touch_handle_t* ret_touch)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    ESP_RETURN_ON_FALSE(ret_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "Ret touch is NULL");

    // Initialize I2C for touch (software bit-banged to avoid GPIO conflict)
    if (g_touch_i2c_handle == NULL) {
        ESP_LOGI(TAG, "Initializing I2C for touch on GPIO%d/%d",
                 BSP_I2C_SW_SDA_GPIO, BSP_I2C_SW_SCL_GPIO);
        
        ret = bsp_i2c_init_sw(BSP_TOUCH_I2C_PORT,
                              BSP_I2C_SW_SDA_GPIO,
                              BSP_I2C_SW_SCL_GPIO,
                              BSP_TOUCH_I2C_CLK_SPEED_HZ,
                              &g_touch_i2c_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C for touch");
            return ret;
        }
    }

    // Configure GT911
    const esp_lcd_touch_config_t tp_config = {
        .x_max = config->x_max,
        .y_max = config->y_max,
        .rst_gpio_num = BSP_TOUCH_RST_GPIO,
        .int_gpio_num = BSP_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,  // Active low
            .interrupt = 0,  // Active low
        },
        .flags = {
            .swap_xy = config->swap_xy,
            .mirror_x = config->mirror_x,
            .mirror_y = config->mirror_y,
        },
    };

    // Create I2C panel IO for touch
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = BSP_TOUCH_I2C_CLK_SPEED_HZ;

    ret = esp_lcd_new_panel_io_i2c(g_touch_i2c_handle, &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create touch panel IO");
        return ret;
    }

    // Create GT911 touch panel
    esp_lcd_touch_handle_t tp = NULL;
    ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_config, &tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GT911 touch panel");
        esp_lcd_panel_io_del(tp_io_handle);
        return ret;
    }

    ESP_LOGI(TAG, "GT911 touch initialized: %dx%d, swap=%d, mirror_x=%d, mirror_y=%d",
             config->x_max, config->y_max,
             config->swap_xy, config->mirror_x, config->mirror_y);

    // Configure interrupt pin for touch events
    gpio_config_t int_config = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_INT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,  // Falling edge
    };
    gpio_config(&int_config);

    *ret_touch = tp;
    g_touch_handle = tp;

    return ESP_OK;
}

esp_lcd_touch_handle_t bsp_touch_start(void)
{
    static bsp_touch_config_t default_config = {
        .x_max = BSP_TOUCH_X_MAX,
        .y_max = BSP_TOUCH_Y_MAX,
        .swap_xy = BSP_TOUCH_SWAP_XY,
        .mirror_x = BSP_TOUCH_MIRROR_X,
        .mirror_y = BSP_TOUCH_MIRROR_Y,
    };

    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t ret = bsp_touch_new(&default_config, &tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start touch");
        return NULL;
    }

    return tp;
}

bool bsp_touch_read_coordinates(esp_lcd_touch_handle_t tp,
                                 uint16_t* x, uint16_t* y,
                                 uint16_t* strength, uint8_t* count,
                                 uint8_t max_count)
{
    if (tp == NULL || x == NULL || y == NULL || count == NULL) {
        return false;
    }

    // Read touch data
    esp_err_t ret = esp_lcd_touch_read_data(tp);
    if (ret != ESP_OK) {
        return false;
    }

    // Get coordinates
    uint16_t touch_x[1] = {0};
    uint16_t touch_y[1] = {0};
    uint16_t touch_strength[1] = {0};
    uint8_t touch_cnt = 0;

    bool touched = esp_lcd_touch_get_coordinates(tp,
                                                  touch_x, touch_y, touch_strength,
                                                  &touch_cnt, max_count);
    
    if (touched && touch_cnt > 0) {
        *x = touch_x[0];
        *y = touch_y[0];
        if (strength) *strength = touch_strength[0];
        *count = touch_cnt;
        
        ESP_LOGD(TAG, "Touch detected: (%d, %d)", *x, *y);
        return true;
    }

    *count = 0;
    return false;
}

esp_err_t bsp_touch_del(esp_lcd_touch_handle_t tp)
{
    if (tp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Deleting touch");

    // Delete touch panel
    esp_lcd_touch_delete(tp);
    g_touch_handle = NULL;

    // I2C bus deleted in bsp_board_deinit()

    return ESP_OK;
}
