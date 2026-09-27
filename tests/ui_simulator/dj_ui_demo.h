/* Demo data source for dj_ui (simulator only, never linked into firmware). */
#ifndef DJ_UI_DEMO_H
#define DJ_UI_DEMO_H

#include "dj_ui.h"

/* Fake data + 33 ms playback timer. Deterministic: driven by lv_tick only. */
void dj_ui_demo_start(void);

/* Demo transport state, for driver assertions. */
uint32_t dj_ui_demo_position(uint8_t deck);
bool dj_ui_demo_playing(uint8_t deck);

/* Deletes the demo timers so another data source can drive dj_ui. */
void dj_ui_demo_stop(void);

#endif
