#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_performance_target.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CONTROLS_HOT_CUE_COUNT 8u
#define UI_CONTROLS_EMPTY_HOT_CUE_MS UINT32_MAX

typedef enum {
    UI_CONTROLS_HOT_CUE_SINGLE = 1,
    UI_CONTROLS_HOT_CUE_LOOP = 2,
} ui_controls_hot_cue_type_t;

typedef struct {
    bool active;
    uint32_t start_ms;
    uint32_t end_ms;
    int beats;
} ui_controls_loop_state_t;

typedef struct {
    bool empty;
    uint32_t position_ms;
    uint32_t end_ms;
    uint8_t type;
} ui_controls_hot_cue_t;

typedef struct {
    ui_performance_target_t performance_target;
    ui_controls_loop_state_t loop[UI_PERFORMANCE_TARGET_DECK_COUNT];
    ui_controls_hot_cue_t hot_cue[UI_CONTROLS_HOT_CUE_COUNT];
} ui_controls_state_t;

void ui_controls_state_init(ui_controls_state_t *state);

uint8_t ui_controls_active_deck(const ui_controls_state_t *state);
bool ui_controls_set_active_deck(ui_controls_state_t *state, uint8_t deck);

void ui_controls_set_loop_shadow(ui_controls_state_t *state,
                                 uint8_t deck,
                                 bool active,
                                 uint32_t start_ms,
                                 uint32_t end_ms,
                                 int beats);

#ifdef __cplusplus
}
#endif
