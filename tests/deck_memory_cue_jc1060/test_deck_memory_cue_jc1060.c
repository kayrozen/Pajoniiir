/* v309: memory cues on the jc1060 deck: the analysis list merged with the
 * edits made on the deck, CUE/LOOP CALL prev / next (halve / double inside a
 * loop), MEMORY and DELETE (SHIFT + CALL), persistence across a reload and
 * the published view the UI draws. Runs the jc1060 deck_core against the
 * deck_core_dual stubs and an in-RAM memory cue store. */
#include "deck_core.h"
#include "control_link.h"
#include "audio_engine.h"
#include "hot_cue_store.h"
#include "rekordbox_anlz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
extern esp_err_t hot_cue_memory_stub_save_result;
extern int hot_cue_memory_stub_save_count;
void hot_cue_memory_stub_reset(void);

#define KEY 0xC0FFEE01u

static int s_tests_run;
static uint32_t s_generation = 1u;

static void ext(uint8_t deck, uint8_t action, bool pressed)
{
    ctrl_event_t ev = {
        .type = CTRL_EV_BUTTON,
        .id = deck == CTRL_DECK_1 ? CTRL_ID_DECK1_EXT_ACTION : CTRL_ID_DECK2_EXT_ACTION,
        .value = CTRL_DECK_EXT_VALUE(action, pressed),
    };
    deck_core_test_apply_event(&ev);
}

static void press(uint8_t deck, uint8_t action) { ext(deck, action, true); }

/* Analysis memory cues 1000, 5000 and the loop 9000-11000, given in file
 * order (the parser sorts; the deck takes them as they are). */
static void load(uint8_t deck, uint32_t key)
{
    anlz_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.has_cue_lists = true;
    meta.memory_cues[0] = (anlz_memory_cue_t) { .start_ms = 1000 };
    meta.memory_cues[1] = (anlz_memory_cue_t) { .start_ms = 5000 };
    meta.memory_cues[2] = (anlz_memory_cue_t) { .start_ms = 9000, .end_ms = 11000 };
    meta.memory_cue_count = 3;
    meta.has_memory_cue = true;
    meta.memory_cue_ms = 1000;
    audio_engine_stub_deck_loaded[deck] = true;
    assert(deck_core_publish_loaded_track(deck, s_generation, key, 120, 300000u, &meta) == ESP_OK);
}

static deck_core_memory_cues_t cues(uint8_t deck)
{
    deck_core_memory_cues_t out;
    assert(deck_core_get_memory_cues(deck, &out));
    assert(out.known);
    return out;
}

static void reset_all(void)
{
    deck_core_test_reset();
    hot_cue_memory_stub_reset();
    for (int d = 0; d < 2; d++) {
        audio_engine_stub_deck_position_ms[d] = 0;
        audio_engine_stub_deck_playing[d] = false;
        audio_engine_stub_deck_seek_count[d] = 0;
        audio_engine_stub_loop_active[d] = false;
        audio_engine_stub_loop_start_ms[d] = 0;
        audio_engine_stub_loop_end_ms[d] = 0;
        audio_engine_stub_loop_set_count[d] = 0;
    }
}

static void test_load_publishes_the_list(void)
{
    reset_all();
    deck_core_memory_cues_t none;
    assert(!deck_core_get_memory_cues(CTRL_DECK_1, &none) && !none.known);
    const uint32_t rev = deck_core_memory_cues_revision();
    load(CTRL_DECK_1, KEY);
    assert(deck_core_memory_cues_revision() != rev);
    deck_core_memory_cues_t c = cues(CTRL_DECK_1);
    assert(c.count == 3u);
    assert(c.cues[0].pos_ms == 1000u && !c.cues[0].local && c.cues[0].end_ms == 0u);
    assert(c.cues[2].pos_ms == 9000u && c.cues[2].end_ms == 11000u);
    /* The load cue is still the earliest memory cue (the deck parks on it). */
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 1000u);
    assert(!deck_core_get_memory_cues(CTRL_DECK_2, &none));
    s_tests_run++;
}

static void test_call_walks_the_cues(void)
{
    reset_all();
    load(CTRL_DECK_1, KEY);
    /* Parked on 1000: CALL < has nothing before it. */
    const int seeks = audio_engine_stub_deck_seek_count[CTRL_DECK_1];
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_PREV);
    assert(audio_engine_stub_deck_seek_count[CTRL_DECK_1] == seeks);

    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT);
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 5000u);
    assert(deck_core_get_deck_state(CTRL_DECK_1).cue_point_ms == 5000u);
    assert(!audio_engine_stub_loop_active[CTRL_DECK_1]);
    /* A release does nothing. */
    ext(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT, false);
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 5000u);

    /* The memory loop is set when called. */
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT);
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 9000u);
    assert(audio_engine_stub_loop_active[CTRL_DECK_1] &&
           audio_engine_stub_loop_start_ms[CTRL_DECK_1] == 9000u &&
           audio_engine_stub_loop_end_ms[CTRL_DECK_1] == 11000u);

    /* Inside an active loop CALL halves / doubles it, as before v309. */
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_PREV);
    assert(audio_engine_stub_loop_end_ms[CTRL_DECK_1] == 10000u);
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT);
    assert(audio_engine_stub_loop_end_ms[CTRL_DECK_1] == 11000u);
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 9000u);

    /* Loop off, playing just past 5000: CALL < goes back to 5000. */
    audio_engine_stub_loop_active[CTRL_DECK_1] = false;
    audio_engine_stub_deck_playing[CTRL_DECK_1] = true;
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 6200u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_PREV);
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 5000u);
    assert(audio_engine_stub_deck_playing[CTRL_DECK_1]);   /* keeps playing */
    /* Past the last cue CALL > does nothing. */
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 20000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT);
    assert(audio_engine_stub_deck_position_ms[CTRL_DECK_1] == 20000u);
    s_tests_run++;
}

static void test_memory_and_delete(void)
{
    reset_all();
    load(CTRL_DECK_1, KEY);
    /* MEMORY at the playhead: a local cue, saved. */
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 3000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_STORE);
    deck_core_memory_cues_t c = cues(CTRL_DECK_1);
    assert(c.count == 4u && c.cues[1].pos_ms == 3000u && c.cues[1].local);
    assert(hot_cue_memory_stub_save_count == 1);
    /* The same place again: nothing. */
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_STORE);
    assert(cues(CTRL_DECK_1).count == 4u && hot_cue_memory_stub_save_count == 1);

    /* MEMORY in an active loop stores the loop. */
    audio_engine_stub_loop_active[CTRL_DECK_1] = true;
    audio_engine_stub_loop_start_ms[CTRL_DECK_1] = 20000u;
    audio_engine_stub_loop_end_ms[CTRL_DECK_1] = 24000u;
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 21000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_STORE);
    c = cues(CTRL_DECK_1);
    assert(c.count == 5u && c.cues[4].pos_ms == 20000u && c.cues[4].end_ms == 24000u);
    audio_engine_stub_loop_active[CTRL_DECK_1] = false;

    /* DELETE needs a cue at the playhead. */
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 2000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_DELETE);
    assert(cues(CTRL_DECK_1).count == 5u);
    /* An analysis cue (1000) and a local one (3000, 20 ms off). */
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 1000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_DELETE);
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 3020u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_DELETE);
    c = cues(CTRL_DECK_1);
    assert(c.count == 3u && c.cues[0].pos_ms == 5000u && c.cues[2].pos_ms == 20000u);

    /* Reloaded, the edits come back from the store. */
    s_generation++;
    load(CTRL_DECK_1, KEY);
    c = cues(CTRL_DECK_1);
    assert(c.count == 3u && c.cues[0].pos_ms == 5000u && c.cues[1].pos_ms == 9000u);
    assert(c.cues[2].pos_ms == 20000u && c.cues[2].local);
    s_tests_run++;
}

static void test_both_decks_and_failures(void)
{
    reset_all();
    load(CTRL_DECK_1, KEY);
    load(CTRL_DECK_2, KEY);
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 7000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_STORE);
    /* The other deck on the same track follows, and edits from it keep both. */
    assert(cues(CTRL_DECK_2).count == 4u);
    audio_engine_stub_deck_position_ms[CTRL_DECK_2] = 5000u;
    press(CTRL_DECK_2, CTRL_DECK_EXT_ACTION_MEMORY_DELETE);
    assert(cues(CTRL_DECK_1).count == 3u && cues(CTRL_DECK_2).count == 3u);
    assert(cues(CTRL_DECK_1).cues[1].pos_ms == 7000u);

    /* A failed save leaves the list as it was. */
    hot_cue_memory_stub_save_result = ESP_FAIL;
    audio_engine_stub_deck_position_ms[CTRL_DECK_1] = 12000u;
    press(CTRL_DECK_1, CTRL_DECK_EXT_ACTION_MEMORY_STORE);
    assert(cues(CTRL_DECK_1).count == 3u);
    hot_cue_memory_stub_save_result = ESP_OK;

    /* An empty deck ignores the four actions. */
    reset_all();
    const int seeks = audio_engine_stub_deck_seek_count[CTRL_DECK_2];
    press(CTRL_DECK_2, CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT);
    press(CTRL_DECK_2, CTRL_DECK_EXT_ACTION_MEMORY_STORE);
    press(CTRL_DECK_2, CTRL_DECK_EXT_ACTION_MEMORY_DELETE);
    assert(audio_engine_stub_deck_seek_count[CTRL_DECK_2] == seeks);
    assert(hot_cue_memory_stub_save_count == 0);
    s_tests_run++;
}

int main(void)
{
    /* tools/controller_profile/compile_profile.py EXT_ACTIONS writes these
     * numbers into the DDJ-400 profile.s3bin. */
    assert(CTRL_DECK_EXT_ACTION_MEMORY_CALL_PREV == 7 && CTRL_DECK_EXT_ACTION_MEMORY_CALL_NEXT == 8);
    assert(CTRL_DECK_EXT_ACTION_MEMORY_STORE == 9 && CTRL_DECK_EXT_ACTION_MEMORY_DELETE == 10);
    test_load_publishes_the_list();
    test_call_walks_the_cues();
    test_memory_and_delete();
    test_both_decks_and_failures();
    printf("deck_memory_cue_jc1060: %d tests passed\n", s_tests_run);
    return 0;
}
