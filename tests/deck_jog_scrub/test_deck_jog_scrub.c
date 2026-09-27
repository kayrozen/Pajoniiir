/* JC1060 jog scrub gate (v261). Replays the v260 trace: a paused deck parked
 * on its 8760 ms cue took one 3 ms seek per stray ring tick, 12 in 600 ms,
 * until PLAY started it at 8724 ms. */
#include "deck_jog_scrub.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    deck_jog_scrub_t gate;
    int32_t pos_ms;
    unsigned seeks;
    int32_t engine_ms;
} deck_t;

static void tick(deck_t *d, bool touched, int delta, uint32_t now_ms)
{
    deck_jog_scrub_action_t a = deck_jog_scrub_tick(&d->gate, touched, false, now_ms);
    if (a == DECK_JOG_SCRUB_IGNORE) return;
    d->pos_ms += delta * 3;
    if (a == DECK_JOG_SCRUB_SEEK) {
        d->seeks++;
        d->engine_ms = d->pos_ms;
    }
}

static bool release_flushes(deck_jog_scrub_t *g)
{
    return deck_jog_scrub_release(g);
}

static void release(deck_t *d)
{
    if (deck_jog_scrub_release(&d->gate)) {
        d->seeks++;
        d->engine_ms = d->pos_ms;
    }
}

/* v265 WDT: a paused platter rewound fast and continuously. Every seek
 * flushes the ring and restarts the cue pre-roll decode, busy for
 * ENGINE_SERVE_MS; the old gate still sent one seek per 50 ms, faster than the
 * engine served them, and the decoder never slept. Ticks every 2 ms (a hard
 * spin), deck task poll every DECK_JOG_SCRUB_POLL_MS as in deck_core. */
#define ENGINE_SERVE_MS 120u
typedef struct {
    deck_t d;
    uint32_t busy_until_ms;
    unsigned seeks_while_busy;
} engine_deck_t;

static bool engine_busy(const engine_deck_t *e, uint32_t now_ms)
{
    return (int32_t)(e->busy_until_ms - now_ms) > 0;
}

static void engine_seek(engine_deck_t *e, uint32_t now_ms)
{
    if (engine_busy(e, now_ms)) e->seeks_while_busy++;
    e->d.seeks++;
    e->d.engine_ms = e->d.pos_ms;
    e->busy_until_ms = now_ms + ENGINE_SERVE_MS;
}

static void fast_rewind_while_busy(void)
{
    const uint32_t t0 = 100000u;
    const uint32_t spin_ms = 10000u;
    engine_deck_t e = { .d = { .pos_ms = 240000, .engine_ms = 240000 } };
    for (uint32_t t = t0; t < t0 + spin_ms; t += 2u) {
        deck_jog_scrub_action_t a =
            deck_jog_scrub_tick(&e.d.gate, true, engine_busy(&e, t), t);
        assert(a != DECK_JOG_SCRUB_IGNORE);
        e.d.pos_ms -= 4 * 3;
        if (a == DECK_JOG_SCRUB_SEEK) engine_seek(&e, t);
        if ((t - t0) % DECK_JOG_SCRUB_POLL_MS == 0u &&
            deck_jog_scrub_poll(&e.d.gate, engine_busy(&e, t), t)) {
            engine_seek(&e, t);
        }
    }
    /* Coalesced: never a seek on top of an unserved one, so at most one per
     * serve time (not one per 50 ms, not one per tick), and the engine was
     * idle between them. */
    assert(e.seeks_while_busy == 0u);
    assert(e.d.seeks >= 2u && e.d.seeks <= spin_ms / ENGINE_SERVE_MS + 1u);
    assert(e.d.pos_ms == 240000 - (int32_t)(spin_ms / 2u) * 12);
    assert(e.d.engine_ms != e.d.pos_ms && e.d.gate.pending);

    /* Platter stopped but still held: the poll lands the last target once the
     * engine is free, exactly once. */
    const uint32_t stop = t0 + spin_ms;
    unsigned seeks = e.d.seeks;
    for (uint32_t t = stop; t < stop + 2u * ENGINE_SERVE_MS; t += DECK_JOG_SCRUB_POLL_MS) {
        if (deck_jog_scrub_poll(&e.d.gate, engine_busy(&e, t), t)) engine_seek(&e, t);
    }
    assert(e.d.seeks == seeks + 1u && e.d.engine_ms == e.d.pos_ms);
    assert(e.seeks_while_busy == 0u && !e.d.gate.pending);
    assert(!release_flushes(&e.d.gate));

    /* Released mid-rewind with the engine busy: the release still lands the
     * last target (the engine coalesces a queued seek into the newest one). */
    engine_deck_t r = { .d = { .pos_ms = 50000, .engine_ms = 50000 } };
    deck_jog_scrub_tick(&r.d.gate, true, false, 1000u);
    engine_seek(&r, 1000u);
    for (uint32_t t = 1002u; t < 1100u; t += 2u) {
        assert(deck_jog_scrub_tick(&r.d.gate, true, engine_busy(&r, t), t) ==
               DECK_JOG_SCRUB_MOVE);
        r.d.pos_ms -= 12;
    }
    assert(!deck_jog_scrub_poll(&r.d.gate, true, 1100u));
    release(&r.d);
    assert(r.d.seeks == 2u && r.d.engine_ms == r.d.pos_ms);

    /* Busy on the very first tick of a touch (CUE pre-roll still decoding):
     * move only, no seek until the engine is free. */
    deck_jog_scrub_t g = { 0 };
    assert(deck_jog_scrub_tick(&g, true, true, 5000u) == DECK_JOG_SCRUB_MOVE);
    assert(!deck_jog_scrub_poll(&g, true, 5010u));
    assert(deck_jog_scrub_poll(&g, false, 5020u));
    assert(!deck_jog_scrub_poll(&g, false, 5100u));   /* nothing pending */
    /* Untouched ticks never seek, busy or not. */
    assert(deck_jog_scrub_tick(&g, false, false, 5200u) == DECK_JOG_SCRUB_IGNORE);
}

/* v266: the release is the new pause position. Engine model closer to
 * audio_engine: a seek while one is queued replaces its target (the decoder
 * reads seek_target_ms once), the served target becomes the audio position. */
typedef struct {
    deck_jog_scrub_t gate;
    int32_t state_ms;           /* deck_core state->position_ms */
    int32_t audio_ms;           /* engine position once served */
    int32_t queued_ms;
    bool queued;
    uint32_t busy_until_ms;
} paused_deck_t;

static bool paused_busy(const paused_deck_t *p, uint32_t now_ms)
{
    return p->queued || (int32_t)(p->busy_until_ms - now_ms) > 0;
}

static void paused_seek(paused_deck_t *p)
{
    p->queued_ms = p->state_ms;
    p->queued = true;
}

static void paused_engine_run(paused_deck_t *p, uint32_t now_ms)
{
    if (p->queued && (int32_t)(p->busy_until_ms - now_ms) <= 0) {
        p->audio_ms = p->queued_ms;
        p->queued = false;
        p->busy_until_ms = now_ms + ENGINE_SERVE_MS;
    }
}

/* Pause at pause_ms, rewind fast by `ticks` ticks of -delta every 2 ms, hold
 * still for hold_ms, release, then let the engine drain. */
static paused_deck_t paused_rewind(int32_t pause_ms, unsigned ticks, int delta, uint32_t hold_ms)
{
    paused_deck_t p = { .state_ms = pause_ms, .audio_ms = pause_ms };
    uint32_t t = 200000u;
    for (unsigned i = 0; i < ticks; i++, t += 2u) {
        paused_engine_run(&p, t);
        deck_jog_scrub_action_t a =
            deck_jog_scrub_tick(&p.gate, true, paused_busy(&p, t), t);
        assert(a != DECK_JOG_SCRUB_IGNORE);
        p.state_ms -= delta * 3;
        if (p.state_ms < 0) p.state_ms = 0;
        if (a == DECK_JOG_SCRUB_SEEK) paused_seek(&p);
        if (i % (DECK_JOG_SCRUB_POLL_MS / 2u) == 0u &&
            deck_jog_scrub_poll(&p.gate, paused_busy(&p, t), t)) {
            paused_seek(&p);
        }
    }
    for (uint32_t end = t + hold_ms; t < end; t += DECK_JOG_SCRUB_POLL_MS) {
        paused_engine_run(&p, t);
        if (deck_jog_scrub_poll(&p.gate, paused_busy(&p, t), t)) paused_seek(&p);
    }
    if (deck_jog_scrub_release(&p.gate)) paused_seek(&p);
    for (uint32_t end = t + 4u * ENGINE_SERVE_MS; t < end; t += 2u) paused_engine_run(&p, t);
    return p;
}

static void paused_release_keeps_position(void)
{
    const int32_t pause_ms = 90000;
    /* Released mid-spin, engine still serving an earlier seek. */
    paused_deck_t a = paused_rewind(pause_ms, 1003u, 4, 0u);
    const int32_t x = pause_ms - 1003 * 12;
    assert(a.state_ms == x && a.audio_ms == x && x != pause_ms);
    assert(!a.queued && !a.gate.pending);

    /* Held still first: the poll already landed X, the release adds nothing. */
    paused_deck_t b = paused_rewind(pause_ms, 500u, 4, 300u);
    assert(b.state_ms == pause_ms - 6000 && b.audio_ms == b.state_ms);

    /* One tick: the first tick seeks at once, release lands on it. */
    paused_deck_t c = paused_rewind(pause_ms, 1u, 1, 0u);
    assert(c.state_ms == pause_ms - 3 && c.audio_ms == c.state_ms);

    /* Forward then clamped at 0: ends at X = 0, not at the pause position. */
    paused_deck_t d = paused_rewind(1000, 400u, 4, 0u);
    assert(d.state_ms == 0 && d.audio_ms == 0);

    /* A second gesture starts from the first release, never from the pause. */
    paused_deck_t e = paused_rewind(pause_ms, 200u, 4, 0u);
    paused_deck_t f = paused_rewind(e.state_ms, 200u, 4, 0u);
    assert(f.state_ms == pause_ms - 4800 && f.audio_ms == f.state_ms);
}

/* v267: jog routing. Stand-ins for CTRL_DECK_CTL_JOG_SCRATCH / _JOG_BEND; the
 * third stream is any other on_jog control. */
enum { STREAM_SCRATCH, STREAM_BEND, STREAM_OTHER };

static deck_jog_route_t route(bool cdj, bool playing, bool touched, bool scratch, int stream)
{
    return deck_jog_route(cdj, playing, touched, touched && scratch,
                          stream == STREAM_SCRATCH, stream == STREAM_BEND);
}

/* VINYL is the pre-v267 on_jog decision, for every input. */
static void vinyl_route_unchanged(void)
{
    for (int bits = 0; bits < 8; bits++) {
        const bool playing = bits & 1, touched = bits & 2, scratch = bits & 4;
        for (int stream = STREAM_SCRATCH; stream <= STREAM_OTHER; stream++) {
            deck_jog_route_t want;
            if (touched && scratch && stream == STREAM_SCRATCH) {
                want = DECK_JOG_ROUTE_SCRATCH;
            } else if (playing && (!touched || stream == STREAM_BEND)) {
                want = DECK_JOG_ROUTE_BEND;
            } else {
                want = DECK_JOG_ROUTE_SCRUB;
            }
            assert(route(false, playing, touched, scratch, stream) == want);
        }
    }
    assert(deck_jog_scrub_step_ms(false, 1) == 3);
    assert(deck_jog_scrub_step_ms(false, -40) == -120);   /* unbounded, 1:1 */
}

/* CDJ: the top touch is inert, the whole wheel bends a playing deck without a
 * touch, a paused deck moves by fine bounded steps through the seek gate. */
static void cdj_mode(void)
{
    for (int stream = STREAM_SCRATCH; stream <= STREAM_OTHER; stream++) {
        /* Bend without a touch, from the top as well as the side ring. */
        assert(route(true, true, false, false, stream) == DECK_JOG_ROUTE_BEND);
        /* A touch (never engages scratch in CDJ) changes nothing: still a
         * bend, where VINYL would scrub a held platter. */
        assert(route(true, true, true, false, stream) == DECK_JOG_ROUTE_BEND);
        /* Paused: fine scrub with or without a touch. */
        assert(route(true, false, false, false, stream) == DECK_JOG_ROUTE_SCRUB);
        assert(route(true, false, true, false, stream) == DECK_JOG_ROUTE_SCRUB);
    }
    /* A scratch latched in VINYL before the switch keeps its head until release. */
    assert(route(true, true, true, true, STREAM_SCRATCH) == DECK_JOG_ROUTE_SCRATCH);
    assert(route(true, true, true, true, STREAM_BEND) == DECK_JOG_ROUTE_BEND);

    /* Fine steps, bounded per event whatever the wheel speed. */
    assert(deck_jog_scrub_step_ms(true, 1) == DECK_JOG_SCRUB_CDJ_STEP_MS);
    assert(deck_jog_scrub_step_ms(true, -1) == -DECK_JOG_SCRUB_CDJ_STEP_MS);
    assert(deck_jog_scrub_step_ms(true, 0) == 0);
    const int32_t bound = DECK_JOG_SCRUB_CDJ_MAX_TICKS * DECK_JOG_SCRUB_CDJ_STEP_MS;
    assert(deck_jog_scrub_step_ms(true, 500) == bound);
    assert(deck_jog_scrub_step_ms(true, -500) == -bound);
    assert(bound < 3 * DECK_JOG_SCRUB_CDJ_MAX_TICKS);    /* finer than VINYL */

    /* Paused, untouched, a single nudge tick: the gate is engaged in CDJ
     * (on_jog passes touched || cdj), so it seeks at once by one fine step. */
    engine_deck_t n = { .d = { .pos_ms = 8760, .engine_ms = 8760 } };
    assert(deck_jog_scrub_tick(&n.d.gate, true, false, 1000u) == DECK_JOG_SCRUB_SEEK);
    n.d.pos_ms += deck_jog_scrub_step_ms(true, 1);
    engine_seek(&n, 1000u);
    assert(n.d.engine_ms == 8760 + DECK_JOG_SCRUB_CDJ_STEP_MS && n.d.gate.ignored == 0u);

    /* Paused, untouched, hard spin backwards (delta -20 every 2 ms): steps are
     * bounded, seeks coalesced by the v265 gate (never on an unserved one),
     * and the deck task poll lands the last target with no touch release. */
    const uint32_t t0 = 200000u;
    const uint32_t spin_ms = 3000u;
    engine_deck_t e = { .d = { .pos_ms = 120000, .engine_ms = 120000 } };
    int32_t prev_engine = e.d.engine_ms;
    int32_t max_jump = 0;
    for (uint32_t t = t0; t < t0 + spin_ms; t += 2u) {
        deck_jog_scrub_action_t a =
            deck_jog_scrub_tick(&e.d.gate, true, engine_busy(&e, t), t);
        assert(a != DECK_JOG_SCRUB_IGNORE);
        const int32_t step = deck_jog_scrub_step_ms(true, -20);
        assert(step == -bound);
        e.d.pos_ms += step;
        bool seek = a == DECK_JOG_SCRUB_SEEK;
        if (!seek && (t - t0) % DECK_JOG_SCRUB_POLL_MS == 0u) {
            seek = deck_jog_scrub_poll(&e.d.gate, engine_busy(&e, t), t);
        }
        if (seek) {
            engine_seek(&e, t);
            const int32_t jump = prev_engine - e.d.engine_ms;
            if (jump > max_jump) max_jump = jump;
            prev_engine = e.d.engine_ms;
        }
    }
    assert(e.seeks_while_busy == 0u);
    assert(e.d.seeks >= 2u && e.d.seeks <= spin_ms / ENGINE_SERVE_MS + 1u);
    assert(e.d.pos_ms == 120000 - (int32_t)(spin_ms / 2u) * bound);
    /* One seek never covers more than the events of one serve time. */
    assert(max_jump > 0 && max_jump <= (int32_t)(ENGINE_SERVE_MS / 2u + 1u) * bound);
    /* VINYL would have moved 1500 events * 20 ticks * 3 ms = 90 s. */
    assert(120000 - e.d.pos_ms == 12000);

    const uint32_t stop = t0 + spin_ms;
    for (uint32_t t = stop; t < stop + 2u * ENGINE_SERVE_MS; t += DECK_JOG_SCRUB_POLL_MS) {
        if (deck_jog_scrub_poll(&e.d.gate, engine_busy(&e, t), t)) engine_seek(&e, t);
    }
    assert(e.d.engine_ms == e.d.pos_ms && !e.d.gate.pending);
    assert(e.seeks_while_busy == 0u);
}

/* v267 long transport. A touch scratch plays from the 4 s window frozen at
 * touch-down; its head used to stop at the window edge. Model: the head moves
 * 125/24 ms per tick inside [oldest, newest]; a tick past the edge starts the
 * transport there (deck_core jog_scratch_edge_to_transport), later ticks move
 * the target, the gate sends it to the v266 engine model (a queued seek is
 * replaced by the newest, each one busy for ENGINE_SERVE_MS). */
#define WINDOW_BACK_MS 3000
#define WINDOW_AHEAD_MS 1000
#define TRACK_LAST_MS 299999u

typedef struct {
    paused_deck_t p;
    int32_t head_ms;            /* scratch head, until the transport starts */
    int32_t origin_ms;
    int32_t head_ticks;
    int32_t shown_ms;           /* what the waveform shows (deck_core_get_deck_state) */
    unsigned served;            /* seeks the engine served */
    int32_t last_served_ms;
    bool monotone;              /* served targets only ever moved one way */
} transport_deck_t;

static void transport_engine_run(transport_deck_t *d, uint32_t now_ms, int dir)
{
    const bool was_queued = d->p.queued;
    paused_engine_run(&d->p, now_ms);
    if (was_queued && !d->p.queued) {
        if (d->served && (d->p.audio_ms - d->last_served_ms) * dir < 0) d->monotone = false;
        d->served++;
        d->last_served_ms = d->p.audio_ms;
    }
}

/* events jog deltas of `delta` every 2 ms from a touch at start_ms, then
 * release and drain. */
static transport_deck_t transport(int32_t start_ms, unsigned events, int delta)
{
    transport_deck_t d = { .p = { .state_ms = start_ms, .audio_ms = start_ms },
                           .head_ms = start_ms, .origin_ms = start_ms,
                           .shown_ms = start_ms, .monotone = true };
    const int dir = delta > 0 ? 1 : -1;
    const int32_t oldest = start_ms - WINDOW_BACK_MS, newest = start_ms + WINDOW_AHEAD_MS;
    uint32_t t = 300000u;
    for (unsigned i = 0; i < events; i++, t += 2u) {
        transport_engine_run(&d, t, dir);
        if (!d.p.gate.transport) {
            const bool at_edge = (dir < 0 && d.head_ms == oldest) ||
                                 (dir > 0 && d.head_ms == newest);
            if (!at_edge) {
                /* Scratch: audible, no seek, the head is the position. */
                d.head_ticks += delta;
                int32_t h = d.origin_ms + d.head_ticks * DECK_JOG_TRANSPORT_MS_NUM /
                                          DECK_JOG_TRANSPORT_MS_DEN;
                d.head_ms = h < oldest ? oldest : h > newest ? newest : h;
                d.shown_ms = d.head_ms;
                continue;
            }
            deck_jog_transport_begin(&d.p.gate, (uint32_t)d.head_ms);
        }
        deck_jog_scrub_action_t a =
            deck_jog_scrub_tick(&d.p.gate, true, paused_busy(&d.p, t), t);
        assert(a != DECK_JOG_SCRUB_IGNORE);
        const int32_t before = d.p.state_ms;
        d.p.state_ms = (int32_t)deck_jog_transport_move(&d.p.gate, delta, TRACK_LAST_MS);
        /* The waveform follows the target on every tick, not the engine. */
        d.shown_ms = d.p.state_ms;
        assert(d.p.state_ms != before || d.p.state_ms == 0 ||
               d.p.state_ms == (int32_t)TRACK_LAST_MS);
        if (a == DECK_JOG_SCRUB_SEEK) paused_seek(&d.p);
        if (i % (DECK_JOG_SCRUB_POLL_MS / 2u) == 0u &&
            deck_jog_scrub_poll(&d.p.gate, paused_busy(&d.p, t), t)) {
            paused_seek(&d.p);
        }
    }
    assert(d.p.gate.transport);
    const int32_t final_ms = d.p.state_ms;
    if (deck_jog_scrub_release(&d.p.gate)) paused_seek(&d.p);
    assert(!d.p.gate.transport);
    for (uint32_t end = t + 4u * ENGINE_SERVE_MS; t < end; t += 2u) transport_engine_run(&d, t, dir);
    /* Audio restarts exactly where the platter was at release. */
    assert(d.p.state_ms == final_ms && d.p.audio_ms == final_ms && !d.p.queued);
    return d;
}

static void long_transport(void)
{
    /* Six seconds of fast rewind (-2 ticks / 2 ms): 6000 ticks, 31.25 s of
     * track, ten times the window. Edge after 3000 ms = 576 ticks. */
    transport_deck_t r = transport(150000, 3000u, -2);
    const int32_t edge_ms = 150000 - WINDOW_BACK_MS;
    const int32_t after_edge_ticks = -(6000 - 576);
    assert(r.p.audio_ms == edge_ms + after_edge_ticks * DECK_JOG_TRANSPORT_MS_NUM /
                                     DECK_JOG_TRANSPORT_MS_DEN);
    assert(r.p.audio_ms < edge_ms - 20000);
    /* The engine kept following the target the whole time: served seeks
     * all along, always further, one per serve time at most. */
    assert(r.monotone);
    assert(r.served >= (6000u - 1152u) / ENGINE_SERVE_MS);
    assert(r.served <= 6000u / ENGINE_SERVE_MS + 2u);
    assert(r.shown_ms == r.p.audio_ms);

    /* Forward throw to the end of the track: clamps at the last valid ms. */
    transport_deck_t f = transport(290000, 4000u, 3);
    assert(f.p.audio_ms == (int32_t)TRACK_LAST_MS && f.monotone);

    /* Backward past the start: clamps at 0. */
    transport_deck_t z = transport(4000, 2000u, -4);
    assert(z.p.audio_ms == 0);

    /* A reversal at a clamp moves off the end at once. */
    deck_jog_scrub_t g = { 0 };
    deck_jog_transport_begin(&g, 100u);
    assert(deck_jog_transport_move(&g, -500, TRACK_LAST_MS) == 0u);
    assert(deck_jog_transport_move(&g, 24, TRACK_LAST_MS) == 125u);
    deck_jog_transport_begin(&g, TRACK_LAST_MS - 10u);
    assert(deck_jog_transport_move(&g, 100, TRACK_LAST_MS) == TRACK_LAST_MS);
    assert(deck_jog_transport_move(&g, -24, TRACK_LAST_MS) == TRACK_LAST_MS - 125u);
    /* Exact count, no rounding drift: 24 single ticks = 125 ms. */
    deck_jog_transport_begin(&g, 50000u);
    uint32_t pos = 0;
    for (int i = 0; i < 24; i++) pos = deck_jog_transport_move(&g, -1, TRACK_LAST_MS);
    assert(pos == 50000u - 125u);
    assert(!deck_jog_scrub_release(&g) && !g.transport);
}

/* v268: CDJ paused nudge, sampled like the UI (one frame per 33 ms). The
 * engine is busy PREROLL_MS after each seek (paused seeks re-arm the cue
 * pre-roll), so its position moves in big, rare steps. What the deck shows
 * (deck_jog_scrub_show) must follow the target: monotone, on the target at
 * every frame, never a jump when the engine catches up. */
#define UI_FRAME_MS 33u
#define PREROLL_MS 400u

static int32_t shown_position(const engine_deck_t *e)
{
    return deck_jog_scrub_show(&e->d.gate, true) == DECK_JOG_SHOW_ENGINE
        ? e->d.engine_ms : e->d.pos_ms;
}

static void cdj_nudge_frames(int delta, uint32_t tick_ms, int32_t *max_frame_step,
                             int32_t *max_engine_step)
{
    const uint32_t t0 = 400000u, spin_ms = 3000u;
    engine_deck_t e = { .d = { .pos_ms = 60000, .engine_ms = 60000 } };
    int32_t last_shown = e.d.pos_ms, last_engine_frame = e.d.engine_ms;
    const int dir = delta > 0 ? 1 : -1;
    *max_frame_step = 0;
    *max_engine_step = 0;
    for (uint32_t t = t0; t < t0 + spin_ms + 2u * PREROLL_MS; t++) {
        const bool spinning = t < t0 + spin_ms;
        const bool busy = (int32_t)(e.busy_until_ms - t) > 0;
        if (spinning && (t - t0) % tick_ms == 0u) {
            deck_jog_scrub_action_t a = deck_jog_scrub_tick(&e.d.gate, true, busy, t);
            assert(a != DECK_JOG_SCRUB_IGNORE);
            e.d.pos_ms += deck_jog_scrub_step_ms(true, (int16_t)delta);
            if (a == DECK_JOG_SCRUB_SEEK) {
                engine_seek(&e, t);
                e.busy_until_ms = t + PREROLL_MS;
            }
        } else if ((t - t0) % DECK_JOG_SCRUB_POLL_MS == 0u &&
                   deck_jog_scrub_poll(&e.d.gate, busy, t)) {
            engine_seek(&e, t);
            e.busy_until_ms = t + PREROLL_MS;
        }
        if ((t - t0) % UI_FRAME_MS == 0u) {
            const int32_t shown = shown_position(&e);
            /* On the target at every frame, never behind or ahead of it. */
            assert(shown == e.d.pos_ms);
            const int32_t step = (shown - last_shown) * dir;
            assert(step >= 0);                                  /* monotone */
            if (step > *max_frame_step) *max_frame_step = step;
            const int32_t es = (e.d.engine_ms - last_engine_frame) * dir;
            if (es > *max_engine_step) *max_engine_step = es;
            last_shown = shown;
            last_engine_frame = e.d.engine_ms;
        }
    }
    /* Still coalesced: never a seek on an unserved one, and few of them. */
    assert(e.seeks_while_busy == 0u);
    assert(e.d.seeks <= (spin_ms + PREROLL_MS) / PREROLL_MS + 1u);
    /* Engine caught up: shows the engine again, same position, no jump. */
    assert(!e.d.gate.pending && e.d.engine_ms == e.d.pos_ms);
    assert(deck_jog_scrub_show(&e.d.gate, true) == DECK_JOG_SHOW_ENGINE);
    assert(shown_position(&e) == last_shown);
}

static void cdj_paused_smooth(void)
{
    int32_t frame_step, engine_step;
    /* Slow nudge, +1 every 8 ms: at most one frame's worth of ticks per
     * frame (ceil(33 / 8) = 5 ms), where the engine moved by ~50 ms. */
    cdj_nudge_frames(1, 8u, &frame_step, &engine_step);
    const int32_t eps = (int32_t)((UI_FRAME_MS + 7u) / 8u) * DECK_JOG_SCRUB_CDJ_STEP_MS;
    assert(frame_step <= eps);
    assert(engine_step >= 8 * eps);
    /* Fast backward spin, -20 every 2 ms (bounded to 8 ms per event). */
    cdj_nudge_frames(-20, 2u, &frame_step, &engine_step);
    const int32_t fast_eps = (int32_t)((UI_FRAME_MS + 1u) / 2u) *
                             DECK_JOG_SCRUB_CDJ_MAX_TICKS * DECK_JOG_SCRUB_CDJ_STEP_MS;
    assert(frame_step <= fast_eps);
    assert(engine_step > fast_eps);

    /* VINYL never shows the paused target (its scrub path is unchanged); a
     * transport always does. */
    deck_jog_scrub_t g = { .pending = true };
    assert(deck_jog_scrub_show(&g, false) == DECK_JOG_SHOW_ENGINE);
    assert(deck_jog_scrub_show(&g, true) == DECK_JOG_SHOW_TARGET);
    deck_jog_transport_begin(&g, 0u);
    assert(deck_jog_scrub_show(&g, false) == DECK_JOG_SHOW_TRANSPORT);
    (void)deck_jog_scrub_release(&g);
    assert(deck_jog_scrub_show(&g, true) == DECK_JOG_SHOW_ENGINE);
}

int main(void)
{
    /* v260 21418..22021 ms: untouched ring ticks on a paused deck */
    static const uint32_t stray_ms[] = { 21418, 21452, 21480, 21572, 21734, 21741,
                                         21762, 21797, 21903, 21934, 21964, 22021 };
    deck_t d = { .pos_ms = 8760, .engine_ms = 8760 };
    for (unsigned i = 0; i < sizeof stray_ms / sizeof stray_ms[0]; i++) tick(&d, false, -1, stray_ms[i]);
    assert(d.seeks == 0 && d.pos_ms == 8760 && d.engine_ms == 8760);
    assert(d.gate.ignored == 12);
    release(&d);                        /* no touch was pending */
    assert(d.seeks == 0);

    /* A held platter spun back 100 ticks over 1 s (one every 10 ms): at most
     * one seek per interval, and the release lands exactly where it stopped. */
    deck_t h = { .pos_ms = 8760, .engine_ms = 8760 };
    for (uint32_t t = 0; t < 1000; t += 10) tick(&h, true, -1, 30000 + t);
    assert(h.pos_ms == 8760 - 300);
    assert(h.seeks >= 1 && h.seeks <= 1000 / DECK_JOG_SCRUB_MIN_INTERVAL_MS + 1);
    assert(h.engine_ms != h.pos_ms);    /* last ticks inside the interval */
    release(&h);
    assert(h.engine_ms == h.pos_ms);
    unsigned seeks = h.seeks;
    release(&h);                        /* duplicate release: nothing more */
    assert(h.seeks == seeks);

    /* First tick of a new touch seeks at once, even right after the last one. */
    tick(&h, true, 1, 31000);
    assert(h.seeks == seeks + 1 && h.engine_ms == h.pos_ms);
    release(&h);
    assert(h.seeks == seeks + 1);

    /* Slow drag: every tick is past the interval, every tick seeks. */
    deck_t s = { .pos_ms = 1000 };
    for (uint32_t t = 0; t < 5; t++) tick(&s, true, 1, 50000 + t * DECK_JOG_SCRUB_MIN_INTERVAL_MS);
    assert(s.seeks == 5 && s.engine_ms == 1015);
    release(&s);
    assert(s.seeks == 5);

    /* Tick clock wrap. */
    deck_t w = { .pos_ms = 0 };
    tick(&w, true, 1, UINT32_MAX - 10u);
    tick(&w, true, 1, 5u);
    assert(w.seeks == 1);
    tick(&w, true, 1, UINT32_MAX - 10u + DECK_JOG_SCRUB_MIN_INTERVAL_MS);
    assert(w.seeks == 2);

    fast_rewind_while_busy();
    paused_release_keeps_position();
    vinyl_route_unchanged();
    cdj_mode();
    long_transport();
    cdj_paused_smooth();

    printf("deck_jog_scrub: all tests passed\n");
    return 0;
}
