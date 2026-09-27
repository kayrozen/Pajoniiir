/* JC1060 audible position (v262): the waveform led the audio by the output
 * latency (UAC ring ~24 ms + URBs + device), the track end showed before it
 * was heard. The UI now anchors on the position mixed one latency ago. */
#include "ui_audible_position.h"
#include "ui_position_interpolator.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define LAT_US 36000u
#define FRAME_US 33000u

/* Engine position at t for a deck playing at 1x from p0 since t0, advanced
 * in 256-frame output blocks (5.333 ms) like the mixer. */
static uint32_t engine_pos(uint32_t p0_ms, uint64_t t0_us, uint64_t t_us)
{
    uint64_t blocks = (t_us - t0_us) * 48000u / 1000000u / 256u;
    return p0_ms + (uint32_t)(blocks * 256u * 1000u / 48000u);
}

static void test_zero_latency_is_passthrough(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    assert(ui_audible_position_update(&ap, 1000, 1000000, 0) == 1000);
    assert(ui_audible_position_update(&ap, 5000, 1033000, 0) == 5000);
    assert(ui_audible_position_update(&ap, 10, 1066000, 0) == 10);
}

static void test_steady_play_lags_by_latency(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    uint32_t last = 0;
    for (unsigned i = 0; i < 200; i++) {
        uint64_t t = 1000000u + (uint64_t)i * FRAME_US;
        uint32_t engine = engine_pos(20000, 1000000u, t);
        uint32_t audible = ui_audible_position_update(&ap, engine, t, LAT_US);
        assert(audible >= last);
        assert(audible <= engine);
        if (i >= 3) {
            uint32_t expect = engine_pos(20000, 1000000u, t - LAT_US);
            assert(abs((int)audible - (int)expect) <= 6);
            assert(engine - audible >= 30 && engine - audible <= 42);
        }
        last = audible;
    }
}

static void test_play_start_holds_then_moves(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    /* Paused at 42000, then play at t=2 s: nothing audible moves for one
     * latency, and the display never steps below the pause point. */
    uint64_t t = 1000000u;
    for (; t < 2000000u; t += FRAME_US) {
        assert(ui_audible_position_update(&ap, 42000, t, LAT_US) == 42000);
    }
    uint64_t play_us = t;
    uint32_t last = 42000;
    for (unsigned i = 0; i < 20; i++, t += FRAME_US) {
        uint32_t audible = ui_audible_position_update(&ap, engine_pos(42000, play_us, t), t, LAT_US);
        assert(audible >= last);
        if (t < play_us + LAT_US) {
            assert(audible == 42000);
        }
        last = audible;
    }
    assert(last > 42500);
}

static void test_fresh_history_returns_oldest(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    /* First sample after boot or a load: nothing older to replay. */
    assert(ui_audible_position_update(&ap, 0, 5000, LAT_US) == 0);
    assert(ui_audible_position_update(&ap, 7, 12000, LAT_US) == 0);
}

static void test_seek_is_held_not_blended(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    uint64_t t = 1000000u;
    for (unsigned i = 0; i < 10; i++, t += FRAME_US) {
        (void)ui_audible_position_update(&ap, engine_pos(60000, 1000000u, t), t, LAT_US);
    }
    uint32_t before = engine_pos(60000, 1000000u, t - FRAME_US);
    /* Hot cue / cue return to 10000 while playing: the old track keeps
     * sounding for one latency, never a position in between. */
    uint64_t seek_us = t;
    for (unsigned i = 0; i < 6; i++, t += FRAME_US) {
        uint32_t audible = ui_audible_position_update(&ap, engine_pos(10000, seek_us, t), t, LAT_US);
        assert(audible <= before || audible >= 10000);
        assert(!(audible > 10200 && audible < 59000));
        if (t >= seek_us + LAT_US) {
            assert(audible >= 10000 && audible < 10200);
        } else {
            assert(audible > 59000);
        }
    }
}

static void test_seek_to_zero_never_negative(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    uint64_t t = 1000000u;
    for (unsigned i = 0; i < 10; i++, t += FRAME_US) {
        (void)ui_audible_position_update(&ap, 800, t, LAT_US);
    }
    for (unsigned i = 0; i < 10; i++, t += FRAME_US) {
        uint32_t audible = ui_audible_position_update(&ap, 0, t, LAT_US);
        assert(audible == 800 || audible == 0);
    }
    assert(ui_audible_position_update(&ap, 0, t, LAT_US) == 0);
}

static void test_scratch_motion_is_replayed(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    /* Reverse scratch at -2x: 66 ms of track per 33 ms frame. */
    uint64_t t = 1000000u;
    uint32_t pos = 30000;
    uint32_t last = UINT32_MAX;
    for (unsigned i = 0; i < 12; i++, t += FRAME_US, pos -= 66) {
        uint32_t audible = ui_audible_position_update(&ap, pos, t, LAT_US);
        assert(audible <= last);
        if (i >= 2) {
            /* 36 ms back at -2x = 72 ms ahead of the head. */
            assert(abs((int)audible - (int)(pos + 72)) <= 2);
        }
        last = audible;
    }
}

static void test_pause_settles_on_engine(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    uint64_t t = 1000000u;
    for (unsigned i = 0; i < 10; i++, t += FRAME_US) {
        (void)ui_audible_position_update(&ap, engine_pos(5000, 1000000u, t), t, LAT_US);
    }
    uint32_t frozen = engine_pos(5000, 1000000u, t);
    uint32_t last = 0;
    for (unsigned i = 0; i < 5; i++, t += FRAME_US) {
        uint32_t audible = ui_audible_position_update(&ap, frozen, t, LAT_US);
        assert(audible >= last && audible <= frozen);
        last = audible;
    }
    assert(last == frozen);
}

static void test_same_tick_keeps_newest(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    (void)ui_audible_position_update(&ap, 100, 1000000u, 0);
    (void)ui_audible_position_update(&ap, 200, 1000000u, 0);
    assert(ap.count == 1);
    assert(ui_audible_position_update(&ap, 300, 1100000u, 50000u) == 250);
}

static void test_ring_wraps(void)
{
    ui_audible_position_t ap;
    ui_audible_position_init(&ap);
    uint64_t t = 1000000u;
    for (unsigned i = 0; i < 5u * UI_AUDIBLE_POSITION_SAMPLES; i++, t += 10000u) {
        uint32_t audible = ui_audible_position_update(&ap, 1000u + i * 10u, t, 20000u);
        if (i >= 2) {
            assert(audible == 1000u + i * 10u - 20u);
        }
    }
    assert(ap.count == UI_AUDIBLE_POSITION_SAMPLES);
}

static void test_display_through_interpolator_is_smooth(void)
{
    /* Audible anchor then extrapolation, jittery UI frames: the display
     * never steps back while playing and trails the mixer by the latency. */
    ui_audible_position_t ap;
    ui_position_interpolator_t interp;
    ui_audible_position_init(&ap);
    ui_position_interpolator_init(&interp);
    uint64_t t = 1000000u;
    uint32_t last = 0;
    srand(262);
    for (unsigned i = 0; i < 400; i++) {
        t += 16000u + (uint64_t)(rand() % 40000);
        uint32_t engine = engine_pos(1000, 1000000u, t);
        uint32_t audible = ui_audible_position_update(&ap, engine, t, LAT_US);
        uint32_t shown = ui_position_interpolator_update(&interp, audible, 600000, true, 1000, t);
        assert(shown >= last);
        if (i >= 4) {
            int lag = (int)engine - (int)shown;
            assert(lag >= 36 - (int)UI_POSITION_INTERPOLATOR_MAX_LEAD_MS - 6 && lag <= 36 + 6);
        }
        last = shown;
    }
}

int main(void)
{
    test_zero_latency_is_passthrough();
    test_steady_play_lags_by_latency();
    test_play_start_holds_then_moves();
    test_fresh_history_returns_oldest();
    test_seek_is_held_not_blended();
    test_seek_to_zero_never_negative();
    test_scratch_motion_is_replayed();
    test_pause_settles_on_engine();
    test_same_tick_keeps_newest();
    test_ring_wraps();
    test_display_through_interpolator_is_smooth();
    printf("ui_audible_position: all tests passed\n");
    return 0;
}
