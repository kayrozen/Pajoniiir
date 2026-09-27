/* JC1060 hot cue recall transport (v261): a pad on a set slot jumps and
 * plays, CDJ / DDJ-400 style. v260 on hardware: pad 2 on a paused D1 at
 * 60220 / 60572 / 60897 ms only moved the playhead. */
#include "deck_hot_cue_recall.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    bool playing;
    bool preview;
    unsigned play_calls;
} deck_t;

/* Mirrors hot_cue_recall_play() in deck_core.c. */
static void recall(deck_t *d)
{
    switch (deck_hot_cue_recall_transport(d->playing, d->preview)) {
    case DECK_HOT_CUE_KEEP_PLAYING:
        return;
    case DECK_HOT_CUE_LATCH_PREVIEW:
        d->preview = false;
        return;
    case DECK_HOT_CUE_START_PLAY:
        d->play_calls++;
        d->playing = true;
        return;
    }
}

int main(void)
{
    /* Paused (parked on the load cue, or anywhere): one tap plays. */
    deck_t d = {0};
    recall(&d);
    assert(d.playing && d.play_calls == 1);

    /* Playing: further pads jump only, never a pause, never a second play. */
    recall(&d);
    recall(&d);
    assert(d.playing && d.play_calls == 1);

    /* CUE held (preview plays): the pad latches, the CUE release then keeps
     * playing instead of returning to the cue point. */
    deck_t p = { .playing = true, .preview = true };
    recall(&p);
    assert(p.playing && !p.preview && p.play_calls == 0);

    assert(deck_hot_cue_recall_transport(false, false) == DECK_HOT_CUE_START_PLAY);
    assert(deck_hot_cue_recall_transport(true, false) == DECK_HOT_CUE_KEEP_PLAYING);
    assert(deck_hot_cue_recall_transport(true, true) == DECK_HOT_CUE_LATCH_PREVIEW);
    assert(deck_hot_cue_recall_transport(false, true) == DECK_HOT_CUE_LATCH_PREVIEW);

    printf("deck_hot_cue_recall: all tests passed\n");
    return 0;
}
