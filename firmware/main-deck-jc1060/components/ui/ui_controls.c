#include "ui_controls.h"

#include <string.h>

static uint8_t ui_controls_valid_deck(uint8_t deck)
{
    return deck < UI_PERFORMANCE_TARGET_DECK_COUNT ? deck : UI_PERFORMANCE_TARGET_DEFAULT_DECK;
}

void ui_controls_state_init(ui_controls_state_t *state)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    ui_performance_target_init(&state->performance_target);

    static const uint32_t default_positions[UI_CONTROLS_HOT_CUE_COUNT] = {
        0,
        15000,
        30000,
        45000,
        60000,
        90000,
        120000,
        150000,
    };
    for (uint8_t i = 0; i < UI_CONTROLS_HOT_CUE_COUNT; i++) {
        state->hot_cue[i] = (ui_controls_hot_cue_t){
            .empty = false,
            .position_ms = default_positions[i],
            .end_ms = 0,
            .type = UI_CONTROLS_HOT_CUE_SINGLE,
        };
    }
}

uint8_t ui_controls_active_deck(const ui_controls_state_t *state)
{
    return state ? ui_performance_target_get(&state->performance_target)
                 : UI_PERFORMANCE_TARGET_DEFAULT_DECK;
}

bool ui_controls_set_active_deck(ui_controls_state_t *state, uint8_t deck)
{
    if (!state || deck >= UI_PERFORMANCE_TARGET_DECK_COUNT) {
        return false;
    }

    uint8_t before = ui_performance_target_get(&state->performance_target);
    ui_performance_target_set(&state->performance_target, deck);
    return ui_performance_target_get(&state->performance_target) != before;
}

void ui_controls_set_loop_shadow(ui_controls_state_t *state,
                                 uint8_t deck,
                                 bool active,
                                 uint32_t start_ms,
                                 uint32_t end_ms,
                                 int beats)
{
    if (!state) {
        return;
    }

    uint8_t idx = ui_controls_valid_deck(deck);
    state->loop[idx] = (ui_controls_loop_state_t){
        .active = active,
        .start_ms = start_ms,
        .end_ms = end_ms,
        .beats = beats,
    };
}

#ifndef UI_CONTROLS_HOST_TEST

#include <stdint.h>

#include "deck_core.h"
#include "lvgl.h"
#include "ui_theme.h"


#endif
