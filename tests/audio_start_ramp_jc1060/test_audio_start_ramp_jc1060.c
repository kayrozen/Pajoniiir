/* JC1060 v328 start fade + PLAY start probe (audio_output_mixer.c). */
#include "audio_output_mixer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static audio_mixer_frame_t f(int16_t l, int16_t r)
{
    return (audio_mixer_frame_t){ .left = l, .right = r };
}

/* Gain rises linearly from 1/129 to 128/129 over 128 frames, then 1. */
static void test_fade_is_monotonic_and_ends_at_unity(void)
{
    audio_output_start_ramp_t r;
    audio_output_start_ramp_arm(&r);
    float prev = 0.0f;
    for (uint32_t n = 0u; n < AUDIO_OUTPUT_START_FADE_FRAMES; n++) {
        const float g = audio_output_start_ramp_next(&r, f(1000, 1000));
        assert(g > prev && g < 1.0f);
        assert(fabsf(g - (float)(n + 1u) / 129.0f) < 1e-6f);
        prev = g;
    }
    for (uint32_t n = 0u; n < 10000u; n++) {
        assert(audio_output_start_ramp_next(&r, f(1000, 1000)) == 1.0f);
    }
    /* NULL ramp: unity, no state. */
    assert(audio_output_start_ramp_next(NULL, f(1, 1)) == 1.0f);
}

/* The probe sees the raw source: first frame, largest step and where, the
 * largest step after the fade window, for exactly PROBE_FRAMES frames. */
static void test_probe_records_steps(void)
{
    audio_output_start_ramp_t r;
    audio_output_start_ramp_arm(&r);
    for (uint32_t n = 0u; n < AUDIO_OUTPUT_START_PROBE_FRAMES + 100u; n++) {
        int16_t v = (int16_t)(n % 2u ? 100 : -100);       /* steps of 200 */
        if (n == 50u) v = 9000;                            /* inside the fade */
        if (n == 3000u) v = -7000;                         /* after it */
        (void)audio_output_start_ramp_next(&r, f(v, (int16_t)-v));
    }
    assert(r.first.left == -100 && r.first.right == 100);
    assert(r.probe_left == 0u);
    assert(r.probe_count == AUDIO_OUTPUT_START_PROBE_FRAMES);
    assert(r.max_step == 8900u && r.max_step_at == 50u);
    assert(r.max_step_late == 7100u);
}

/* Re-arming (a new start) restarts both the fade and the probe. */
static void test_rearm_restarts(void)
{
    audio_output_start_ramp_t r;
    audio_output_start_ramp_arm(&r);
    for (uint32_t n = 0u; n < 6000u; n++) (void)audio_output_start_ramp_next(&r, f(5, 5));
    audio_output_start_ramp_arm(&r);
    assert(r.probe_left == AUDIO_OUTPUT_START_PROBE_FRAMES && r.max_step == 0u);
    assert(audio_output_start_ramp_next(&r, f(5, 5)) < 0.01f);
}

int main(void)
{
    test_fade_is_monotonic_and_ends_at_unity();
    test_probe_records_steps();
    test_rearm_restarts();
    puts("audio_start_ramp_jc1060 tests passed");
    return 0;
}
