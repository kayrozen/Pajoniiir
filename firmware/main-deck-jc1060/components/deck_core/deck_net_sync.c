#include "deck_net_sync.h"

#include <math.h>
#include <string.h>

static float wrap_to(float x, float span)
{
    /* Into [-span/2, span/2). */
    x = fmodf(x, span);
    if (x >= span * 0.5f) x -= span;
    if (x < -span * 0.5f) x += span;
    return x;
}

static float wrap_positive(float x, float span)
{
    x = fmodf(x, span);
    return x < 0.0f ? x + span : x;
}

void deck_net_sync_engage(deck_net_sync_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->engaged = true;
    s->need_snap = true;
    s->status = DECK_NET_SYNC_WAIT;
}

void deck_net_sync_disengage(deck_net_sync_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

bool deck_net_clock_bar_position(const deck_net_clock_t *clock, uint32_t now_ms,
                                 float *out_bar, bool *bar_known)
{
    if (!clock || !clock->valid || clock->period_us == 0u) {
        return false;
    }
    /* Signed: the anchor may be a few ms ahead of a now sampled earlier. */
    const float elapsed_us = (float)(int32_t)(now_ms - clock->anchor_ms) * 1000.0f;
    const float beats = elapsed_us / (float)clock->period_us;
    if (beats > DECK_NET_SYNC_STALE_BEATS || beats < -1.0f) {
        return false;
    }
    const bool known = clock->beat_in_bar >= 1u && clock->beat_in_bar <= 4u;
    const float start = known ? (float)(clock->beat_in_bar - 1u) : 0.0f;
    if (out_bar) *out_bar = wrap_positive(start + beats, 4.0f);
    if (bar_known) *bar_known = known;
    return true;
}

uint8_t deck_net_clock_beat_in_bar(const deck_net_clock_t *clock, uint32_t now_ms)
{
    float bar = 0.0f;
    bool known = false;
    if (!deck_net_clock_bar_position(clock, now_ms, &bar, &known) || !known) {
        return 0u;
    }
    uint8_t beat = (uint8_t)bar;
    return beat < 4u ? (uint8_t)(beat + 1u) : 1u;
}

float deck_net_clock_bpm(const deck_net_clock_t *clock)
{
    if (!clock || !clock->valid || clock->period_us == 0u) {
        return 0.0f;
    }
    return 60000000.0f / (float)clock->period_us;
}

bool deck_net_sync_local_position(const anlz_beat_t *beats, uint16_t beat_count,
                                  uint32_t position_ms, float *out_bar,
                                  bool *bar_known, float *beat_len_ms, float *bpm)
{
    if (!beats || beat_count < 2u) {
        return false;
    }
    /* Last beat at or before the playhead; 0 when before the first. */
    uint16_t lo = 0u, hi = beat_count;
    while ((uint16_t)(hi - lo) > 1u) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
        if (beats[mid].time_ms <= position_ms) lo = mid;
        else hi = mid;
    }
    const uint16_t i = lo;
    const uint16_t a = i + 1u < beat_count ? i : (uint16_t)(beat_count - 2u);
    if (beats[a + 1u].time_ms <= beats[a].time_ms) {
        return false;
    }
    const float len = (float)(beats[a + 1u].time_ms - beats[a].time_ms);
    const float frac = ((float)position_ms - (float)beats[i].time_ms) / len;
    const uint16_t phase = beats[i].beat_phase;   /* PQTZ beat number 1..4 */
    const bool known = phase >= 1u && phase <= 4u;
    const float start = known ? (float)(phase - 1u) : 0.0f;
    if (out_bar) *out_bar = wrap_positive(start + frac, 4.0f);
    if (bar_known) *bar_known = known;
    if (beat_len_ms) *beat_len_ms = len;
    if (bpm) *bpm = beats[i].bpm_x100 ? (float)beats[i].bpm_x100 / 100.0f : 60000.0f / len;
    return true;
}

static float clampf(float x, float lim)
{
    return x > lim ? lim : (x < -lim ? -lim : x);
}

void deck_net_sync_step(deck_net_sync_t *s, const deck_net_clock_t *clock,
                        const deck_net_sync_local_t *local, uint32_t now_ms,
                        deck_net_sync_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s || !s->engaged) {
        out->status = DECK_NET_SYNC_OFF;
        return;
    }
    const bool playing = local && local->playing;
    if (playing && !s->was_playing) {
        s->need_snap = true;     /* align the bar on PLAY, as a CDJ does */
    }
    s->was_playing = playing;

    float m_bar = 0.0f, l_bar = 0.0f, len_ms = 0.0f, l_bpm = 0.0f;
    bool m_known = false, l_known = false;
    const bool have_master = deck_net_clock_bar_position(clock, now_ms, &m_bar, &m_known);
    const bool have_local = local &&
        deck_net_sync_local_position(local->beats, local->beat_count, local->position_ms,
                                     &l_bar, &l_known, &len_ms, &l_bpm);
    if (!have_master || !have_local || l_bpm <= 0.0f) {
        /* Keep whatever tempo we had: a master that stops leaves us playing. */
        s->filt_valid = false;
        s->far = false;
        s->status = DECK_NET_SYNC_WAIT;
        out->status = s->status;
        return;
    }

    const float base = (deck_net_clock_bpm(clock) / l_bpm - 1.0f) * 100.0f;
    if (fabsf(base) > DECK_NET_SYNC_MAX_PERCENT) {
        s->status = DECK_NET_SYNC_WAIT;   /* out of reach: half/double tempo */
        out->status = s->status;
        return;
    }

    const bool bar = m_known && l_known;
    const float e_beat = wrap_to(m_bar - l_bar, 1.0f);
    const float e_bar = bar ? wrap_to(m_bar - l_bar, 4.0f) : e_beat;
    float trim = 0.0f;

    if (!playing || (local && local->hold)) {
        /* Tempo only: a paused or held deck has no phase to keep. */
        s->filt_valid = false;
        s->far = false;
        s->status = DECK_NET_SYNC_WAIT;
    } else if (s->settling && (int32_t)(now_ms - s->settle_until_ms) < 0) {
        s->status = DECK_NET_SYNC_ALIGNING;
    } else {
        s->settling = false;
        if (fabsf(e_bar) > DECK_NET_SYNC_FAR_BEATS) {
            if (!s->far) {
                s->far = true;
                s->far_since_ms = now_ms;
            }
        } else {
            s->far = false;
        }
        const bool far_due = s->far &&
            (uint32_t)(now_ms - s->far_since_ms) >= DECK_NET_SYNC_FAR_HOLD_MS &&
            (!s->snapped || (uint32_t)(now_ms - s->last_snap_ms) >= DECK_NET_SYNC_SNAP_GAP_MS);
        if (s->need_snap || far_due) {
            float target = (float)local->position_ms + e_bar * len_ms;
            if (target < 0.0f) target += 4.0f * len_ms;   /* next bar instead */
            out->seek = true;
            out->seek_ms = (uint32_t)(target + 0.5f);
            s->need_snap = false;
            s->far = false;
            s->filt_valid = false;
            s->snapped = true;
            s->last_snap_ms = now_ms;
            s->settling = true;
            s->settle_until_ms = now_ms + DECK_NET_SYNC_SETTLE_MS;
            s->status = DECK_NET_SYNC_ALIGNING;
        } else {
            s->err_filt = s->filt_valid
                ? s->err_filt + DECK_NET_SYNC_FILTER * (e_beat - s->err_filt)
                : e_beat;
            s->filt_valid = true;
            const float mag = fabsf(s->err_filt) - DECK_NET_SYNC_DEADBAND;
            if (mag > 0.0f) {
                trim = clampf(copysignf(mag, s->err_filt) * DECK_NET_SYNC_TRIM_GAIN,
                              DECK_NET_SYNC_TRIM_MAX);
            }
            s->status = fabsf(s->err_filt) < DECK_NET_SYNC_LOCK_BEATS
                ? DECK_NET_SYNC_LOCKED : DECK_NET_SYNC_ALIGNING;
            out->phase_error = s->err_filt;
        }
    }

    const float pitch = clampf(base + trim, DECK_NET_SYNC_MAX_PERCENT);
    if (!s->have_pitch || fabsf(pitch - s->pitch_percent) >= DECK_NET_SYNC_PITCH_EPSILON) {
        s->have_pitch = true;
        s->pitch_percent = pitch;
        out->set_pitch = true;
    }
    out->pitch_percent = s->pitch_percent;
    out->status = s->status;
}
