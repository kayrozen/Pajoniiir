// LOAD LOCK (v293): refuse a track load onto a deck that is playing.
//
// Settings switch, off by default (= upstream main-deck-p4: LOAD at any
// time). When it is on, a load reaches a deck only while that deck is paused
// or stopped; "playing" is what the operator hears, so a CDJ cue preview
// (CUE held on a paused deck) counts as playing too. deck_core owns the switch
// and the verdict (deck_core_load_allowed), the load layer applies it at its
// single chokepoint. Plain data, no RTOS.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECK_LOAD_LOCK_ALLOW_OFF,       /* switch off: load at any time */
    DECK_LOAD_LOCK_ALLOW_STOPPED,   /* switch on, deck paused or stopped */
    DECK_LOAD_LOCK_REFUSE_PLAYING,  /* switch on, deck playing */
} deck_load_lock_verdict_t;

deck_load_lock_verdict_t deck_load_lock_check(bool lock_on, bool playing);
bool deck_load_lock_allows(deck_load_lock_verdict_t verdict);
/* "off", "stopped", "playing" for logs. */
const char *deck_load_lock_verdict_name(deck_load_lock_verdict_t verdict);

#ifdef __cplusplus
}
#endif
