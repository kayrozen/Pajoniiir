/**
 * @file bsp_board.c
 * @brief Board-level initialization for JC1060P470C_I_W_Y
 */

#include "bsp_board_config.h"
#include "bsp_common.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_private/esp_clk.h"

static const char* TAG = "bsp_board";

/** @brief Backlight PWM channel */
#define BACKLIGHT_LEDC_CHAN    LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_TIMER   LEDC_TIMER_0
#define BACKLIGHT_FREQ_HZ      (5000)
#define BACKLIGHT_DUTY_MAX     (1023)

/** @brief Shared software-I2C master bus (touch + audio codec) */
static i2c_master_bus_handle_t g_i2c_shared_handle = NULL;

static bool g_board_initialized = false;

esp_err_t bsp_board_init(void)
{
    if (g_board_initialized) {
        ESP_LOGI(TAG, "Board already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing board %s", BSP_BOARD_NAME);

    // Configure backlight GPIO
    ESP_LOGD(TAG, "Configuring backlight GPIO%d", BSP_LCD_BL_GPIO);
    gpio_config_t bl_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BSP_LCD_BL_GPIO),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_gpio_config));
    gpio_set_level(BSP_LCD_BL_GPIO, 0); // Start with backlight off

    // Configure backlight PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = BACKLIGHT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_LEDC_CHAN,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BSP_LCD_BL_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    // Initialize shared I2C master (GT911 + ES8311) on GPIO7/8
#if BSP_USE_SW_I2C_FOR_TOUCH
    ESP_LOGI(TAG, "Initializing shared I2C master on GPIO%d/%d",
             BSP_I2C_SW_SDA_GPIO, BSP_I2C_SW_SCL_GPIO);
    ESP_ERROR_CHECK(bsp_i2c_init_sw(BSP_TOUCH_I2C_PORT,
                                     BSP_I2C_SW_SDA_GPIO,
                                     BSP_I2C_SW_SCL_GPIO,
                                     BSP_TOUCH_I2C_CLK_SPEED_HZ,
                                     &g_i2c_shared_handle));
#else
    // Hardware I2C (will conflict with I2S LRCK)
    ESP_LOGW(TAG, "Using hardware I2C - potential GPIO12 conflict!");
#endif

    // Initialize software I2C for audio codec
#if BSP_USE_SW_I2C_FOR_AUDIO
    // The audio codec shares the software-I2C bus used by touch.
    ESP_LOGI(TAG, "Audio codec will share software I2C bus (GT911=0x5D, ES8311=0x18)");
#endif

    // Configure UART TX/RX pins for control link (done in control_link component)
    // GPIO28 (TX) and GPIO29 (RX) configured by UART driver

    // Configure USB host pins (if enabled)
#if BSP_USB_HOST_ENABLED
    ESP_LOGI(TAG, "USB host enabled (GPIO%d/%d)", BSP_USB_DM_GPIO, BSP_USB_DP_GPIO);
    // USB host initialization done in usb_storage component
#endif

    // Configure SDMMC pins (if enabled)
#if BSP_SDMMC_ENABLED
    ESP_LOGI(TAG, "SDMMC enabled (GPIO%d-%d)", BSP_SDMMC_D0_GPIO, BSP_SDMMC_CLK_GPIO);
    // SDMMC initialization done in sd_io_gate component
#endif

    // Configure ESP-Hosted GPIOs
#if BSP_ESP_HOSTED_ENABLED
    if (BSP_ESP_HOSTED_RST_GPIO != GPIO_NUM_NC) {
        gpio_config_t rst_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = (1ULL << BSP_ESP_HOSTED_RST_GPIO),
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_config));
        // Release C6 from reset (temporarily, to test C6 USB enumeration)
        gpio_set_level(BSP_ESP_HOSTED_RST_GPIO, 1);
        ESP_LOGI(TAG, "ESP32-C6 released from reset (GPIO%d)", BSP_ESP_HOSTED_RST_GPIO);
    }
#endif

    g_board_initialized = true;
    ESP_LOGI(TAG, "Board initialization complete");
    
    return ESP_OK;
}

esp_err_t bsp_board_deinit(void)
{
    if (!g_board_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing board");

    // Turn off backlight
    bsp_backlight_off();

    // Deinitialize I2C
    if (g_i2c_shared_handle != NULL) {
        bsp_i2c_deinit(g_i2c_shared_handle);
        g_i2c_shared_handle = NULL;
    }

    // Reset GPIOs
    gpio_reset_pin(BSP_LCD_BL_GPIO);
    
#if BSP_ESP_HOSTED_ENABLED
    if (BSP_ESP_HOSTED_RST_GPIO != GPIO_NUM_NC) {
        gpio_reset_pin(BSP_ESP_HOSTED_RST_GPIO);
    }
#endif

    g_board_initialized = false;
    return ESP_OK;
}

const char* bsp_board_get_name(void)
{
    return BSP_BOARD_NAME;
}

esp_err_t bsp_i2c_init_sw(i2c_port_t port, int sda_gpio, int scl_gpio,
                          uint32_t clk_speed_hz, i2c_master_bus_handle_t* i2c_handle)
{
    // Hardware i2c_master on the board's dedicated I2C pins (GPIO7/8).
    // The board has external pull-ups, so internal ones stay disabled.
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .scl_io_num = scl_gpio,
        .sda_io_num = sda_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize shared I2C bus");
        return ret;
    }

    ESP_LOGI(TAG, "Shared I2C bus initialized on GPIO%d (SDA), GPIO%d (SCL)",
             sda_gpio, scl_gpio);

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(i2c_master_bus_handle_t i2c_handle)
{
    if (i2c_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return i2c_del_master_bus(i2c_handle);
}

esp_err_t bsp_backlight_set_brightness(uint8_t duty_percent)
{
    if (duty_percent > 100) {
        duty_percent = 100;
    }

    uint32_t duty = (duty_percent * BACKLIGHT_DUTY_MAX) / 100;
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHAN, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHAN);
    
    return ESP_OK;
}

esp_err_t bsp_backlight_on(void)
{
    return bsp_backlight_set_brightness(100);
}

esp_err_t bsp_backlight_off(void)
{
    return bsp_backlight_set_brightness(0);
}

const char* bsp_board_get_revision(void)
{
    // TODO: Read from EEPROM or GPIO strapping if available
    return "1.0";
}

i2c_master_bus_handle_t bsp_i2c_get_shared(void)
{
    return g_i2c_shared_handle;
}
