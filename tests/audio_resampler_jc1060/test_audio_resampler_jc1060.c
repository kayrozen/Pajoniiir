/* JC1060 windowed-sinc resampler (v325): the upstream behaviour tests adapted
 * to the TAPS/2-frame latency, plus the 44.1 -> 48 kHz quality gate that
 * motivated it, seek/reset and pitch-change continuity, and drift. */
#include "audio_resampler.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LATENCY (AUDIO_RESAMPLER_TAPS / 2u)   /* output frames behind the source */
#define FACTOR_44K1_TO_48K (44100.0f / 48000.0f)

typedef struct {
    const audio_mixer_frame_t *frames;
    uint32_t count;
    uint32_t index;
} source_t;

typedef struct {
    uint64_t popped;
} counting_source_t;

typedef struct {
    double freq_hz;
    double rate_hz;
    double amplitude;
    uint64_t index;
} tone_source_t;

static bool pop_source(void *ctx, audio_mixer_frame_t *out_frame)
{
    source_t *source = (source_t *)ctx;
    if (source->index >= source->count) return false;
    *out_frame = source->frames[source->index++];
    return true;
}

static bool pop_counting_source(void *ctx, audio_mixer_frame_t *out_frame)
{
    counting_source_t *source = (counting_source_t *)ctx;
    source->popped++;
    out_frame->left = (int16_t)(source->popped & 0x3FFFu);
    out_frame->right = (int16_t)-out_frame->left;
    return true;
}

static bool pop_tone(void *ctx, audio_mixer_frame_t *out_frame)
{
    tone_source_t *t = (tone_source_t *)ctx;
    double v = t->amplitude * sin(2.0 * M_PI * t->freq_hz *
                                  (double)t->index / t->rate_hz);
    t->index++;
    out_frame->left = (int16_t)lrint(v);
    out_frame->right = (int16_t)-out_frame->left;
    return true;
}

static void test_reset_outputs_silence_without_source(void)
{
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    uint32_t consumed = 99;
    audio_mixer_frame_t out = audio_resampler_next(&state, 1.0f, NULL, NULL, &consumed);

    assert(out.left == 0);
    assert(out.right == 0);
    assert(consumed == 0);
}

/* Unity rate is bit-exact (a 48 kHz track at pitch 0 is untouched), delayed
 * by TAPS/2 frames: one popped frame per output, frame n out at call n + 8. */
static void test_unity_pitch_is_bit_exact_with_fixed_latency(void)
{
    audio_mixer_frame_t frames[64];
    for (uint32_t i = 0u; i < 64u; i++) {
        frames[i].left = (int16_t)(100 * (int)(i + 1u) - 3000);
        frames[i].right = (int16_t)-frames[i].left;
    }
    source_t source = { .frames = frames, .count = 64u, .index = 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    for (uint32_t n = 0u; n < 48u; n++) {
        uint32_t consumed = 0u;
        audio_mixer_frame_t out =
            audio_resampler_next(&state, 1.0f, pop_source, &source, &consumed);
        assert(consumed == 1u);
        if (n < LATENCY) {
            assert(out.left == 0 && out.right == 0);
        } else {
            assert(out.left == frames[n - LATENCY].left);
            assert(out.right == frames[n - LATENCY].right);
        }
    }
}

/* Half speed: one source frame every second output; a constant (DC) source
 * comes out at the same constant once the window is full (unity DC gain per
 * phase, so no ripple at the fractional phases). */
static void test_fractional_pitch_keeps_dc_exact(void)
{
    audio_mixer_frame_t frames[64];
    for (uint32_t i = 0u; i < 64u; i++) {
        frames[i] = (audio_mixer_frame_t){ .left = 12000, .right = -12000 };
    }
    source_t source = { .frames = frames, .count = 64u, .index = 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    uint32_t consumed_total = 0u;
    for (uint32_t n = 0u; n < 80u; n++) {
        uint32_t consumed = 0u;
        audio_mixer_frame_t out =
            audio_resampler_next(&state, 0.5f, pop_source, &source, &consumed);
        consumed_total += consumed;
        assert(consumed == ((n % 2u) == 1u ? 1u : 0u));
        if (n >= 2u * AUDIO_RESAMPLER_TAPS + 2u) {
            assert(abs(out.left - 12000) <= 1);
            assert(abs(out.right + 12000) <= 1);
        }
    }
    assert(consumed_total == 40u);
}

/* Ring underrun (pop_source returns false after data has flowed) must settle
 * on the last sample, not decay to zero - a snap to zero clicks. */
static void test_underrun_holds_last_frame(void)
{
    audio_mixer_frame_t frames[] = {
        { .left = 1000, .right = -1000 },
    };
    source_t source = { .frames = frames, .count = 1, .index = 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    uint32_t consumed = 0;
    audio_resampler_next(&state, 1.0f, pop_source, &source, &consumed);
    assert(consumed == 1);

    audio_mixer_frame_t out = { 0 };
    for (uint32_t n = 0u; n < 2u * AUDIO_RESAMPLER_TAPS; n++) {
        out = audio_resampler_next(&state, 1.0f, pop_source, &source, &consumed);
        assert(consumed == 0);
    }
    assert(out.left == 1000);
    assert(out.right == -1000);

    /* Same at a fractional rate: the held window is flat, so the filter
     * returns the held value at every phase. */
    for (uint32_t n = 0u; n < 64u; n++) {
        out = audio_resampler_next(&state, FACTOR_44K1_TO_48K, pop_source, &source,
                                   &consumed);
        assert(consumed == 0);
        assert(abs(out.left - 1000) <= 1);
        assert(abs(out.right + 1000) <= 1);
    }
}

static void test_non_finite_pitch_is_sanitized(void)
{
    audio_mixer_frame_t frames[] = {
        { .left = 123, .right = -123 },
    };
    source_t source = { .frames = frames, .count = 1, .index = 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    uint32_t consumed = 0;
    (void)audio_resampler_next(&state, NAN, pop_source, &source, &consumed);
    assert(consumed == 1u);
    assert(state.step_q32 == (UINT64_C(1) << 32));
}

/* Full-scale square wave (32 frames per half): the sinc overshoots at each
 * edge (Gibbs) and must clamp to the rail, never wrap to the other sign. */
static void test_overshoot_clamps(void)
{
    audio_mixer_frame_t frames[512];
    for (uint32_t i = 0u; i < 512u; i++) {
        int16_t v = ((i / 32u) % 2u) ? -32768 : 32767;
        frames[i] = (audio_mixer_frame_t){ .left = v, .right = v };
    }
    source_t source = { .frames = frames, .count = 512u, .index = 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);
    for (uint32_t n = 0u; n < 500u; n++) {
        uint32_t consumed = 0u;
        audio_mixer_frame_t out = audio_resampler_next(&state, FACTOR_44K1_TO_48K,
                                                       pop_source, &source, &consumed);
        /* Source position under the output, then its place in the period. */
        double pos = (double)(n + 1u) * FACTOR_44K1_TO_48K - (double)LATENCY;
        if (pos < 16.0) continue;
        double in_half = fmod(pos, 32.0);
        if (in_half < 6.0 || in_half > 26.0) continue;   /* near an edge */
        bool high = ((uint32_t)(pos / 32.0) % 2u) == 0u;
        if (high) {
            assert(out.left > 30000);
        } else {
            assert(out.left < -30000);
        }
    }
}

static void assert_five_minute_consumption(float factor)
{
    const uint32_t output_frames = 5u * 60u * 48000u;
    counting_source_t source = { 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    uint64_t consumed_total = 0u;
    for (uint32_t i = 0u; i < output_frames; i++) {
        uint32_t consumed = 0u;
        (void)audio_resampler_next(&state, factor,
                                   pop_counting_source, &source, &consumed);
        consumed_total += consumed;
    }

    uint64_t expected = (uint64_t)((double)output_frames * (double)factor);
    assert(consumed_total == expected);
    assert(source.popped == expected);
}

static void test_five_minute_q32_phase_has_zero_consumption_drift(void)
{
    assert_five_minute_consumption(44100.0f / 48000.0f);
    assert_five_minute_consumption(1.1f);
}

static void test_pitch_change_refreshes_cached_q32_step(void)
{
    counting_source_t source = { 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);

    uint64_t consumed_total = 0u;
    for (uint32_t i = 0u; i < 10u; i++) {
        uint32_t consumed = 0u;
        (void)audio_resampler_next(&state, 0.5f,
                                   pop_counting_source, &source, &consumed);
        consumed_total += consumed;
    }
    for (uint32_t i = 0u; i < 10u; i++) {
        uint32_t consumed = 0u;
        (void)audio_resampler_next(&state, 1.5f,
                                   pop_counting_source, &source, &consumed);
        consumed_total += consumed;
    }
    assert(consumed_total == 20u);
    assert(source.popped == 20u);
}

/* --- 44.1 -> 48 kHz quality (the v325 motivation) ------------------------ */

#define QN 16384u
static float s_capture[QN];

static double tone_level(double freq_hz)
{
    double re = 0.0, im = 0.0;
    for (uint32_t n = 0u; n < QN; n++) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * n / (QN - 1u));
        double ph = 2.0 * M_PI * freq_hz * n / 48000.0;
        re += s_capture[n] * w * cos(ph);
        im -= s_capture[n] * w * sin(ph);
    }
    return sqrt(re * re + im * im) / (QN / 4.0);
}

/* Level of the tone after conversion and the largest other component
 * (image/alias), both in dB relative to the input amplitude. */
static void measure_tone(double freq_hz, double *level_db, double *spur_db)
{
    const double amplitude = 0.5 * 32767.0;
    tone_source_t tone = { .freq_hz = freq_hz, .rate_hz = 44100.0,
                           .amplitude = amplitude };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);
    for (uint32_t n = 0u; n < QN + 2000u; n++) {
        uint32_t consumed = 0u;
        audio_mixer_frame_t out = audio_resampler_next(&state, FACTOR_44K1_TO_48K,
                                                       pop_tone, &tone, &consumed);
        if (n >= 2000u) s_capture[n - 2000u] = out.left;
    }
    *level_db = 20.0 * log10(tone_level(freq_hz) / amplitude);
    double worst = 0.0;
    for (double f = 300.0; f < 23800.0; f += 50.0) {
        if (fabs(f - freq_hz) < 200.0) continue;
        double a = tone_level(f);
        if (a > worst) worst = a;
    }
    *spur_db = 20.0 * log10(worst / amplitude + 1e-12);
}

static void test_44k1_to_48k_keeps_highs_and_rejects_images(void)
{
    /* Linear interpolation (v324 and before): 10 kHz -1.5 dB / -23 dB spur,
     * 15 kHz -3.4 / -15, 17 kHz -4.5 / -13. */
    static const struct { double hz, min_level_db, max_spur_db; } gate[] = {
        { 1000.0,  -0.05, -70.0 },
        { 5000.0,  -0.05, -70.0 },
        { 10000.0, -0.05, -70.0 },
        { 15000.0, -0.30, -70.0 },
        { 17000.0, -1.20, -70.0 },
    };
    for (size_t i = 0u; i < sizeof(gate) / sizeof(gate[0]); i++) {
        double level = 0.0, spur = 0.0;
        measure_tone(gate[i].hz, &level, &spur);
        printf("  44.1->48k %5.0f Hz: level %6.2f dB, worst spur %6.1f dB\n",
               gate[i].hz, level, spur);
        assert(level >= gate[i].min_level_db);
        assert(spur <= gate[i].max_spur_db);
    }
}

/* --- continuity ---------------------------------------------------------- */

/* A pitch change mid-tone (fader move, jog bend) must not jump: neighbouring
 * outputs stay within what the tone's own slope allows. */
static void test_pitch_change_mid_tone_is_continuous(void)
{
    tone_source_t tone = { .freq_hz = 440.0, .rate_hz = 44100.0,
                           .amplitude = 20000.0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);
    /* Max slope of a 440 Hz, 20000 tone sped up 1.1x: 2*pi*440*1.1/48000 *
     * 20000 ~ 1267 per output frame. */
    const int max_step = 1400;
    int prev = 0;
    for (uint32_t n = 0u; n < 20000u; n++) {
        float factor = FACTOR_44K1_TO_48K;
        if (n >= 6000u && n < 12000u) factor *= 1.06f;          /* pitch +6 % */
        if (n >= 12000u) factor *= 0.94f + 0.0001f * (float)(n % 100u); /* bend */
        uint32_t consumed = 0u;
        audio_mixer_frame_t out = audio_resampler_next(&state, factor, pop_tone,
                                                       &tone, &consumed);
        if (n > 2u * AUDIO_RESAMPLER_TAPS) {
            assert(abs((int)out.left - prev) <= max_step);
        }
        prev = out.left;
    }
}

/* Seek / cue: the engine resets the resampler and feeds the new position.
 * The output must start from silence and reach the new material within the
 * window, with no stale frame of the old position after the reset. */
static void test_reset_mid_stream_starts_clean(void)
{
    audio_mixer_frame_t old_frames[64], new_frames[64];
    for (uint32_t i = 0u; i < 64u; i++) {
        old_frames[i] = (audio_mixer_frame_t){ .left = 30000, .right = 30000 };
        new_frames[i] = (audio_mixer_frame_t){ .left = -5000, .right = -5000 };
    }
    source_t old_src = { .frames = old_frames, .count = 64u, .index = 0 };
    source_t new_src = { .frames = new_frames, .count = 64u, .index = 0 };
    audio_resampler_state_t state;
    audio_resampler_reset(&state);
    for (uint32_t n = 0u; n < 40u; n++) {
        uint32_t consumed = 0u;
        (void)audio_resampler_next(&state, FACTOR_44K1_TO_48K, pop_source,
                                   &old_src, &consumed);
    }

    audio_resampler_reset(&state);
    for (uint32_t n = 0u; n < 40u; n++) {
        uint32_t consumed = 0u;
        audio_mixer_frame_t out = audio_resampler_next(&state, FACTOR_44K1_TO_48K,
                                                       pop_source, &new_src,
                                                       &consumed);
        /* Never any trace of the old +30000 material. */
        assert(out.left <= 1000);
        if (n >= 2u * AUDIO_RESAMPLER_TAPS) {
            assert(abs(out.left + 5000) <= 1);
        }
    }
}

int main(void)
{
    test_reset_outputs_silence_without_source();
    test_unity_pitch_is_bit_exact_with_fixed_latency();
    test_fractional_pitch_keeps_dc_exact();
    test_underrun_holds_last_frame();
    test_non_finite_pitch_is_sanitized();
    test_overshoot_clamps();
    test_five_minute_q32_phase_has_zero_consumption_drift();
    test_pitch_change_refreshes_cached_q32_step();
    test_44k1_to_48k_keeps_highs_and_rejects_images();
    test_pitch_change_mid_tone_is_continuous();
    test_reset_mid_stream_starts_clean();
    puts("audio_resampler_jc1060 tests passed");
    return 0;
}
