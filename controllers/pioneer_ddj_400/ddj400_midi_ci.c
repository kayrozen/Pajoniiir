/**
 * @file ddj400_midi_ci.c
 * @brief MIDI Controller Interface for Pioneer DDJ-400
 * 
 * Converts raw MIDI messages from DDJ-400 to Pajoniiir control_link events
 * 
 * Differences vs FLX4:
 * - 6 hot cues (A-F) instead of 8 (A-H)
 * - No LCD display messages
 * - Manual loop section (buttons vs touch pads)
 * - 13 Beat FX instead of 14
 * - No Smart CFX
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "control_link.h"
#include "ddj400_midi_ci.h"

static const char* TAG = "ddj400_midi";

// DDJ-400 specific constants
#define DDJ400_HOT_CUE_COUNT       6
#define DDJ400_HOT_CUE_BASE_DECK1  0x14
#define DDJ400_HOT_CUE_BASE_DECK2  0x1A
#define DDJ400_CHANNEL_DECK1       0x90
#define DDJ400_CHANNEL_DECK2       0x91
#define DDJ400_CHANNEL_GLOBAL      0xB0

// Transport notes (Deck 1)
#define DDJ400_NOTE_PLAY           0x0E
#define DDJ400_NOTE_CUE            0x0F
#define DDJ400_NOTE_SYNC           0x10
#define DDJ400_NOTE_SHIFT          0x11
#define DDJ400_NOTE_LOAD           0x0D

// Loop section
#define DDJ400_NOTE_LOOP_IN        0x30
#define DDJ400_NOTE_LOOP_OUT       0x31
#define DDJ400_NOTE_LOOP_SHIFT     0x32
#define DDJ400_NOTE_LOOP_RELOOP    0x33
#define DDJ400_NOTE_LOOP_HALF      0x34
#define DDJ400_NOTE_LOOP_DOUBLE    0x35

// Beat Jump
#define DDJ400_NOTE_JUMP_BASE      0x38

// Pad FX
#define DDJ400_NOTE_PAD_FX_BASE    0x40
#define DDJ400_NOTE_FX_ON_OFF      0x4A

// CC mappings
#define DDJ400_CC_JOG_ROTATE       0x00
#define DDJ400_CC_PITCH            0x08
#define DDJ400_CC_FILTER           0x56
#define DDJ400_CC_EQ_LOW           0x50
#define DDJ400_CC_EQ_MID           0x51
#define DDJ400_CC_EQ_HIGH          0x52
#define DDJ400_CC_TRIM             0x53
#define DDJ400_CC_FX_SELECT        0x5A
#define DDJ400_CC_FX_LEVEL         0x5B
#define DDJ400_CC_FX_BEAT          0x5C

// Global controls
#define DDJ400_CC_MASTER_VOL       0x20
#define DDJ400_CC_BOOTH_VOL        0x21
#define DDJ400_CC_CROSSFADER       0x22

typedef struct {
    uint8_t deck;
    uint8_t channel;
    bool shift_active;
    bool pad_fx_mode;
    uint8_t current_fx_id;
} ddj400_state_t;

static ddj400_state_t g_ddj400_state[2] = {
    {.deck = 0, .channel = 0x90, .shift_active = false, .pad_fx_mode = false},
    {.deck = 1, .channel = 0x91, .shift_active = false, .pad_fx_mode = false}
};

/**
 * @brief Convert MIDI note on to control_link event
 */
static esp_err_t ddj400_parse_note_on(uint8_t channel, uint8_t note, uint8_t velocity, 
                                       control_link_event_t* cl_event)
{
    if (velocity == 0) {
        // Note On with velocity 0 = Note Off
        return ddj400_parse_note_off(channel, note, 0x40, cl_event);
    }

    uint8_t deck_idx = (channel == DDJ400_CHANNEL_DECK2) ? 1 : 0;
    ddj400_state_t* state = &g_ddj400_state[deck_idx];

    // Transport
    if (note == DDJ400_NOTE_PLAY) {
        cl_event->type = CTRL_EVENT_PLAY;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_CUE) {
        cl_event->type = CTRL_EVENT_CUE;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_SYNC) {
        cl_event->type = CTRL_EVENT_SYNC;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_SHIFT) {
        state->shift_active = (velocity > 0);
        cl_event->type = CTRL_EVENT_SHIFT;
        cl_event->deck = state->deck;
        cl_event->value = state->shift_active ? 1 : 0;
        return ESP_OK;
    }

    // Hot Cues (6 only)
    if (note >= DDJ400_HOT_CUE_BASE_DECK1 && note < DDJ400_HOT_CUE_BASE_DECK1 + DDJ400_HOT_CUE_COUNT) {
        if (channel == DDJ400_CHANNEL_DECK1 || channel == DDJ400_CHANNEL_DECK2) {
            uint8_t cue_id = note - (deck_idx == 0 ? DDJ400_HOT_CUE_BASE_DECK1 : DDJ400_HOT_CUE_BASE_DECK2);
            
            if (state->pad_fx_mode) {
                // Pad FX mode
                cl_event->type = CTRL_EVENT_PAD_FX;
                cl_event->deck = state->deck;
                cl_event->param.pad_fx.cue_id = cue_id;
                cl_event->param.pad_fx.fx_id = state->current_fx_id;
                cl_event->param.pad_fx.active = 1;
            } else {
                // Normal hot cue
                cl_event->type = CTRL_EVENT_HOT_CUE;
                cl_event->deck = state->deck;
                cl_event->param.hot_cue.id = cue_id;
                cl_event->param.hot_cue.active = 1;
            }
            return ESP_OK;
        }
    }

    // Loop section
    if (note == DDJ400_NOTE_LOOP_IN) {
        cl_event->type = CTRL_EVENT_LOOP_IN;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_LOOP_OUT) {
        cl_event->type = CTRL_EVENT_LOOP_OUT;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_LOOP_SHIFT) {
        cl_event->type = CTRL_EVENT_LOOP_SHIFT;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_LOOP_RELOOP) {
        cl_event->type = CTRL_EVENT_LOOP_TOGGLE;
        cl_event->deck = state->deck;
        cl_event->value = 1;
        return ESP_OK;
    }

    // Beat Jump
    if (note >= DDJ400_NOTE_JUMP_BASE && note <= DDJ400_NOTE_JUMP_BASE + 7) {
        int8_t jump_amounts[] = {-1, 1, -1, 1, -2, 2, -4, 4}; // Quarter/beats
        int8_t beats = jump_amounts[note - DDJ400_NOTE_JUMP_BASE];
        
        if (state->shift_active) {
            beats /= 4; // Quarter beats with shift
        }
        
        cl_event->type = CTRL_EVENT_BEAT_JUMP;
        cl_event->deck = state->deck;
        cl_event->param.beat_jump.beats = beats;
        return ESP_OK;
    }

    // FX On/Off
    if (note == DDJ400_NOTE_FX_ON_OFF) {
        cl_event->type = CTRL_EVENT_FX_TOGGLE;
        cl_event->deck = state->deck;
        cl_event->param.fx.on = 1;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Convert MIDI note off to control_link event
 */
esp_err_t ddj400_parse_note_off(uint8_t channel, uint8_t note, uint8_t velocity, 
                                 control_link_event_t* cl_event)
{
    uint8_t deck_idx = (channel == DDJ400_CHANNEL_DECK2) ? 1 : 0;
    ddj400_state_t* state = &g_ddj400_state[deck_idx];

    // Transport release
    if (note == DDJ400_NOTE_PLAY) {
        cl_event->type = CTRL_EVENT_PLAY;
        cl_event->deck = state->deck;
        cl_event->value = 0;
        return ESP_OK;
    }
    
    if (note == DDJ400_NOTE_CUE) {
        cl_event->type = CTRL_EVENT_CUE;
        cl_event->deck = state->deck;
        cl_event->value = 0;
        return ESP_OK;
    }

    // Hot Cues release
    if (note >= DDJ400_HOT_CUE_BASE_DECK1 && note < DDJ400_HOT_CUE_BASE_DECK1 + DDJ400_HOT_CUE_COUNT) {
        if (channel == DDJ400_CHANNEL_DECK1 || channel == DDJ400_CHANNEL_DECK2) {
            uint8_t cue_id = note - (deck_idx == 0 ? DDJ400_HOT_CUE_BASE_DECK1 : DDJ400_HOT_CUE_BASE_DECK2);
            
            cl_event->type = CTRL_EVENT_HOT_CUE;
            cl_event->deck = state->deck;
            cl_event->param.hot_cue.id = cue_id;
            cl_event->param.hot_cue.active = 0;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Convert MIDI Control Change to control_link event
 */
static esp_err_t ddj400_parse_cc(uint8_t channel, uint8_t cc, uint8_t value, 
                                  control_link_event_t* cl_event)
{
    uint8_t deck_idx = (channel == DDJ400_CHANNEL_DECK2) ? 1 : 0;
    
    // Global controls (always channel 0xB0)
    if (channel == DDJ400_CHANNEL_GLOBAL) {
        if (cc == DDJ400_CC_MASTER_VOL) {
            cl_event->type = CTRL_EVENT_MASTER_VOLUME;
            cl_event->deck = 0xFF; // Global
            cl_event->param.volume.value = value; // 0-127
            return ESP_OK;
        }
        
        if (cc == DDJ400_CC_BOOTH_VOL) {
            cl_event->type = CTRL_EVENT_BOOTH_VOLUME;
            cl_event->deck = 0xFF;
            cl_event->param.volume.value = value;
            return ESP_OK;
        }
        
        if (cc == DDJ400_CC_CROSSFADER) {
            cl_event->type = CTRL_EVENT_CROSSFADER;
            cl_event->deck = 0xFF;
            cl_event->param.crossfader.position = value; // 0=left, 64=center, 127=right
            return ESP_OK;
        }
    }

    // Deck-specific controls
    ddj400_state_t* state = &g_ddj400_state[deck_idx];

    if (cc == DDJ400_CC_JOG_ROTATE) {
        // Jog wheel rotation (relative encoder)
        int16_t delta = (int16_t)value;
        if (delta > 64) {
            delta -= 128; // Negative rotation
        }
        
        cl_event->type = CTRL_EVENT_JOG_ROTATE;
        cl_event->deck = state->deck;
        cl_event->param.jog.delta = delta;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_PITCH) {
        // Pitch fader: 0=+10%, 64=0%, 127=-10%
        int16_t pitch_permille = (64 - (int16_t)value) * 1000 / 64;
        
        cl_event->type = CTRL_EVENT_PITCH;
        cl_event->deck = state->deck;
        cl_event->param.pitch.permille = pitch_permille;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_FILTER) {
        // Filter: 0=LPF max, 64=center, 127=HPF max
        int16_t filter_value = ((int16_t)value - 64) * 1000 / 64; // -1000 to +1000
        
        cl_event->type = CTRL_EVENT_FILTER;
        cl_event->deck = state->deck;
        cl_event->param.filter.value = filter_value;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_EQ_LOW) {
        cl_event->type = CTRL_EVENT_EQ_LOW;
        cl_event->deck = state->deck;
        cl_event->param.eq.value = value; // 0-127
        return ESP_OK;
    }

    if (cc == DDJ400_CC_EQ_MID) {
        cl_event->type = CTRL_EVENT_EQ_MID;
        cl_event->deck = state->deck;
        cl_event->param.eq.value = value;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_EQ_HIGH) {
        cl_event->type = CTRL_EVENT_EQ_HIGH;
        cl_event->deck = state->deck;
        cl_event->param.eq.value = value;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_TRIM) {
        cl_event->type = CTRL_EVENT_TRIM;
        cl_event->deck = state->deck;
        cl_event->param.trim.value = value;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_FX_SELECT) {
        // Cycle through 13 effects
        state->current_fx_id = value % 13;
        cl_event->type = CTRL_EVENT_FX_SELECT;
        cl_event->deck = state->deck;
        cl_event->param.fx.fx_id = state->current_fx_id;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_FX_LEVEL) {
        cl_event->type = CTRL_EVENT_FX_LEVEL;
        cl_event->deck = state->deck;
        cl_event->param.fx.level = value;
        return ESP_OK;
    }

    if (cc == DDJ400_CC_FX_BEAT) {
        // Beat divider: map to standard divisions
        uint8_t divisions[] = {1, 2, 4, 8, 16, 32};
        uint8_t div_idx = (value / 21) % 6;
        
        cl_event->type = CTRL_EVENT_FX_BEAT;
        cl_event->deck = state->deck;
        cl_event->param.fx.beat_division = divisions[div_idx];
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Main MIDI parser for DDJ-400
 */
esp_err_t ddj400_midi_parse(const uint8_t* midi_data, size_t len, 
                             control_link_event_t* cl_event)
{
    if (midi_data == NULL || cl_event == NULL || len < 3) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = midi_data[0];
    uint8_t data1 = midi_data[1];
    uint8_t data2 = midi_data[2];

    uint8_t msg_type = status & 0xF0;
    uint8_t channel = status & 0x0F;

    memset(cl_event, 0, sizeof(control_link_event_t));

    switch (msg_type) {
        case 0x90: // Note On
            return ddj400_parse_note_on(status, data1, data2, cl_event);
            
        case 0x80: // Note Off
            return ddj400_parse_note_off(status, data1, data2, cl_event);
            
        case 0xB0: // Control Change
            return ddj400_parse_cc(status, data1, data2, cl_event);
            
        default:
            ESP_LOGD(TAG, "Unhandled MIDI message: 0x%02X 0x%02X 0x%02X", 
                     status, data1, data2);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

/**
 * @brief Get controller info
 */
const controller_info_t* ddj400_get_info(void)
{
    static const controller_info_t info = {
        .name = "Pioneer DDJ-400",
        .vid = 0x0853,
        .pid = 0x0504,
        .hot_cue_count = DDJ400_HOT_CUE_COUNT,
        .has_lcd = false,
        .has_smart_cfx = false,
        .has_pad_fx = true,
        .has_manual_loop = true,
        .fx_count = 13
    };
    return &info;
}

/**
 * @brief Initialize DDJ-400 controller state
 */
esp_err_t ddj400_init(void)
{
    memset(g_ddj400_state, 0, sizeof(g_ddj400_state));
    g_ddj400_state[0].deck = 0;
    g_ddj400_state[0].channel = DDJ400_CHANNEL_DECK1;
    g_ddj400_state[1].deck = 1;
    g_ddj400_state[1].channel = DDJ400_CHANNEL_DECK2;
    
    ESP_LOGI(TAG, "DDJ-400 controller initialized");
    return ESP_OK;
}
