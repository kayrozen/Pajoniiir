/* v294: the DDJ-400 channel CUE (headphones) LED is host-driven. A PFL press
 * toggles the engine state and must publish the LED snapshot, otherwise the
 * diff publisher never sees the change and LED_PFL (note 0x54) stays dark.
 * Runs the jc1060 deck_core against the deck_core_dual stubs. */
#include "deck_core.h"
#include "control_link.h"
#include "audio_engine.h"
#include <assert.h>
#include <stdio.h>

int audio_engine_stub_channel_volume[DECK_CORE_DECK_COUNT];
int audio_engine_stub_pregain[DECK_CORE_DECK_COUNT];
int audio_engine_stub_master_volume;
int audio_engine_stub_headphone_mix;
int audio_engine_stub_headphone_level;
int audio_engine_stub_master_cue_toggle_count;
bool audio_engine_stub_master_cue_enabled;
int audio_engine_stub_crossfader;
int audio_engine_stub_pfl_toggle_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_eq_raw[DECK_CORE_DECK_COUNT][AUDIO_EQ_BAND_COUNT];
int audio_engine_stub_eq_set_count[DECK_CORE_DECK_COUNT][AUDIO_EQ_BAND_COUNT];
int audio_engine_stub_filter_raw[DECK_CORE_DECK_COUNT];
int audio_engine_stub_filter_set_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_beat_fx_filter_target;
int audio_engine_stub_beat_fx_filter_depth;
bool audio_engine_stub_beat_fx_filter_enabled;
int audio_engine_stub_beat_fx_filter_set_count;
int audio_engine_stub_beat_fx_echo_target;
int audio_engine_stub_beat_fx_echo_depth;
uint32_t audio_engine_stub_beat_fx_echo_delay_ms;
bool audio_engine_stub_beat_fx_echo_enabled;
int audio_engine_stub_beat_fx_echo_set_count;
int audio_engine_stub_beat_fx_delay_target;
int audio_engine_stub_beat_fx_delay_depth;
uint32_t audio_engine_stub_beat_fx_delay_delay_ms;
bool audio_engine_stub_beat_fx_delay_enabled;
int audio_engine_stub_beat_fx_delay_set_count;
int audio_engine_stub_beat_fx_flanger_target;
int audio_engine_stub_beat_fx_flanger_depth;
uint32_t audio_engine_stub_beat_fx_flanger_period_ms;
bool audio_engine_stub_beat_fx_flanger_enabled;
int audio_engine_stub_beat_fx_flanger_set_count;
int audio_engine_stub_pad_fx_deck;
int audio_engine_stub_pad_fx_mode;
int audio_engine_stub_pad_fx_pad;
bool audio_engine_stub_pad_fx_active;
int audio_engine_stub_pad_fx_set_count;
bool audio_engine_stub_smart_cfx_enabled;
bool audio_engine_stub_smart_fader_enabled;
esp_err_t audio_engine_stub_deck_play_result[DECK_CORE_DECK_COUNT];
bool audio_engine_stub_deck_playing[DECK_CORE_DECK_COUNT];
bool audio_engine_stub_deck_loaded[DECK_CORE_DECK_COUNT];
uint32_t audio_engine_stub_deck_position_ms[DECK_CORE_DECK_COUNT];
int audio_engine_stub_deck_seek_count[DECK_CORE_DECK_COUNT];
bool audio_engine_stub_loop_active[DECK_CORE_DECK_COUNT];
uint32_t audio_engine_stub_loop_start_ms[DECK_CORE_DECK_COUNT];
uint32_t audio_engine_stub_loop_end_ms[DECK_CORE_DECK_COUNT];
int audio_engine_stub_loop_set_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_loop_clear_count[DECK_CORE_DECK_COUNT];
float audio_engine_stub_pitch_percent[DECK_CORE_DECK_COUNT];
int audio_engine_stub_pitch_percent_set_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_jog_nudge_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_jog_nudge_last_delta[DECK_CORE_DECK_COUNT];
int audio_engine_stub_hold_set_count[DECK_CORE_DECK_COUNT];
bool audio_engine_stub_hold[DECK_CORE_DECK_COUNT];
int audio_engine_stub_scratch_begin_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_scratch_move_count[DECK_CORE_DECK_COUNT];
int audio_engine_stub_scratch_move_last_delta[DECK_CORE_DECK_COUNT];
int audio_engine_stub_scratch_end_count[DECK_CORE_DECK_COUNT];
bool audio_engine_stub_scratch_available[DECK_CORE_DECK_COUNT];
extern int control_link_stub_led_count;
extern led_id_t control_link_stub_led[128];
extern uint8_t control_link_stub_state[128];
extern uint8_t control_link_stub_deck[128];
void control_link_stub_reset_leds(void);
int control_link_stub_last_led_state(led_id_t led, uint8_t deck);

static int s_tests_run;

static ctrl_event_t mixer_button(uint8_t id, int16_t value)
{
    return (ctrl_event_t) {
        .type = CTRL_EV_BUTTON,
        .id = id,
        .value = value,
    };
}

static void reset_all(void)
{
    deck_core_test_reset();
    audio_engine_stub_pfl_toggle_count[CTRL_DECK_1] = 0;
    audio_engine_stub_pfl_toggle_count[CTRL_DECK_2] = 0;
    control_link_stub_reset_leds();
}

static void test_pfl_press_lights_led_on_its_deck(void)
{
    reset_all();
    ctrl_event_t press = mixer_button(CTRL_ID_DECK1_PFL, 1);
    deck_core_test_apply_event(&press);

    assert(audio_engine_stub_pfl_toggle_count[CTRL_DECK_1] == 1);
    assert(control_link_stub_last_led_state(LED_PFL, CTRL_DECK_1) == 1);
    assert(control_link_stub_last_led_state(LED_PFL, CTRL_DECK_2) != 1);
    s_tests_run++;
}

static void test_pfl_release_leaves_led_alone(void)
{
    reset_all();
    ctrl_event_t press = mixer_button(CTRL_ID_DECK2_PFL, 1);
    ctrl_event_t release = mixer_button(CTRL_ID_DECK2_PFL, 0);
    deck_core_test_apply_event(&press);
    assert(control_link_stub_last_led_state(LED_PFL, CTRL_DECK_2) == 1);

    control_link_stub_reset_leds();
    deck_core_test_apply_event(&release);
    assert(audio_engine_stub_pfl_toggle_count[CTRL_DECK_2] == 1);
    assert(control_link_stub_last_led_state(LED_PFL, CTRL_DECK_2) == -1);
    s_tests_run++;
}

static void test_second_pfl_press_turns_led_off(void)
{
    reset_all();
    ctrl_event_t press = mixer_button(CTRL_ID_DECK2_PFL, 1);
    deck_core_test_apply_event(&press);
    assert(control_link_stub_last_led_state(LED_PFL, CTRL_DECK_2) == 1);

    deck_core_test_apply_event(&press);
    assert(audio_engine_stub_pfl_toggle_count[CTRL_DECK_2] == 2);
    assert(control_link_stub_last_led_state(LED_PFL, CTRL_DECK_2) == 0);
    s_tests_run++;
}

int main(void)
{
    test_pfl_press_lights_led_on_its_deck();
    test_pfl_release_leaves_led_alone();
    test_second_pfl_press_turns_led_off();
    printf("deck_core_pfl_led_jc1060: %d tests passed\n", s_tests_run);
    return 0;
}
