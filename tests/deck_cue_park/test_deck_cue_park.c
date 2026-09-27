/* JC1060 load cue park (v254): the load cue must reach the engine exactly
 * once per load, whether the engine's loader binds the file before or after
 * deck_core handles LOAD_CUE. Replays the v253 first-load race: LOAD_CUE at
 * 13 490 ms, engine source bound at 13 582 ms. */
#include "deck_cue_park.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    uint32_t ready_at_ms;       /* engine takes seeks from here on */
    uint32_t seeks_ok;
    uint32_t last_seek_ms;
    bool fail_hard;
} engine_t;

static deck_cue_park_seek_t engine_seek(engine_t *e, uint32_t now_ms, uint32_t cue_ms)
{
    if (e->fail_hard) return DECK_CUE_PARK_SEEK_FAILED;
    if (now_ms < e->ready_at_ms) return DECK_CUE_PARK_SEEK_NOT_READY;
    e->seeks_ok++;
    e->last_seek_ms = cue_ms;
    return DECK_CUE_PARK_SEEK_OK;
}

/* Deck task retry loop; returns the verdict that ended the park. */
static deck_cue_park_verdict_t run(deck_cue_park_t *p, engine_t *e, uint32_t *now_ms,
                                   uint16_t loaded_gen, const uint32_t *play_at_ms)
{
    for (int i = 0; i < 100000; i++) {
        bool playing = play_at_ms && *now_ms >= *play_at_ms;
        deck_cue_park_verdict_t v = deck_cue_park_check(p, true, loaded_gen, playing, *now_ms);
        if (v != DECK_CUE_PARK_TRY) return v;
        if (deck_cue_park_seek_result(p, engine_seek(e, *now_ms, p->cue_ms))) {
            return DECK_CUE_PARK_TRY;
        }
        *now_ms += DECK_CUE_PARK_RETRY_MS;
    }
    assert(0);
    return DECK_CUE_PARK_IDLE;
}

/* v253 bug: the first seek is refused while the loader binds the file; the
 * park retries and the cue lands exactly once, as soon as the engine is up. */
static void test_v253_first_load_race(void)
{
    engine_t e = { .ready_at_ms = 13582u };
    deck_cue_park_t p = {0};
    uint32_t now = 13490u;
    assert(engine_seek(&e, now, 2569u) == DECK_CUE_PARK_SEEK_NOT_READY);
    deck_cue_park_arm(&p, 7u, 2569u, now);
    assert(run(&p, &e, &now, 7u, NULL) == DECK_CUE_PARK_TRY);
    assert(e.seeks_ok == 1u && e.last_seek_ms == 2569u);
    assert(now >= 13582u && now < 13582u + DECK_CUE_PARK_RETRY_MS);
    assert(!p.active);
    /* Exactly once: nothing left to apply. */
    assert(deck_cue_park_check(&p, true, 7u, false, now + 1000u) == DECK_CUE_PARK_IDLE);
    assert(e.seeks_ok == 1u);
}

/* A newer load (or a clear) on the deck before the engine is ready drops the
 * old cue: it must never land on the next track. */
static void test_newer_load_drops_stale_cue(void)
{
    deck_cue_park_t p = {0};
    deck_cue_park_arm(&p, 7u, 2569u, 100u);
    assert(deck_cue_park_check(&p, true, 8u, false, 110u) == DECK_CUE_PARK_DROP_STALE);
    assert(!p.active);
    deck_cue_park_arm(&p, 7u, 2569u, 100u);
    assert(deck_cue_park_check(&p, false, 7u, false, 110u) == DECK_CUE_PARK_DROP_STALE);
    /* 16-bit wrap of the generation still compares exactly. */
    deck_cue_park_arm(&p, 0xFFFFu, 10u, 0u);
    assert(deck_cue_park_check(&p, true, 0x0000u, false, 5u) == DECK_CUE_PARK_DROP_STALE);
}

/* The two decks park independently: the same track on deck 2 neither
 * consumes nor re-applies deck 1's cue. */
static void test_decks_independent(void)
{
    engine_t e1 = { .ready_at_ms = 200u }, e2 = { .ready_at_ms = 0u };
    deck_cue_park_t p1 = {0}, p2 = {0};
    uint32_t t1 = 100u, t2 = 150u;
    deck_cue_park_arm(&p1, 3u, 2569u, t1);
    deck_cue_park_arm(&p2, 9u, 2569u, t2);
    assert(run(&p2, &e2, &t2, 9u, NULL) == DECK_CUE_PARK_TRY && e2.seeks_ok == 1u);
    assert(p1.active && e1.seeks_ok == 0u);
    assert(run(&p1, &e1, &t1, 3u, NULL) == DECK_CUE_PARK_TRY && e1.seeks_ok == 1u);
}

static void test_playing_drops(void)
{
    engine_t e = { .ready_at_ms = 1000u };
    deck_cue_park_t p = {0};
    uint32_t now = 0u;
    uint32_t play_at = 300u;
    deck_cue_park_arm(&p, 1u, 5000u, now);
    assert(run(&p, &e, &now, 1u, &play_at) == DECK_CUE_PARK_DROP_PLAYING);
    assert(e.seeks_ok == 0u && !p.active);
}

static void test_timeout_and_hard_failure(void)
{
    engine_t e = { .ready_at_ms = 0xFFFFFFFFu };
    deck_cue_park_t p = {0};
    uint32_t now = 0xFFFFFF00u;         /* tick wrap during the wait */
    deck_cue_park_arm(&p, 1u, 5000u, now);
    assert(run(&p, &e, &now, 1u, NULL) == DECK_CUE_PARK_DROP_TIMEOUT);
    assert(!p.active);

    engine_t bad = { .fail_hard = true };
    now = 0u;
    deck_cue_park_arm(&p, 1u, 5000u, now);
    assert(deck_cue_park_check(&p, true, 1u, false, now) == DECK_CUE_PARK_TRY);
    assert(deck_cue_park_seek_result(&p, engine_seek(&bad, now, 5000u)));
    assert(!p.active && p.attempts == 1u);
}

static void test_cancel(void)
{
    deck_cue_park_t p = {0};
    deck_cue_park_arm(&p, 1u, 5000u, 0u);
    deck_cue_park_cancel(&p);
    assert(deck_cue_park_check(&p, true, 1u, false, 10u) == DECK_CUE_PARK_IDLE);
}

int main(void)
{
    test_v253_first_load_race();
    test_newer_load_drops_stale_cue();
    test_decks_independent();
    test_playing_drops();
    test_timeout_and_hard_failure();
    test_cancel();
    puts("deck_cue_park tests passed");
    return 0;
}
