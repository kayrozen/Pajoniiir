/* JC1060 paused/CUE pre-roll (v251 fix for "audio does not start at the cue",
 * v251b fix for "nothing plays after PLAY").
 *
 * Replays the decode task against the real canonical timeline: the 2 s
 * forward cap of deck_pcm_free(), the "decode only with a full batch of room"
 * check of the steady-state loop, the per-frame publish and the output start
 * gate of deck_output_active(). While the deck is paused nothing is consumed,
 * so the pre-roll must reach its cue frame without any output. */
#include "audio_cue_preroll.h"
#include "audio_pcm_timeline.h"
#include "audio_start_gate.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define FORWARD_MS  2000u
#define CAPACITY    196608u            /* 768 KiB / 4 bytes, as on firmware */
#define BATCH       1152               /* one MP3 frame */
#define MIN_ROOM    2304u              /* MINIMP3_MAX_SAMPLES_PER_FRAME */
#define PREBUFFER   4096u
#define OUT_FRAMES  256u               /* AE_OUT_FRAMES per output period */

static int16_t s_frames[CAPACITY * 2u];

typedef struct {
    audio_pcm_timeline_t t;
    audio_cue_preroll_t p;
    uint32_t rate;
    uint64_t decoded;                  /* source frames since the decode start */
    uint64_t source_len;               /* frames left in the file */
    bool safety_net;                   /* v251b: force-publish when stalled */
    bool playing, start_waiting;
    bool forced;                       /* published short of the cue */
    uint64_t playhead;                 /* published playhead */
} sim_t;

/* deck_pcm_forward_cap() / deck_pcm_free() with the timeline active. */
static uint32_t forward_cap(const sim_t *s)
{
    uint32_t target = (uint32_t)(((uint64_t)s->rate * FORWARD_MS) / 1000u);
    return target > s->t.capacity ? s->t.capacity : target;
}

static uint32_t forward_free(const sim_t *s)
{
    uint32_t future = audio_pcm_timeline_future_frames(&s->t);
    uint32_t cap = forward_cap(s);
    return future < cap ? cap - future : 0u;
}

/* Frame value = source position in frames since the decode start. */
static int16_t tag(uint64_t n) { return (int16_t)(n % 30000u); }

static void publish(sim_t *s, bool force)
{
    uint64_t ph;
    if (!s->p.pending ||
        !audio_cue_preroll_publish_point(&s->p, audio_pcm_timeline_write_seq(&s->t), force, &ph)) {
        return;
    }
    assert(audio_pcm_timeline_set_playhead(&s->t, ph));
    s->forced = ph < s->p.frames;
    s->playhead = ph;
    s->p.pending = false;
}

static void sim_seek(sim_t *s, uint32_t rate, uint32_t target_ms, uint32_t max_pre_frames,
                     bool safety_net)
{
    audio_pcm_timeline_init(&s->t, s_frames, CAPACITY);
    s->rate = rate;
    s->decoded = 0;
    s->source_len = 600ull * rate;
    s->safety_net = safety_net;
    s->playing = s->start_waiting = s->forced = false;
    (void)audio_cue_preroll_arm(&s->p, true, target_ms, rate, max_pre_frames);
}

/* v251 armed against the whole forward cap; the fix leaves one batch free. */
static uint32_t v251_max(uint32_t rate) { return (uint32_t)(((uint64_t)rate * FORWARD_MS) / 1000u); }
static uint32_t fixed_max(uint32_t rate) { return v251_max(rate) - MIN_ROOM; }

/* One pass of the decode loop. Returns false when the producer is stalled. */
static bool producer_step(sim_t *s)
{
    bool eof = s->decoded >= s->source_len;
    if (eof || forward_free(s) < MIN_ROOM) {
        if (s->safety_net && s->p.pending) publish(s, true);
        return false;
    }
    for (int i = 0; i < BATCH && s->decoded < s->source_len; i++) {
        if (forward_free(s) == 0u) {   /* blocks until output consumes */
            if (s->safety_net && s->p.pending) publish(s, true);
            if (forward_free(s) == 0u) return false;
        }
        assert(audio_pcm_timeline_push(&s->t, tag(s->decoded), tag(s->decoded)));
        s->decoded++;
        if (s->p.pending) publish(s, false);
    }
    return true;
}

/* audio_engine_play() + deck_output_active() start gate. */
static void sim_play(sim_t *s)
{
    s->playing = true;
    s->start_waiting = s->p.pending ||
        !audio_start_gate_ready(audio_pcm_timeline_future_frames(&s->t), PREBUFFER, false);
}

static bool output_open(sim_t *s)
{
    if (!s->playing) return false;
    if (s->start_waiting) {
        if (s->p.pending) return false;
        if (!audio_start_gate_ready(audio_pcm_timeline_future_frames(&s->t), PREBUFFER, false)) {
            return false;
        }
        s->start_waiting = false;
    }
    return true;
}

/* Runs producer and output for `periods` output periods. Returns the first
 * frame heard (source frame index) or -1 when the output never opened. */
static int64_t run(sim_t *s, uint32_t periods)
{
    int64_t first = -1;
    for (uint32_t k = 0; k < periods; k++) {
        (void)producer_step(s);
        if (!output_open(s)) continue;
        for (uint32_t i = 0; i < OUT_FRAMES; i++) {
            audio_mixer_frame_t f;
            if (!audio_pcm_timeline_pop(&s->t, &f)) break;
            if (first < 0) first = f.left;
        }
    }
    return first;
}

static sim_t s_sim;

static void test_arm(void)
{
    audio_cue_preroll_t p;
    const uint32_t max = fixed_max(48000u);          /* 93696 frames = 1952 ms */
    assert(audio_cue_preroll_arm(&p, true, 2939u, 48000u, max) == 987u);
    assert(p.pending && p.frames == 93696u && p.frames <= max);
    /* 44.1 kHz: whole ms rounding keeps frames under the cap. */
    assert(audio_cue_preroll_arm(&p, true, 5000u, 44100u, fixed_max(44100u)) == 3053u);
    assert(p.frames == 85862u && p.frames <= fixed_max(44100u));
    /* Cue closer to the start than the pre-roll: all history there is. */
    assert(audio_cue_preroll_arm(&p, true, 500u, 48000u, max) == 0u);
    assert(p.pending && p.frames == 24000u);
    /* Playing / loop / cue at 0 / unknown rate: plain seek, no pre-roll. */
    assert(audio_cue_preroll_arm(&p, false, 2939u, 48000u, max) == 2939u && !p.pending);
    assert(audio_cue_preroll_arm(&p, true, 0u, 48000u, max) == 0u && !p.pending);
    assert(audio_cue_preroll_arm(&p, true, 2939u, 0u, max) == 2939u && !p.pending);
}

/* v251 as flashed: pre-roll = whole cap. The paused producer stops one batch
 * short of the cue frame and the pre-roll never publishes. */
static void test_v251_stall_reproduced(void)
{
    const uint32_t rates[] = { 44100u, 48000u };
    for (unsigned r = 0; r < 2; r++) {
        sim_t *s = &s_sim;
        sim_seek(s, rates[r], 2569u, v251_max(rates[r]), false);
        assert(run(s, 2000u) < 0);
        assert(s->p.pending);
        assert(audio_pcm_timeline_write_seq(&s->t) < s->p.frames);
        assert(forward_free(s) < MIN_ROOM);
    }
}

/* The reported bug: PLAY lands while the pre-roll is pending, and with the
 * v251 arming the output gate never opens - nothing plays at all. */
static void test_v251_play_during_pending_never_plays(void)
{
    sim_t *s = &s_sim;
    sim_seek(s, 48000u, 2569u, v251_max(48000u), false);
    (void)run(s, 10u);                 /* PLAY right after the seek */
    assert(s->p.pending);
    sim_play(s);
    assert(run(s, 20000u) < 0);        /* ~100 s of output periods: silence */
    assert(s->p.pending);
}

/* Fixed: PLAY during the pending pre-roll starts exactly at the cue. */
static void test_play_during_pending_starts_at_cue(void)
{
    const uint32_t rates[] = { 44100u, 48000u };
    for (unsigned r = 0; r < 2; r++) {
        sim_t *s = &s_sim;
        sim_seek(s, rates[r], 2569u, fixed_max(rates[r]), false);
        (void)run(s, 10u);
        assert(s->p.pending);          /* still decoding the pre-roll */
        sim_play(s);
        int64_t first = run(s, 2000u);
        assert(!s->p.pending && !s->forced);
        assert(first == tag(s->p.frames));
    }
}

/* Fixed, paused long enough: cue published with full scratch history. */
static void test_cue_published_while_paused(void)
{
    sim_t *s = &s_sim;
    sim_seek(s, 48000u, 2939u, fixed_max(48000u), false);
    assert(run(s, 2000u) < 0);         /* paused: nothing heard */
    assert(!s->p.pending && !s->forced);
    assert(audio_pcm_timeline_play_seq(&s->t) == s->p.frames);
    assert(audio_pcm_timeline_history_frames(&s->t) == s->p.frames);
    sim_play(s);
    assert(run(s, 10u) == tag(s->p.frames));
}

/* Safety net alone (v251 arming): a stalled producer force-publishes, so
 * PLAY is never gated forever; the playhead is honest about the shortfall. */
static void test_safety_net_releases_stalled_preroll(void)
{
    sim_t *s = &s_sim;
    sim_seek(s, 48000u, 2569u, v251_max(48000u), true);
    (void)run(s, 10u);
    sim_play(s);
    int64_t first = run(s, 2000u);
    assert(!s->p.pending && s->forced);
    assert(first == tag(s->playhead));
    assert(s->p.frames - s->playhead < MIN_ROOM + BATCH);  /* < 72 ms early */
}

static void test_eof_short_of_cue(void)
{
    sim_t *s = &s_sim;
    sim_seek(s, 48000u, 2939u, fixed_max(48000u), true);
    s->source_len = 50000u;            /* file ends before the cue */
    (void)run(s, 2000u);
    assert(!s->p.pending && s->forced);
    assert(audio_pcm_timeline_play_seq(&s->t) == 50000u);
    uint64_t ph = 0;
    assert(!audio_cue_preroll_publish_point(&s->p, 50000u, true, &ph));
}

int main(void)
{
    test_arm();
    test_v251_stall_reproduced();
    test_v251_play_during_pending_never_plays();
    test_play_during_pending_starts_at_cue();
    test_cue_published_while_paused();
    test_safety_net_releases_stalled_preroll();
    test_eof_short_of_cue();
    puts("audio_cue_preroll tests passed");
    return 0;
}
