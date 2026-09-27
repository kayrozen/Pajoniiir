#include "deck_hot_cue_recall.h"

deck_hot_cue_transport_t deck_hot_cue_recall_transport(bool playing, bool cue_preview)
{
    if (cue_preview) {
        return DECK_HOT_CUE_LATCH_PREVIEW;
    }
    return playing ? DECK_HOT_CUE_KEEP_PLAYING : DECK_HOT_CUE_START_PLAY;
}
