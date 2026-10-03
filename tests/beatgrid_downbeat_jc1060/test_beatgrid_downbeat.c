/* JC1060 v306: the beat grid's downbeat is PQTZ beat number 1 (rekordbox,
 * dbserver beatgrid, vynull analysis/beatgrid.go), not phase 0. Reading
 * `beat_phase % 4 == 0` as the downbeat drew it on beat 4, one beat early.
 * The fixture is a vynull grid whose downbeat the user moved to the third
 * beat: numbers 3, 4, 1, 2, 3, 4, 1, ... */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_beat_indicator.h"
#include "ui_overview_grid.h"
#include "ui_overview_renderer.h"

enum { BEATS = 16, FIRST_MS = 100, BEAT_MS = 500, DOWNBEAT_IDX = 2 };

static anlz_beat_t s_beats[BEATS];

static void grid_fixture(void)
{
    for (uint16_t i = 0; i < BEATS; i++) {
        s_beats[i] = (anlz_beat_t){
            .beat_phase = (uint16_t)((i + 4u - DOWNBEAT_IDX) % 4u + 1u),
            .bpm_x100 = 12000,
            .time_ms = FIRST_MS + (uint32_t)i * BEAT_MS,
        };
    }
    assert(s_beats[0].beat_phase == 3u && s_beats[DOWNBEAT_IDX].beat_phase == 1u);
}

static void test_convention_helpers(void)
{
    assert(anlz_beat_is_downbeat(1u));
    for (uint16_t p = 2u; p <= 4u; p++) {
        assert(!anlz_beat_is_downbeat(p));
        assert(anlz_beat_bar_index(p) == p - 1u);
    }
    assert(anlz_beat_bar_index(1u) == 0u);
    /* 0 = unknown (pre-nexus / unanalyzed): never the downbeat */
    assert(!anlz_beat_is_downbeat(0u) && anlz_beat_bar_index(0u) == 0u);
    assert(!anlz_beat_is_downbeat(5u) && anlz_beat_bar_index(5u) == 0u);
}

static void test_beat_indicator_follows_beat_one(void)
{
    const uint32_t down = FIRST_MS + DOWNBEAT_IDX * BEAT_MS;
    ui_beat_indicator_state_t s = ui_beat_indicator_calculate(down + 50u, s_beats, BEATS, 0);
    assert(s.valid && s.downbeat && s.phase == 0u);
    /* the beat before is beat 4 of the previous bar, not the downbeat */
    s = ui_beat_indicator_calculate(down - BEAT_MS + 50u, s_beats, BEATS, 0);
    assert(s.valid && !s.downbeat && s.phase == 3u);
    s = ui_beat_indicator_calculate(FIRST_MS + 50u, s_beats, BEATS, 0);
    assert(s.valid && !s.downbeat && s.phase == 2u);
    s = ui_beat_indicator_calculate(down + 4u * BEAT_MS + 50u, s_beats, BEATS, 0);
    assert(s.valid && s.downbeat && s.phase == 0u);
}

static void test_overview_columns_on_downbeats(void)
{
    int columns[8];
    /* 1 px per 10 ms, no spacing limit */
    const uint32_t duration_ms = 8000u;
    size_t n = ui_overview_grid_build_columns(s_beats, BEATS, duration_ms, 800, 1,
                                              columns, sizeof(columns) / sizeof(columns[0]));
    assert(n == 4u);
    for (size_t k = 0; k < n; k++) {
        const uint32_t t = FIRST_MS + (uint32_t)(DOWNBEAT_IDX + 4u * k) * BEAT_MS;
        assert(columns[k] == (int)(t / 10u));
    }
}

static void test_zoom_grid_marks_beat_one(void)
{
    enum { W = 400, H = 40 };
    static uint8_t px[W * H];
    /* window 0..4000 ms over 400 px: x = t / 10 */
    ui_overview_renderer_draw_main(px, W, W, H, NULL, 8000u, &(anlz_metadata_t){
                                       .beats = s_beats, .beat_count = BEATS },
                                   2000u, 4000u);
    const ui_overview_grid_style_t down = ui_overview_grid_style_for_phase(1u);
    const ui_overview_grid_style_t reg = ui_overview_grid_style_for_phase(2u);
    assert(down.palette_index != reg.palette_index);
    const int mid = H / 2;
    for (uint16_t i = 0; i < 8u; i++) {
        const int x = (int)(s_beats[i].time_ms / 10u);
        const uint8_t want = s_beats[i].beat_phase == 1u ? down.palette_index : reg.palette_index;
        if (px[mid * W + x] != want) {
            fprintf(stderr, "beat %u (number %u) at x=%d: palette %u, want %u\n", i,
                    s_beats[i].beat_phase, x, px[mid * W + x], want);
            assert(0);
        }
    }
    /* the old reading put the downbeat on beat 4 (x = 60) */
    assert(px[mid * W + (FIRST_MS + 1 * BEAT_MS) / 10] == reg.palette_index);
    assert(px[mid * W + (FIRST_MS + 2 * BEAT_MS) / 10] == down.palette_index);

    /* Over a waveform only the downbeats are redrawn on top of it. */
    static uint8_t wave[8u * 150u];
    memset(wave, 0x1f, sizeof(wave));   /* full height everywhere */
    const ui_waveform_source_t src = { UI_WAVEFORM_SOURCE_HIGH, wave, sizeof(wave) };
    ui_overview_renderer_draw_main(px, W, W, H, &src, 8000u, &(anlz_metadata_t){
                                       .beats = s_beats, .beat_count = BEATS },
                                   2000u, 4000u);
    const int row = H / 4;   /* inside the waveform, clear of the playhead */
    for (uint16_t i = 0; i < 8u; i++) {
        const int x = (int)(s_beats[i].time_ms / 10u);
        const bool on_top = px[row * W + x] == down.palette_index;
        if (on_top != (s_beats[i].beat_phase == 1u)) {
            fprintf(stderr, "over waveform: beat %u (number %u) at x=%d: palette %u\n", i,
                    s_beats[i].beat_phase, x, px[row * W + x]);
            assert(0);
        }
    }
}

int main(void)
{
    grid_fixture();
    test_convention_helpers();
    test_beat_indicator_follows_beat_one();
    test_overview_columns_on_downbeats();
    test_zoom_grid_marks_beat_one();
    puts("beatgrid_downbeat_jc1060: all tests passed");
    return 0;
}
