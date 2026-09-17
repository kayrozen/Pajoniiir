/**
 * @file bsp_audio.c
 * @brief Audio driver for JC1060P470C_I_W_Y (ES8311 codec)
 */

#include "bsp_board_config.h"
#include "bsp/audio.h"
#include "bsp_common.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_es8311.h"
#include "esp_codec_default.h"

static const char* TAG = "bsp_audio";

static i2s_chan_handle_t g_i2s_tx_chan = NULL;
static esp_codec_dev_handle_t g_codec_dev = NULL;
static int g_current_volume_db = -20;  // Default volume
static bool g_initialized = false;

/**
 * @brief Configure I2S peripheral for ES8311
 */
static esp_err_t bsp_i2s_init(const bsp_audio_config_t* config)
{
    // I2S channel configuration
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(
        BSP_AUDIO_I2S_PORT,
        I2S_ROLE_MASTER
    );
    chan_config.auto_clear = true;  // Clear DMA buffer on stop

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_config, &g_i2s_tx_chan, NULL),
                        TAG, "Failed to create I2S channel");

    // I2S standard configuration
    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            config->bit_width,
            config->channel_format
        ),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK_GPIO,
            .bclk = BSP_AUDIO_I2S_SCLK_GPIO,
            .ws = BSP_AUDIO_I2S_LRCK_GPIO,
            .dout = BSP_AUDIO_I2S_DOUT_GPIO,
            .din = BSP_AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_i2s_tx_chan, &std_config),
                        TAG, "Failed to initialize I2S standard mode");

    ESP_LOGI(TAG, "I2S initialized: %d Hz, %d-bit, %s",
             config->sample_rate,
             config->bit_width,
             config->channel_format == I2S_SLOT_MODE_STEREO ? "Stereo" : "Mono");

    return ESP_OK;
}

/**
 * @brief Initialize ES8311 codec
 */
static esp_err_t bsp_codec_init(void)
{
    // Configure PA control GPIO
    if (BSP_AUDIO_PA_CTRL_GPIO != GPIO_NUM_NC) {
        gpio_config_t pa_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = (1ULL << BSP_AUDIO_PA_CTRL_GPIO),
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pa_config);
        gpio_set_level(BSP_AUDIO_PA_CTRL_GPIO, 1);  // Enable PA
        ESP_LOGD(TAG, "Audio PA enabled (GPIO%d)", BSP_AUDIO_PA_CTRL_GPIO);
    }

    // ES8311 codec configuration
    esp_codec_dev_es8311_config_t es8311_config = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,  // Playback + Record
        .master_mode = false,  // Slave mode (I2S master)
        .pa_pin = BSP_AUDIO_PA_CTRL_GPIO,
        .use_mclk = (BSP_AUDIO_I2S_MCLK_GPIO != GPIO_NUM_NC),
    };

    // Codec device configuration
    esp_codec_dev_cfg_t dev_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,  // Output device
        .codec_dev = &es8311_codec,
        .codec_if = &es8311_config,
        .cb_func = NULL,
        .cb_ctx = NULL,
    };

    g_codec_dev = esp_codec_dev_new(&dev_config);
    if (g_codec_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}

esp_err_t bsp_audio_init(const bsp_audio_config_t* config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Audio already initialized");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");

    ESP_LOGI(TAG, "Initializing audio");

    // Initialize I2S
    ESP_RETURN_ON_ERROR(bsp_i2s_init(config), TAG, "I2S init failed");

    // Initialize codec
    ESP_RETURN_ON_ERROR(bsp_codec_init(), TAG, "Codec init failed");

    // Set initial volume
    bsp_audio_set_volume(config->volume);

    g_initialized = true;
    ESP_LOGI(TAG, "Audio initialization complete");

    return ESP_OK;
}

i2s_chan_handle_t bsp_audio_get_i2s_tx_chan(void)
{
    return g_i2s_tx_chan;
}

esp_codec_dev_handle_t bsp_audio_get_codec_dev(void)
{
    return g_codec_dev;
}

esp_err_t bsp_audio_set_volume(int volume)
{
    if (g_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Volume range: -96 dB to 0 dB
    if (volume < -96) volume = -96;
    if (volume > 0) volume = 0;

    esp_err_t ret = esp_codec_dev_set_gain(g_codec_dev, volume);
    if (ret == ESP_OK) {
        g_current_volume_db = volume;
        ESP_LOGI(TAG, "Volume set to %d dB", volume);
    }

    return ret;
}

int bsp_audio_get_volume(void)
{
    return g_current_volume_db;
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    if (g_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mute) {
        return esp_codec_dev_set_mute(g_codec_dev, true);
    } else {
        // Restore previous volume
        return esp_codec_dev_set_gain(g_codec_dev, g_current_volume_db);
    }
}

esp_err_t bsp_audio_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing audio");

    // Delete codec device
    if (g_codec_dev != NULL) {
        esp_codec_dev_close(g_codec_dev);
        esp_codec_dev_delete(g_codec_dev);
        g_codec_dev = NULL;
    }

    // Delete I2S channel
    if (g_i2s_tx_chan != NULL) {
        i2s_del_channel(g_i2s_tx_chan);
        g_i2s_tx_chan = NULL;
    }

    // Reset PA GPIO
    if (BSP_AUDIO_PA_CTRL_GPIO != GPIO_NUM_NC) {
        gpio_set_level(BSP_AUDIO_PA_CTRL_GPIO, 0);
        gpio_reset_pin(BSP_AUDIO_PA_CTRL_GPIO);
    }

    g_initialized = false;
    return ESP_OK;
}
