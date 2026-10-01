#pragma once
/* deck_core_dual's audio_engine fake plus the jc1060-only entry points. */
#include "../../deck_core_dual/stubs/audio_engine.h"

static inline bool audio_engine_deck_seek_busy(uint8_t deck) {
  (void)deck;
  return false;
}
static inline bool audio_engine_deck_censor_begin(uint8_t deck) {
  (void)deck;
  return false;
}
static inline void audio_engine_deck_censor_end(uint8_t deck) {
  (void)deck;
}
static inline uint32_t audio_engine_deck_track_length_ms(uint8_t deck) {
  (void)deck;
  return 0;
}
static inline uint32_t audio_engine_deck_position_ms_nowait(uint8_t deck) {
  return audio_engine_deck_position_ms(deck);
}
