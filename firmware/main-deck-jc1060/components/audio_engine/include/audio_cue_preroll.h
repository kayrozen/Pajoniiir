#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Paused/CUE seek pre-roll. The decoder starts `frames` before the cue so a
 * backward scratch has history, then moves the canonical playhead onto the
 * cue frame. The audible start is always the cue: output must not start while
 * the pre-roll is pending, and the playhead must be published as soon as the
 * cue frame is written. While paused play_seq stays 0, so every pre-roll frame
 * counts against the forward cap (write - play):
 *  - v250: waiting for the end of a decode batch blocked the producer at the
 *    cap and PLAY heard the pre-roll;
 *  - v251: the producer needs a full batch of room to decode, so a cue frame
 *    in the last batch below the cap was never written and the gate stayed
 *    shut after PLAY. max_pre_frames must leave that batch free, and a stalled
 *    producer force-publishes whatever it has.
 */
typedef struct {
    uint32_t frames;    /* timeline seq of the cue frame (pre-roll length) */
    bool     pending;
} audio_cue_preroll_t;

/* Arms the pre-roll for a paused user seek to target_ms and returns the decode
 * start; frames never exceeds max_pre_frames. With arm=false (playing, loop,
 * unknown rate) it disarms and returns target_ms unchanged. */
uint32_t audio_cue_preroll_arm(audio_cue_preroll_t *p, bool arm, uint32_t target_ms,
                               uint32_t sample_rate, uint32_t max_pre_frames);

/* Producer check after every published frame. Returns true (with the
 * playhead to publish) once the cue frame is written, or with force (source
 * EOF, producer stalled) at whatever is there. The caller clears `pending`
 * after a successful publish. */
bool audio_cue_preroll_publish_point(const audio_cue_preroll_t *p, uint64_t write_seq,
                                     bool force, uint64_t *playhead);
