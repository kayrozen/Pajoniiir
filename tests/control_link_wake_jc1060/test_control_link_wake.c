/* v269: which controller events a screensaver wake may consume.
 * Presses and relative ticks are spent on the wake; releases, held modifiers
 * and absolute faders/knobs must always reach deck_core. */
#include "control_link.h"

#include <assert.h>
#include <stdio.h>

static ctrl_event_t ev(ctrl_event_type_t type, uint8_t id, int16_t value)
{
    ctrl_event_t e = {
        .type = type,
        .id = id,
        .value = value,
        .deck = control_link_id_deck(id),
        .control = control_link_id_control(id),
    };
    return e;
}

static bool consumable(ctrl_event_type_t type, uint8_t id, int16_t value)
{
    ctrl_event_t e = ev(type, id, value);
    return control_link_event_wake_consumable(&e);
}

static void test_presses_and_ticks_are_consumed(void)
{
    const uint8_t d1 = CTRL_NS_DECK1, d2 = CTRL_NS_DECK2;
    assert(consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_PLAY, 1));
    assert(consumable(CTRL_EV_BUTTON, d2 + CTRL_DECK_CTL_SYNC, 127));
    assert(consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_LOOP_IN, 1));
    assert(consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_PAD_ACTION,
                      CTRL_PAD_ACTION_VALUE(CTRL_PAD_MODE_HOT_CUE, 3, false, true)));
    assert(consumable(CTRL_EV_JOG, d1 + CTRL_DECK_CTL_JOG_BEND, 2));
    assert(consumable(CTRL_EV_JOG, d2 + CTRL_DECK_CTL_JOG_SCRATCH, -1));
    assert(consumable(CTRL_EV_BROWSE, CTRL_ID_BROWSE_DELTA, 1));
    assert(consumable(CTRL_EV_BUTTON, CTRL_ID_LOAD_DECK1, 1));
    assert(consumable(CTRL_EV_BUTTON, CTRL_ID_BROWSE_PRESS, 1));
    assert(consumable(CTRL_EV_BUTTON, CTRL_ID_BEAT_FX_ON, 1));
    assert(consumable(CTRL_EV_BUTTON, CTRL_ID_DECK1_PFL, 1));
}

static void test_releases_and_holds_pass_through(void)
{
    const uint8_t d1 = CTRL_NS_DECK1;
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_PLAY, 0));
    assert(!consumable(CTRL_EV_BUTTON, CTRL_ID_LOAD_DECK1, 0));
    /* A pad release carries a non-zero mode/pad encoding. */
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_PAD_ACTION,
                       CTRL_PAD_ACTION_VALUE(CTRL_PAD_MODE_SAMPLER, 7, true, false)));
    /* Holds: a consumed press would leave deck_core seeing only the release. */
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_CUE, 1));
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_SHIFT, 1));
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_JOG_TOUCH, 1));
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_JOG_SEARCH_TOUCH, 1));
    assert(!consumable(CTRL_EV_BUTTON, d1 + CTRL_DECK_CTL_EXT_ACTION, 0x80));
}

static void test_absolute_controls_pass_through(void)
{
    assert(!consumable(CTRL_EV_PITCH, CTRL_NS_DECK1 + CTRL_DECK_CTL_TEMPO, 8000));
    assert(!consumable(CTRL_EV_BUTTON, CTRL_ID_CROSSFADER, 64));
    assert(!consumable(CTRL_EV_BUTTON, CTRL_ID_CH1_VOLUME, 100));
    assert(!consumable(CTRL_EV_BUTTON, CTRL_ID_CH2_EQ_LOW, 30));
    assert(!consumable(CTRL_EV_BUTTON, CTRL_ID_MASTER_VOLUME, 90));
    assert(!consumable(CTRL_EV_BUTTON, CTRL_ID_BEAT_FX_DEPTH, 50));
    assert(!consumable(CTRL_EV_STATE, CTRL_ID_FLX4_CONNECTION, 1));
    assert(!control_link_event_wake_consumable(NULL));
}

int main(void)
{
    test_presses_and_ticks_are_consumed();
    test_releases_and_holds_pass_through();
    test_absolute_controls_pass_through();
    printf("control_link_wake_jc1060: all tests passed\n");
    return 0;
}
