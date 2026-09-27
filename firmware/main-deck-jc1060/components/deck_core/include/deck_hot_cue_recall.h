// Hot cue recall transport (v261).
//
// A controller hot cue pad on a set slot jumps and plays, CDJ / DDJ-400 style:
// a paused deck starts from the cue, a playing deck keeps playing from it, and
// a CUE preview (CUE held) is latched so the CUE release no longer returns to
// the cue point. Only applied once the seek went through.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECK_HOT_CUE_KEEP_PLAYING,      /* already playing: the jump is enough */
    DECK_HOT_CUE_START_PLAY,        /* paused: start playback after the jump */
    DECK_HOT_CUE_LATCH_PREVIEW,     /* CUE preview: keep playing, end the preview */
} deck_hot_cue_transport_t;

deck_hot_cue_transport_t deck_hot_cue_recall_transport(bool playing, bool cue_preview);

#ifdef __cplusplus
}
#endif
