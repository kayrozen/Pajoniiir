#include "ui_position_interpolator.h"

static uint32_t clamp_to_duration(uint32_t position_ms, uint32_t duration_ms)
{
    if (duration_ms > 0 && position_ms > duration_ms) {
        return duration_ms;
    }
    return position_ms;
}

static uint32_t rebase(ui_position_interpolator_t *interp,
                       uint32_t snapshot_position_ms,
                       bool playing,
                       uint64_t now_us)
{
    interp->initialized = true;
    interp->last_playing = playing;
    interp->anchor_position_ms = snapshot_position_ms;
    interp->anchor_time_us = now_us;
    interp->display_us = (uint64_t)snapshot_position_ms * 1000u;
    return snapshot_position_ms;
}

void ui_position_interpolator_init(ui_position_interpolator_t *interp)
{
    if (!interp) {
        return;
    }

    interp->initialized = false;
    interp->last_playing = false;
    interp->anchor_position_ms = 0;
    interp->anchor_time_us = 0;
    interp->display_us = 0;
}

uint32_t ui_position_interpolator_update(ui_position_interpolator_t *interp,
                                         uint32_t snapshot_position_ms,
                                         uint32_t duration_ms,
                                         bool playing,
                                         uint32_t speed_permille,
                                         uint64_t now_us)
{
    if (!interp) {
        return clamp_to_duration(snapshot_position_ms, duration_ms);
    }

    snapshot_position_ms = clamp_to_duration(snapshot_position_ms, duration_ms);
    /* Zero is an explicit stationary/authoritative mode used while the audible
     * scratch head owns deck position. Return every snapshot exactly so a held
     * platter stays parked and jog motion is not filtered by the forward
     * prediction. Re-anchor here so normal playback resumes from the last
     * scratch position when the platter is released. */
    if (speed_permille == 0) {
        return rebase(interp, snapshot_position_ms, playing, now_us);
    }

    if (!interp->initialized || !playing || !interp->last_playing) {
        return rebase(interp, snapshot_position_ms, playing, now_us);
    }

    /* The engine position (base + frames played) only moves backwards on a
     * seek, cue, hot cue or loop wrap: follow those immediately. */
    if (snapshot_position_ms < interp->anchor_position_ms) {
        return rebase(interp, snapshot_position_ms, playing, now_us);
    }

    if (snapshot_position_ms != interp->anchor_position_ms) {
        interp->anchor_position_ms = snapshot_position_ms;
        interp->anchor_time_us = now_us;
    }

    /* v231: extrapolate from the newest engine position by at most
     * MAX_LEAD, and never step the display backwards. A newer engine
     * position ahead of the display is taken at once (no blend), so the
     * display is at most MAX_LEAD ahead and never behind the engine. */
    uint64_t elapsed_us = now_us >= interp->anchor_time_us
                        ? now_us - interp->anchor_time_us
                        : 0;
    uint64_t lead_us = (elapsed_us * speed_permille) / 1000u;
    if (lead_us > (uint64_t)UI_POSITION_INTERPOLATOR_MAX_LEAD_MS * 1000u) {
        lead_us = (uint64_t)UI_POSITION_INTERPOLATOR_MAX_LEAD_MS * 1000u;
    }
    uint64_t predicted_us = (uint64_t)snapshot_position_ms * 1000u + lead_us;
    if (duration_ms > 0 && predicted_us > (uint64_t)duration_ms * 1000u) {
        predicted_us = (uint64_t)duration_ms * 1000u;
    }
    if (predicted_us > interp->display_us) {
        interp->display_us = predicted_us;
    }

    interp->last_playing = playing;
    uint64_t display_ms = interp->display_us / 1000u;
    return display_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)display_ms;
}
