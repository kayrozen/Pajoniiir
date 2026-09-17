/**
 * @file audio.h
 * @brief Audio interface for JC1060P470C_I_W_Y (ES8311)
 */

#pragma once

#include "esp_err.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio configuration structure
 */
typedef struct {
    uint32_t sample_rate;            ///< Sample rate in Hz
    i2s_data_bit_width_t bit_width;  ///< Data bit width
    i2s_slot_mode_t channel_format;  ///< Mono/Stereo
    int volume;                      ///< Initial volume (-96 to 0 dB)
} bsp_audio_config_t;

/**
 * @brief Initialize audio codec
 * 
 * @param config Audio configuration
 * @return ESP_OK on success
 */
esp_err_t bsp_audio_init(const bsp_audio_config_t* config);

/**
 * @brief Get I2S transmit channel
 * 
 * @return I2S channel handle
 */
i2s_chan_handle_t bsp_audio_get_i2s_tx_chan(void);

/**
 * @brief Get codec device
 * 
 * @return Codec device handle
 */
esp_codec_dev_handle_t bsp_audio_get_codec_dev(void);

/**
 * @brief Set audio volume
 * 
 * @param volume Volume in dB (-96 to 0)
 * @return ESP_OK on success
 */
esp_err_t bsp_audio_set_volume(int volume);

/**
 * @brief Get current volume
 * 
 * @return Volume in dB
 */
int bsp_audio_get_volume(void);

/**
 * @brief Mute/unmute audio
 * 
 * @param mute true to mute
 * @return ESP_OK on success
 */
esp_err_t bsp_audio_set_mute(bool mute);

/**
 * @brief Deinitialize audio
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_audio_deinit(void);

#ifdef __cplusplus
}
#endif
