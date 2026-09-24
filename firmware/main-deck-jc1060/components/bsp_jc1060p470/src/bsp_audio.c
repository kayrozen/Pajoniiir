/**
 * @file bsp_audio.c
 * @brief Audio driver for JC1060P470C_I_W_Y (ES8311 codec)
 *
 * Uses the modern esp_codec_dev API (ES8311 device bundled in the package).
 */

#include "bsp_board_config.h"
#include "bsp/audio.h"
#include "bsp_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include <math.h>

static const char* TAG = "bsp_audio";

static i2s_chan_handle_t       g_i2s_tx_chan = NULL;
static esp_codec_dev_handle_t  g_codec_dev   = NULL;
static const audio_codec_if_t *g_codec_if    = NULL;
static int g_current_volume_db = 0;   /* Range kept in dB to match bsp/audio.h */
static bool g_initialized = false;

/**
 * @brief Configure I2S peripheral for ES8311
 */
static esp_err_t bsp_i2s_init(const bsp_audio_config_t* config)
{
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(
        BSP_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_config.auto_clear = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_config, &g_i2s_tx_chan, NULL),
                        TAG, "Failed to create I2S channel");

    i2s_std_config_t std_config = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        config->bit_width, config->channel_format),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK_GPIO,
            .bclk = BSP_AUDIO_I2S_SCLK_GPIO,
            .ws   = BSP_AUDIO_I2S_LRCK_GPIO,
            .dout = BSP_AUDIO_I2S_DOUT_GPIO,
            .din  = BSP_AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_i2s_tx_chan, &std_config),
                        TAG, "Failed to initialize I2S standard mode");

    ESP_LOGI(TAG, "I2S initialized: %d Hz, %d-bit, %s",
             config->sample_rate, config->bit_width,
             config->channel_format == I2S_SLOT_MODE_STEREO ? "Stereo" : "Mono");
    return ESP_OK;
}

/**
 * @brief Initialize ES8311 codec via modern esp_codec_dev hierarchy
 */
static esp_err_t bsp_codec_init(void)
{
    /* Diagnostic: probe every 7-bit address on the shared bus once. */
    i2c_master_bus_handle_t bus = bsp_i2c_get_shared();
    ESP_LOGI(TAG, "I2C bus scan:");
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  device ACK at 0x%02X", addr);
        }
    }

    /* I2S data interface backed by the TX channel created by bsp_i2s_init(). */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = BSP_AUDIO_I2S_PORT,
        .tx_handle = g_i2s_tx_chan,
        .clk_src   = 0,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    /* I2C control interface on the shared software-I2C bus. */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = (uint8_t)BSP_AUDIO_I2C_PORT,
        .addr           = (uint8_t)BSP_AUDIO_CODEC_ADDR,
        .bus_handle     = (void*)bsp_i2c_get_shared(),
        .clock_speed_hz = (int)BSP_AUDIO_I2C_CLK_SPEED_HZ,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO interface");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin     = (int16_t)BSP_AUDIO_PA_CTRL_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk   = (BSP_AUDIO_I2S_MCLK_GPIO != GPIO_NUM_NC),
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
    };
    g_codec_if = es8311_codec_new(&es8311_cfg);
    if (g_codec_if == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = g_codec_if,
        .data_if  = data_if,
    };
    g_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (g_codec_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}

esp_err_t bsp_audio_init_cfg(const bsp_audio_config_t* config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Audio already initialized");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");

    ESP_LOGI(TAG, "Initializing audio");

    ESP_RETURN_ON_ERROR(bsp_i2s_init(config), TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(bsp_codec_init(),    TAG, "Codec init failed");

    /* Ensure the PA is driven - es8311_codec drives pa_pin already, but keep
     * a deterministic on-level here for boards wired without PA feedback. */
    if (BSP_AUDIO_PA_CTRL_GPIO != GPIO_NUM_NC) {
        gpio_config_t pa_config = {
            .mode          = GPIO_MODE_OUTPUT,
            .pin_bit_mask  = (1ULL << BSP_AUDIO_PA_CTRL_GPIO),
            .pull_up_en    = GPIO_PULLUP_DISABLE,
            .pull_down_en  = GPIO_PULLDOWN_DISABLE,
            .intr_type     = GPIO_INTR_DISABLE,
        };
        gpio_config(&pa_config);
        gpio_set_level(BSP_AUDIO_PA_CTRL_GPIO, 1);
    }

    bsp_audio_set_volume(config->volume);

    /* Open the codec output stream (required before esp_codec_dev_write). */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = config->sample_rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(g_codec_dev, &fs),
                        TAG, "Failed to open codec output stream");

    g_initialized = true;
    ESP_LOGI(TAG, "Audio initialization complete");
    return ESP_OK;
}

i2s_chan_handle_t bsp_audio_get_i2s_tx_chan(void) { return g_i2s_tx_chan; }
esp_codec_dev_handle_t bsp_audio_get_codec_dev_cfg(void) { return g_codec_dev; }

esp_err_t bsp_audio_set_volume(int volume)
{
    if (g_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume < -96) volume = -96;
    if (volume > 0)   volume = 0;

    /* Map -96..0 dB onto the 0..100 volume scale of esp_codec_dev. */
    int vol = (int)(((volume + 96) * 100) / 192 + 0.5f);
    int ret = esp_codec_dev_set_out_vol(g_codec_dev, vol);
    if (ret == ESP_CODEC_DEV_OK) {
        g_current_volume_db = volume;
        ESP_LOGI(TAG, "Volume set to %d dB (scale %d)", volume, vol);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Failed to set volume: %d", ret);
    return ESP_ERR_INVALID_STATE;
}

int bsp_audio_get_volume(void) { return g_current_volume_db; }

esp_err_t bsp_audio_test_tone(void)
{
    /* Bring-up helper: 1 s of 440 Hz sine on the codec output. */
    if (g_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    enum { SR = 44100, FREQ = 440, CHUNK_FRAMES = 1024, TOTAL_FRAMES = SR };
    static int16_t frames[CHUNK_FRAMES * 2];

    ESP_LOGI(TAG, "Playing 1 s test tone (%d Hz)", FREQ);
    for (int written = 0; written < TOTAL_FRAMES; written += CHUNK_FRAMES) {
        for (int i = 0; i < CHUNK_FRAMES; i++) {
            double t = (double)(written + i) / SR;
            int16_t s = (int16_t)(12000.0 * sin(2.0 * 3.14159265358979323846 * FREQ * t));
            frames[2 * i]     = s;   /* left  */
            frames[2 * i + 1] = s;   /* right */
        }
        int ret = esp_codec_dev_write(g_codec_dev, frames, sizeof(frames));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Test tone write failed: %d", ret);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Test tone done");
    return ESP_OK;
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    if (g_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_codec_dev_set_out_mute(g_codec_dev, mute);
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t bsp_audio_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Deinitializing audio");

    if (g_codec_dev != NULL) {
        if (g_initialized) {
            esp_codec_dev_close(g_codec_dev);
        }
        esp_codec_dev_delete(g_codec_dev);
        g_codec_dev = NULL;
    }
    if (g_codec_if != NULL) {
        /* interface refcount managed by esp_codec_dev internals */
        g_codec_if = NULL;
    }
    if (g_i2s_tx_chan != NULL) {
        i2s_del_channel(g_i2s_tx_chan);
        g_i2s_tx_chan = NULL;
    }
    if (BSP_AUDIO_PA_CTRL_GPIO != GPIO_NUM_NC) {
        gpio_set_level(BSP_AUDIO_PA_CTRL_GPIO, 0);
        gpio_reset_pin(BSP_AUDIO_PA_CTRL_GPIO);
    }
    g_initialized = false;
    return ESP_OK;
}
