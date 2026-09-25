#pragma once

#include <stdint.h>

#include "deck_core.h"
#include "rekordbox_anlz.h"

/* v244: per-slot merge of the two hot cue sources shown by the UI.
 * For each slot A..H the local hot_cue_store value (pads, NVS; published by
 * deck_core_get_hot_cues()) wins; a slot the store does not hold falls back
 * to the Rekordbox ANLZ cue for that index; otherwise the slot is empty.
 *
 * `store` and `meta` may each be NULL. `out` receives the occupied slots in
 * slot order, one entry per slot at most; the return value is their count
 * (0..ANLZ_MAX_CUES). No allocation, safe from the LVGL task. */
uint8_t ui_hot_cue_view_merge(const deck_core_hot_cues_t *store,
                              const anlz_metadata_t *meta,
                              anlz_cue_t out[ANLZ_MAX_CUES]);
