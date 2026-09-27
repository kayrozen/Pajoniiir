#include "deck_cue_park.h"

#include <string.h>

void deck_cue_park_arm(deck_cue_park_t *p, uint16_t generation, uint32_t cue_ms, uint32_t now_ms)
{
    p->active = true;
    p->generation = generation;
    p->cue_ms = cue_ms;
    p->armed_ms = now_ms;
    p->attempts = 0u;
}

void deck_cue_park_cancel(deck_cue_park_t *p)
{
    memset(p, 0, sizeof(*p));
}

deck_cue_park_verdict_t deck_cue_park_check(deck_cue_park_t *p,
                                            bool loaded_valid,
                                            uint16_t loaded_generation,
                                            bool playing,
                                            uint32_t now_ms)
{
    if (!p->active) {
        return DECK_CUE_PARK_IDLE;
    }
    deck_cue_park_verdict_t verdict = DECK_CUE_PARK_TRY;
    if (!loaded_valid || loaded_generation != p->generation) {
        verdict = DECK_CUE_PARK_DROP_STALE;
    } else if (playing) {
        verdict = DECK_CUE_PARK_DROP_PLAYING;
    } else if ((uint32_t)(now_ms - p->armed_ms) >= DECK_CUE_PARK_TIMEOUT_MS) {
        verdict = DECK_CUE_PARK_DROP_TIMEOUT;
    }
    if (verdict != DECK_CUE_PARK_TRY) {
        p->active = false;
    }
    return verdict;
}

bool deck_cue_park_seek_result(deck_cue_park_t *p, deck_cue_park_seek_t result)
{
    p->attempts++;
    if (result == DECK_CUE_PARK_SEEK_NOT_READY) {
        return false;
    }
    p->active = false;
    return true;
}
