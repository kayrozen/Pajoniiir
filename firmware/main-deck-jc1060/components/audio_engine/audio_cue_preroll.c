#include "audio_cue_preroll.h"

uint32_t audio_cue_preroll_arm(audio_cue_preroll_t *p, bool arm, uint32_t target_ms,
                               uint32_t sample_rate, uint32_t max_pre_frames)
{
    if (!p) return target_ms;
    p->frames = 0u;
    p->pending = false;
    if (!arm || sample_rate == 0u || target_ms == 0u) return target_ms;
    /* Whole ms, rounded down, so frames <= max_pre_frames. */
    uint32_t max_pre_ms = (uint32_t)(((uint64_t)max_pre_frames * 1000u) / sample_rate);
    uint32_t pre_ms = target_ms < max_pre_ms ? target_ms : max_pre_ms;
    p->frames = (uint32_t)(((uint64_t)pre_ms * sample_rate) / 1000u);
    p->pending = p->frames > 0u;
    return target_ms - pre_ms;
}

bool audio_cue_preroll_publish_point(const audio_cue_preroll_t *p, uint64_t write_seq,
                                     bool force, uint64_t *playhead)
{
    if (!p || !p->pending || !playhead) return false;
    if (write_seq >= p->frames) {
        *playhead = p->frames;
        return true;
    }
    if (force) {
        /* Source EOF or stalled producer: start at the last written frame. */
        *playhead = write_seq;
        return true;
    }
    return false;
}
