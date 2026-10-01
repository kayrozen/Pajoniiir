/* JC1060 LOAD LOCK (v293): with the Settings switch on, a track load reaches
 * a deck only while it is paused or stopped; off keeps the upstream rule
 * (LOAD at any time). Replays the three places the firmware asks: submit,
 * the worker just before it resets the deck, and the LOAD button state. */
#include "deck_load_lock.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The load path: submit refuses, else the worker re-checks after resolving
 * the identity (PLAY may have been pressed meanwhile). true = deck reset. */
static bool load(bool lock_on, bool playing_at_submit, bool playing_at_worker)
{
    if (!deck_load_lock_allows(deck_load_lock_check(lock_on, playing_at_submit))) return false;
    if (!deck_load_lock_allows(deck_load_lock_check(lock_on, playing_at_worker))) return false;
    return true;
}

int main(void)
{
    /* Off: upstream behaviour, never refused. */
    assert(deck_load_lock_check(false, false) == DECK_LOAD_LOCK_ALLOW_OFF);
    assert(deck_load_lock_check(false, true) == DECK_LOAD_LOCK_ALLOW_OFF);
    assert(deck_load_lock_allows(DECK_LOAD_LOCK_ALLOW_OFF));

    /* On: paused / stopped loads, playing refuses. */
    assert(deck_load_lock_check(true, false) == DECK_LOAD_LOCK_ALLOW_STOPPED);
    assert(deck_load_lock_check(true, true) == DECK_LOAD_LOCK_REFUSE_PLAYING);
    assert(deck_load_lock_allows(DECK_LOAD_LOCK_ALLOW_STOPPED));
    assert(!deck_load_lock_allows(DECK_LOAD_LOCK_REFUSE_PLAYING));

    /* Log names. */
    assert(strcmp(deck_load_lock_verdict_name(DECK_LOAD_LOCK_ALLOW_OFF), "off") == 0);
    assert(strcmp(deck_load_lock_verdict_name(DECK_LOAD_LOCK_ALLOW_STOPPED), "stopped") == 0);
    assert(strcmp(deck_load_lock_verdict_name(DECK_LOAD_LOCK_REFUSE_PLAYING), "playing") == 0);
    assert(strcmp(deck_load_lock_verdict_name((deck_load_lock_verdict_t)99), "?") == 0);

    /* Load path. */
    assert(load(false, true, true));        /* off: a playing deck is replaced */
    assert(load(true, false, false));       /* on: paused deck loads */
    assert(!load(true, true, true));        /* on: refused at submit */
    assert(!load(true, false, true));       /* on: PLAY during resolve, refused
                                               before the deck reset */
    assert(!load(true, true, false));       /* refused at submit even if it
                                               pauses before the worker */

    /* Two decks: the lock is per target deck, the switch is global. */
    const bool playing[2] = {true, false};
    assert(!deck_load_lock_allows(deck_load_lock_check(true, playing[0])));
    assert(deck_load_lock_allows(deck_load_lock_check(true, playing[1])));

    printf("deck_load_lock: all tests passed\n");
    return 0;
}
