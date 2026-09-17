/**
 * @file ddj400_midi_ci.h
 * @brief MIDI Controller Interface for Pioneer DDJ-400
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "control_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Controller information structure
 */
typedef struct {
    const char* name;
    uint16_t vid;
    uint16_t pid;
    uint8_t hot_cue_count;
    bool has_lcd;
    bool has_smart_cfx;
    bool has_pad_fx;
    bool has_manual_loop;
    uint8_t fx_count;
} controller_info_t;

/**
 * @brief Parse MIDI message from DDJ-400 to control_link event
 * 
 * @param midi_data Raw MIDI bytes (minimum 3 bytes)
 * @param len Length of MIDI data
 * @param cl_event Output control_link event
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for unknown messages
 */
esp_err_t ddj400_midi_parse(const uint8_t* midi_data, size_t len, 
                            control_link_event_t* cl_event);

/**
 * @brief Get DDJ-400 controller information
 * 
 * @return Pointer to static controller_info_t structure
 */
const controller_info_t* ddj400_get_info(void);

/**
 * @brief Initialize DDJ-400 controller state
 * 
 * @return ESP_OK on success
 */
esp_err_t ddj400_init(void);

/**
 * @brief Parse MIDI Note Off message
 * 
 * @param channel MIDI channel (0x80-0x8F)
 * @param note Note number
 * @param velocity Release velocity (typically 0x40)
 * @param cl_event Output control_link event
 * @return ESP_OK on success
 */
esp_err_t ddj400_parse_note_off(uint8_t channel, uint8_t note, uint8_t velocity,
                                control_link_event_t* cl_event);

#ifdef __cplusplus
}
#endif
