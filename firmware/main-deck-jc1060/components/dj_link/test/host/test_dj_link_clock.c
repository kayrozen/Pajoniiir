/* v339: host tests for the beat clock estimator. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dj_link_clock.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

#define PERIOD 468750                /* 128 BPM */

/* Deterministic jitter in [-amp, amp] us. */
static uint32_t s_seed = 12345;
static int32_t jitter(int32_t amp)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (int32_t)((s_seed >> 8) % (uint32_t)(2 * amp + 1)) - amp;
}

static int64_t llabs64(int64_t v) { return v < 0 ? -v : v; }

static void test_jitter_averaged(void)
{
    dj_link_clock_t c;
    dj_link_clock_reset(&c);
    const int64_t t0 = 1000000;
    int64_t worst_fit = 0, worst_raw = 0;
    for (int k = 0; k < 80; k++) {
        const int64_t truth = t0 + (int64_t)k * PERIOD;
        const int32_t j = jitter(2000);
        dj_link_clock_beat(&c, truth + j, (uint8_t)(k % 4 + 1), PERIOD);
        dj_link_clock_fit_t f;
        CHECK(dj_link_clock_get(&c, &f));
        if (k >= 8) {
            if (llabs64(f.anchor_us - truth) > worst_fit) worst_fit = llabs64(f.anchor_us - truth);
            if (llabs64(j) > worst_raw) worst_raw = llabs64(j);
            CHECK(f.locked);
            CHECK(f.beat_in_bar == (uint8_t)(k % 4 + 1));
        }
    }
    printf("jitter test: raw worst %lld us, fit worst %lld us\n", (long long)worst_raw,
           (long long)worst_fit);
    CHECK(worst_raw > 1500);
    CHECK(worst_fit < 900);               /* the raw jitter is 2000 */
    dj_link_clock_fit_t f;
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == DJ_LINK_CLOCK_WINDOW);
    CHECK(llabs64((int64_t)f.period_us - PERIOD) < 100);
}

static void test_period_refined_from_hint(void)
{
    /* The player runs 0.1 % slow against its announced tempo. */
    dj_link_clock_t c;
    dj_link_clock_reset(&c);
    const double real = PERIOD * 1.001;
    for (int k = 0; k < 40; k++) {
        dj_link_clock_beat(&c, 5000000 + (int64_t)llround(k * real) + jitter(300), 0, PERIOD);
    }
    dj_link_clock_fit_t f;
    dj_link_clock_get(&c, &f);
    CHECK(f.locked);
    CHECK(fabs((double)f.period_us - real) < real * 0.0003);

    /* A line far from the hint (1 %) is not believed: the hint stands. */
    dj_link_clock_reset(&c);
    for (int k = 0; k < 12; k++) {
        dj_link_clock_beat(&c, (int64_t)llround(k * PERIOD * 1.01), 0, PERIOD);
    }
    dj_link_clock_get(&c, &f);
    CHECK(f.period_us == PERIOD || llabs64((int64_t)f.period_us - PERIOD) < 2000);
}

static void test_dropped_packets(void)
{
    dj_link_clock_t c;
    dj_link_clock_reset(&c);
    int64_t last_truth = 0;
    for (int k = 0; k < 30; k++) {
        if (k == 12 || k == 13 || k == 20) {
            continue;                                   /* lost on the network */
        }
        last_truth = 2000000 + (int64_t)k * PERIOD;
        dj_link_clock_beat(&c, last_truth + jitter(500), (uint8_t)(k % 4 + 1), PERIOD);
    }
    dj_link_clock_fit_t f;
    dj_link_clock_get(&c, &f);
    CHECK(f.locked && f.beats >= 10);
    CHECK(llabs64(f.anchor_us - last_truth) < 600);
    CHECK(f.beat_in_bar == (uint8_t)(29 % 4 + 1));
}

static void test_restarts(void)
{
    dj_link_clock_t c;
    dj_link_clock_fit_t f;
    dj_link_clock_reset(&c);
    for (int k = 0; k < 8; k++) {
        dj_link_clock_beat(&c, (int64_t)k * PERIOD, (uint8_t)(k % 4 + 1), PERIOD);
    }
    dj_link_clock_get(&c, &f);
    CHECK(f.locked && f.beats == 8);

    /* The bar jumps (a seek): bar says beat 3 where beat 4 is due. */
    dj_link_clock_beat(&c, 8 * (int64_t)PERIOD, 3, PERIOD);
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 1 && !f.locked && f.beat_in_bar == 3);

    /* Tempo change of 1 %. */
    for (int k = 1; k < 6; k++) {
        dj_link_clock_beat(&c, (8 + k) * (int64_t)PERIOD, (uint8_t)((3 - 1 + k) % 4 + 1), PERIOD);
    }
    dj_link_clock_get(&c, &f);
    CHECK(f.locked);
    dj_link_clock_beat(&c, 14 * (int64_t)PERIOD, (uint8_t)((3 - 1 + 6) % 4 + 1), (uint32_t)(PERIOD * 1.01));
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 1 && !f.locked && f.period_us == (uint32_t)(PERIOD * 1.01));

    /* A silence of more than DJ_LINK_CLOCK_GAP_BEATS beats. */
    dj_link_clock_reset(&c);
    for (int k = 0; k < 6; k++) {
        dj_link_clock_beat(&c, (int64_t)k * PERIOD, 0, PERIOD);
    }
    dj_link_clock_beat(&c, 12 * (int64_t)PERIOD, 0, PERIOD);
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 1);
}

static void test_duplicates_and_outliers(void)
{
    dj_link_clock_t c;
    dj_link_clock_fit_t f;
    dj_link_clock_reset(&c);
    for (int k = 0; k < 8; k++) {
        dj_link_clock_beat(&c, (int64_t)k * PERIOD, (uint8_t)(k % 4 + 1), PERIOD);
    }
    /* A second packet 10 ms after the beat: not a new beat. */
    dj_link_clock_beat(&c, 7 * (int64_t)PERIOD + 10000, 4, PERIOD);
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 8);

    /* One packet 80 ms late (more than 10 % of a beat): skipped. */
    dj_link_clock_beat(&c, 8 * (int64_t)PERIOD + 80000, 1, PERIOD);
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 8 && f.beat_in_bar == 4);
    /* The next one on time is taken, two beats on from the last used. */
    dj_link_clock_beat(&c, 9 * (int64_t)PERIOD + 300, 2, PERIOD);
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 9 && f.beat_in_bar == 2);

    /* Three bad ones in a row: restart on the third. */
    dj_link_clock_beat(&c, 10 * (int64_t)PERIOD + 90000, 3, PERIOD);
    dj_link_clock_beat(&c, 10 * (int64_t)PERIOD + 90500, 3, PERIOD);
    dj_link_clock_beat(&c, 10 * (int64_t)PERIOD + 91000, 3, PERIOD);
    dj_link_clock_get(&c, &f);
    CHECK(f.beats == 1);
}

static void test_bar_unknown_and_inputs(void)
{
    dj_link_clock_t c;
    dj_link_clock_fit_t f;
    dj_link_clock_reset(&c);
    CHECK(!dj_link_clock_get(&c, &f) && !f.valid);
    dj_link_clock_beat(&c, 1000, 0, 0);                 /* no usable tempo */
    CHECK(!dj_link_clock_get(&c, &f));
    for (int k = 0; k < 6; k++) {
        dj_link_clock_beat(&c, (int64_t)k * PERIOD, 0, PERIOD);   /* bar unknown */
    }
    dj_link_clock_get(&c, &f);
    CHECK(f.locked && f.beat_in_bar == 0);

    dj_link_clock_bank_t b;
    dj_link_clock_bank_reset(&b);
    dj_link_clock_bank_beat(&b, 0, 1000, 1, PERIOD);
    dj_link_clock_bank_beat(&b, 7, 1000, 1, PERIOD);
    CHECK(!dj_link_clock_bank_get(&b, 0, &f) && !dj_link_clock_bank_get(&b, 7, &f));
    dj_link_clock_bank_beat(&b, 3, 1000, 2, PERIOD);
    CHECK(dj_link_clock_bank_get(&b, 3, &f) && f.beat_in_bar == 2 && !f.locked);
    CHECK(!dj_link_clock_bank_get(&b, 2, &f));
}

static void test_large_times(void)
{
    /* esp_timer runs for days: 64-bit microseconds, no precision loss. */
    dj_link_clock_t c;
    dj_link_clock_fit_t f;
    dj_link_clock_reset(&c);
    const int64_t base = 40LL * 24 * 3600 * 1000000LL;
    int64_t truth = 0;
    for (int k = 0; k < 20; k++) {
        truth = base + (int64_t)k * PERIOD;
        dj_link_clock_beat(&c, truth, (uint8_t)(k % 4 + 1), PERIOD);
    }
    dj_link_clock_get(&c, &f);
    CHECK(llabs64(f.anchor_us - truth) <= 1 && f.rms_us < 1.0f);
}

int main(void)
{
    test_jitter_averaged();
    test_period_refined_from_hint();
    test_dropped_packets();
    test_restarts();
    test_duplicates_and_outliers();
    test_bar_unknown_and_inputs();
    test_large_times();
    if (s_failures) {
        printf("%d dj_link_clock check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_clock tests passed\n");
    return 0;
}
