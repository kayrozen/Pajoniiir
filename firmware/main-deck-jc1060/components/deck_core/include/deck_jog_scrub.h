// Jog scrub seek gate (v261).
//
// Without a scratch engine the jog moves a paused or held deck by seeking,
// and every seek on a paused deck re-arms the ~2 s cue pre-roll decode. The
// DDJ-400 ring also sends stray +/-1 ticks while nobody holds the platter
// (at boot, and when pads on the same deck are hit), which used to walk a
// deck parked on its cue away from it one 3 ms seek at a time. The gate only
// scrubs while the platter top is touched, and seeks at most once per
// DECK_JOG_SCRUB_MIN_INTERVAL_MS; the ticks in between only move the target,
// the touch release seeks to where the platter stopped. Plain data, no RTOS:
// the deck task owns one per deck.
//
// v265: a fast rewind of a paused platter still sent 20 seeks/s, each one
// flushing the ring and restarting the ~2 s cue pre-roll decode, so the
// decoder never caught up nor slept and IDLE0 starved into the task watchdog.
// A seek now also waits until the engine has served the previous one (busy);
// the ticks in between only move the target, and deck_jog_scrub_poll() sends
// the latest target once the engine is free.
//
// v267: CDJ jog mode (docs/JOG_MODES_VINYL_VS_CDJ.md). The platter top touch
// is inert, the whole wheel bends a playing deck, and a paused deck moves by
// fine bounded steps through the same seek gate, engaged without a touch.
//
// v267 long transport: a VINYL touch scratch is bound to the 4 s PCM window
// the engine froze on touch, and the head stopped at its edge. Past the edge
// the touch becomes a transport: the ticks move a continuous target (same
// ms per tick as the scratch head, counted exactly), sought through the gate
// above (the engine jumps straight to each target, audio muted meanwhile),
// and the release lands on the last target.
//
// v268: a CDJ paused nudge showed the engine position, which only moves when
// a coalesced seek goes out (each one re-arms the cue pre-roll, so seldom):
// the waveform stepped. Until the target reaches the engine the deck shows
// the target, which moves on every tick; the engine position equals the
// target from the moment a seek is published, so the display never jumps.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_JOG_SCRUB_MIN_INTERVAL_MS 50u
/* Deck task wake-up while a touched deck holds a pending target. */
#define DECK_JOG_SCRUB_POLL_MS 10u
/* VINYL scrub: ms per jog tick. */
#define DECK_JOG_SCRUB_VINYL_STEP_MS 3
/* CDJ paused nudge: ms per jog tick, and at most this many ticks per event. */
#define DECK_JOG_SCRUB_CDJ_STEP_MS 1
#define DECK_JOG_SCRUB_CDJ_MAX_TICKS 8

typedef struct {
    bool transport;             /* scratch left its window: ticks move the target */
    int32_t transport_ticks;    /* ticks since transport_base_ms */
    uint32_t transport_base_ms;
    bool seeked;                /* a scrub seek went out during this touch */
    bool pending;               /* the target moved since the last scrub seek */
    uint32_t last_seek_ms;
    uint32_t ignored;           /* untouched ticks dropped since the last report */
} deck_jog_scrub_t;

typedef enum {
    DECK_JOG_SCRUB_IGNORE,      /* platter not touched: no position change */
    DECK_JOG_SCRUB_MOVE,        /* move the target, seek later */
    DECK_JOG_SCRUB_SEEK,        /* move the target and seek to it now */
} deck_jog_scrub_action_t;

typedef enum {
    DECK_JOG_ROUTE_SCRATCH,     /* audible scratch read head */
    DECK_JOG_ROUTE_BEND,        /* pitch-bend nudge of a playing deck */
    DECK_JOG_ROUTE_SCRUB,       /* position move through the seek gate */
} deck_jog_route_t;

/* Where one jog delta goes. scratch_active: a touch engaged the scratch head
 * (VINYL, or latched before a switch to CDJ, kept until its release).
 * scratch_stream / bend_stream: the event came from the platter top or the
 * side ring. */
deck_jog_route_t deck_jog_route(bool cdj_mode, bool playing, bool touched,
                                bool scratch_active, bool scratch_stream, bool bend_stream);
/* Position change in ms for one scrub delta. */
int32_t deck_jog_scrub_step_ms(bool cdj_mode, int16_t delta);

/* One jog delta on the scrub path (deck paused, or held without scratch).
 * busy: the engine has not served the previous seek yet. touched: the gate
 * is engaged (platter touched in VINYL, always in CDJ). */
deck_jog_scrub_action_t deck_jog_scrub_tick(deck_jog_scrub_t *s, bool touched, bool busy,
                                            uint32_t now_ms);
/* Deck task poll while the gate is engaged: true when the pending target
 * must be sought now. */
bool deck_jog_scrub_poll(deck_jog_scrub_t *s, bool busy, uint32_t now_ms);
/* Touch released: true when the last target never reached the engine. Ends
 * a transport. */
bool deck_jog_scrub_release(deck_jog_scrub_t *s);

/* What the deck shows (deck_core_get_deck_state). */
typedef enum {
    DECK_JOG_SHOW_ENGINE,       /* the engine position */
    DECK_JOG_SHOW_TARGET,       /* CDJ paused nudge: target not sought yet */
    DECK_JOG_SHOW_TRANSPORT,    /* jog transport: the target, even if the deck plays (held) */
} deck_jog_show_t;

deck_jog_show_t deck_jog_scrub_show(const deck_jog_scrub_t *s, bool cdj_mode);

/* Transport: ms per tick, the scratch head's 250 frames @ 48 kHz = 125/24 ms
 * (AUDIO_SCRATCH_DEFAULT_FRAMES_PER_TICK). */
#define DECK_JOG_TRANSPORT_MS_NUM 125
#define DECK_JOG_TRANSPORT_MS_DEN 24
/* The scratch head at from_ms reached its window edge. */
void deck_jog_transport_begin(deck_jog_scrub_t *s, uint32_t from_ms);
/* One transport delta: the new target, clamped to [0, last_ms]. At a clamp
 * the count restarts there, so reversing moves off the end at once. */
uint32_t deck_jog_transport_move(deck_jog_scrub_t *s, int16_t delta, uint32_t last_ms);

#ifdef __cplusplus
}
#endif
