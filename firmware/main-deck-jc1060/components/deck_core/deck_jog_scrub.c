#include "deck_jog_scrub.h"

static bool seek_allowed(const deck_jog_scrub_t *s, bool busy, uint32_t now_ms)
{
    return !busy && (!s->seeked || now_ms - s->last_seek_ms >= DECK_JOG_SCRUB_MIN_INTERVAL_MS);
}

static void note_seek(deck_jog_scrub_t *s, uint32_t now_ms)
{
    s->seeked = true;
    s->pending = false;
    s->last_seek_ms = now_ms;
}

deck_jog_scrub_action_t deck_jog_scrub_tick(deck_jog_scrub_t *s, bool touched, bool busy,
                                            uint32_t now_ms)
{
    if (!touched) {
        s->ignored++;
        return DECK_JOG_SCRUB_IGNORE;
    }
    if (!seek_allowed(s, busy, now_ms)) {
        s->pending = true;
        return DECK_JOG_SCRUB_MOVE;
    }
    note_seek(s, now_ms);
    return DECK_JOG_SCRUB_SEEK;
}

bool deck_jog_scrub_poll(deck_jog_scrub_t *s, bool busy, uint32_t now_ms)
{
    if (!s->pending || !seek_allowed(s, busy, now_ms)) {
        return false;
    }
    note_seek(s, now_ms);
    return true;
}

bool deck_jog_scrub_release(deck_jog_scrub_t *s)
{
    const bool flush = s->pending;
    s->seeked = false;
    s->pending = false;
    s->transport = false;
    return flush;
}

void deck_jog_transport_begin(deck_jog_scrub_t *s, uint32_t from_ms)
{
    s->transport = true;
    s->transport_ticks = 0;
    s->transport_base_ms = from_ms;
}

uint32_t deck_jog_transport_move(deck_jog_scrub_t *s, int16_t delta, uint32_t last_ms)
{
    s->transport_ticks += delta;
    int64_t target = (int64_t)s->transport_base_ms +
                     ((int64_t)s->transport_ticks * DECK_JOG_TRANSPORT_MS_NUM) /
                     DECK_JOG_TRANSPORT_MS_DEN;
    if (target <= 0 || target >= (int64_t)last_ms) {
        s->transport_base_ms = target <= 0 ? 0u : last_ms;
        s->transport_ticks = 0;
        return s->transport_base_ms;
    }
    return (uint32_t)target;
}

deck_jog_route_t deck_jog_route(bool cdj_mode, bool playing, bool touched,
                                bool scratch_active, bool scratch_stream, bool bend_stream)
{
    if (touched && scratch_active && scratch_stream) {
        return DECK_JOG_ROUTE_SCRATCH;
    }
    if (cdj_mode) {
        return playing ? DECK_JOG_ROUTE_BEND : DECK_JOG_ROUTE_SCRUB;
    }
    if (playing && (!touched || bend_stream)) {
        return DECK_JOG_ROUTE_BEND;
    }
    return DECK_JOG_ROUTE_SCRUB;
}

int32_t deck_jog_scrub_step_ms(bool cdj_mode, int16_t delta)
{
    if (!cdj_mode) {
        return (int32_t)delta * DECK_JOG_SCRUB_VINYL_STEP_MS;
    }
    int32_t ticks = delta;
    if (ticks > DECK_JOG_SCRUB_CDJ_MAX_TICKS) {
        ticks = DECK_JOG_SCRUB_CDJ_MAX_TICKS;
    } else if (ticks < -DECK_JOG_SCRUB_CDJ_MAX_TICKS) {
        ticks = -DECK_JOG_SCRUB_CDJ_MAX_TICKS;
    }
    return ticks * DECK_JOG_SCRUB_CDJ_STEP_MS;
}

deck_jog_show_t deck_jog_scrub_show(const deck_jog_scrub_t *s, bool cdj_mode)
{
    if (s->transport) {
        return DECK_JOG_SHOW_TRANSPORT;
    }
    return cdj_mode && s->pending ? DECK_JOG_SHOW_TARGET : DECK_JOG_SHOW_ENGINE;
}
