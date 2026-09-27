// Load cue that the audio engine could not take yet (v254).
//
// A track load publishes the loaded track to deck_core before the engine's
// loader task has bound the file (audio_engine_deck_seek returns
// ESP_ERR_INVALID_STATE until "bounded compressed cache" is logged). The cue
// is parked here and retried by the deck task until the engine takes it, so
// every load parks on its cue exactly once, whatever finishes first. Plain
// data, no RTOS: the deck task owns one per deck.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_CUE_PARK_RETRY_MS   10u
#define DECK_CUE_PARK_TIMEOUT_MS 5000u

typedef struct {
    bool active;
    uint16_t generation;        /* loaded-track generation (low 16 bits) */
    uint32_t cue_ms;
    uint32_t armed_ms;
    uint32_t attempts;
} deck_cue_park_t;

typedef enum {
    DECK_CUE_PARK_IDLE,         /* nothing parked */
    DECK_CUE_PARK_TRY,          /* seek to cue_ms now */
    DECK_CUE_PARK_DROP_STALE,   /* another load / clear replaced the track */
    DECK_CUE_PARK_DROP_PLAYING, /* deck started: never jump a playing deck */
    DECK_CUE_PARK_DROP_TIMEOUT, /* engine never became ready */
} deck_cue_park_verdict_t;

typedef enum {
    DECK_CUE_PARK_SEEK_OK,
    DECK_CUE_PARK_SEEK_NOT_READY,   /* ESP_ERR_INVALID_STATE: try again */
    DECK_CUE_PARK_SEEK_FAILED,      /* anything else: give up */
} deck_cue_park_seek_t;

void deck_cue_park_arm(deck_cue_park_t *p, uint16_t generation, uint32_t cue_ms, uint32_t now_ms);
void deck_cue_park_cancel(deck_cue_park_t *p);
/* What to do now. Every verdict except TRY and IDLE disarms the park. */
deck_cue_park_verdict_t deck_cue_park_check(deck_cue_park_t *p,
                                            bool loaded_valid,
                                            uint16_t loaded_generation,
                                            bool playing,
                                            uint32_t now_ms);
/* Result of the seek a TRY asked for. Returns true when the park is done
 * (applied or given up), false when it stays armed for the next retry. */
bool deck_cue_park_seek_result(deck_cue_park_t *p, deck_cue_park_seek_t result);

#ifdef __cplusplus
}
#endif
