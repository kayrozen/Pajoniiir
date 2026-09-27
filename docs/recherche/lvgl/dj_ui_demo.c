/* Demo: fake waveforms, library, playback. Replace with your MIDI/network data source. */
#include "dj_ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PPS 50

static const dj_track_t lib_src[8] = {
    { "Etherwood - TANGARA.mp3",     "Etherwood",          "7A", 174, 223000 },
    { "Bru-C - Differently (Fe...",  "Bru-C",              "6A", 170, 149000 },
    { "London Elektricity - Ec...",  "London Elektricity", "6A", 174, 255000 },
    { "Brodie - Rig Fairy (fea...",  "Brodie",             "7A", 175, 192000 },
    { "Julie London - Cry Me A...",  "Julie London",       "9A", 126, 177000 },
    { "WINK - cantBREATHE.mp3",      "WINK",               "8A", 174, 248000 },
    { "Gardna - Body Groovin'.mp3",  "Gardna",             "3A", 165, 167000 },
    { "Kleu - Insane.mp3",           "Kleu",               "9A", 176, 271000 },
};

static uint8_t order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static dj_track_t page[8];
static dj_sort_t cur_sort = DJ_SORT_NONE;
static int8_t loaded_id[2] = { 0, 6 };

static uint8_t *peaks[2], *cores[2];
static uint32_t count[2], len_ms[2], pos_ms[2];
static float tempo[2] = { 1.2f, -0.4f };
static float bpm[2];
static uint8_t fx_beat = 1;
static bool fx_on = true;

static float wave(float seed, int i)
{
    float x = i * 0.21f + seed;
    float v = 0.55f + 0.3f * sinf(x) * sinf(x * 0.37f + seed) + 0.2f * sinf(x * 2.7f + seed * 3);
    v *= 0.55f + 0.45f * fabsf(sinf(i * 0.013f + seed));
    v = fabsf(v);
    return v < 0.06f ? 0.06f : (v > 1.f ? 1.f : v);
}

static int cmp_sort(const void *a, const void *b)
{
    const dj_track_t *x = &lib_src[*(const uint8_t *)a], *y = &lib_src[*(const uint8_t *)b];
    switch (cur_sort) {
    case DJ_SORT_ARTIST: return strcmp(x->artist, y->artist);
    case DJ_SORT_NAME:   return strcmp(x->title, y->title);
    case DJ_SORT_BPM:    return (int)(x->bpm - y->bpm);
    case DJ_SORT_KEY:    return atoi(x->key) - atoi(y->key);
    default:             return *(const uint8_t *)a - *(const uint8_t *)b;
    }
}

static void push_library(void)
{
    for (int i = 0; i < 8; i++) order[i] = (uint8_t)i;
    qsort(order, 8, 1, cmp_sort);
    for (int r = 0; r < 8; r++) page[r] = lib_src[order[r]];
    dj_ui_library_set_rows(page, 8);
    for (int d = 0; d < 2; d++) {
        int8_t row = -1;
        for (int r = 0; r < 8; r++) if (order[r] == loaded_id[d]) row = (int8_t)r;
        dj_ui_library_set_loaded((uint8_t)d, row);
    }
}

static void load_track(uint8_t d, uint8_t id)
{
    const dj_track_t *t = &lib_src[id];
    float seed = 1.3f + id * 1.7f;
    len_ms[d] = t->len_ms;
    count[d] = t->len_ms / 1000 * PPS;
    free(peaks[d]); free(cores[d]);
    peaks[d] = malloc(count[d]);
    cores[d] = malloc(count[d]);
    for (uint32_t i = 0; i < count[d]; i++) {
        peaks[d][i] = (uint8_t)(wave(seed, (int)i) * 255);
        cores[d][i] = (uint8_t)((0.4f + 0.3f * wave(seed + 2, (int)i)) * 255);
    }
    pos_ms[d] = t->len_ms / 3;
    bpm[d] = t->bpm;
    dj_ui_set_track(d, t->title, t->artist, "USB 1", id + 1, 52, t->len_ms);
    dj_ui_set_key(d, t->key);
    dj_ui_set_bpm(d, t->bpm, 120);
    dj_ui_set_tempo(d, tempo[d]);
    dj_ui_set_waveform(d, peaks[d], cores[d], count[d], PPS);
    dj_ui_set_artwork(d, NULL);
    for (uint8_t k = 0; k < 8; k++) dj_ui_set_hotcue(d, k, false, 0, 0);
}

static void push_fx(void)
{
    static const float mult[4] = { 0.25f, 0.5f, 1.f, 2.f };
    uint16_t ms = (uint16_t)(60000.f / (bpm[0] * (1 + tempo[0] / 100)) * mult[fx_beat]);
    dj_ui_set_fx("ECHO", 1, fx_beat, ms, 68, fx_on);
}

/* ---- callbacks ---- */
static void on_lib_load(uint8_t deck, uint8_t row)
{
    loaded_id[deck] = (int8_t)order[row];
    load_track(deck, order[row]);
    dj_ui_library_set_loaded(deck, (int8_t)row);
    dj_ui_library_set_deck_status(deck, "LOADED");
    if (deck == 0) push_fx();
}

static void on_lib_sort(dj_sort_t s) { cur_sort = s; push_library(); }

static void on_hotcue(uint8_t deck, uint8_t idx)
{
    static bool set[2][8] = { { true, false, false, false, false, false, false, true },
                              { true, false, true, false, false, false, true, false } };
    set[deck][idx] = !set[deck][idx];
    dj_ui_set_hotcue(deck, idx, set[deck][idx], pos_ms[deck], idx);
}

static void on_fx_beat(uint8_t i) { fx_beat = i; push_fx(); }
static void on_fx_toggle(void) { fx_on = !fx_on; push_fx(); }

static void tick(lv_timer_t *t)
{
    (void)t;
    for (uint8_t d = 0; d < 2; d++) {
        pos_ms[d] += (uint32_t)(33 * (1 + tempo[d] / 100));
        if (pos_ms[d] >= len_ms[d]) pos_ms[d] = 0;
        dj_ui_set_position(d, pos_ms[d]);
    }
}

void dj_ui_demo_start(void)
{
    static const dj_ui_callbacks_t cb = {
        .on_hotcue = on_hotcue, .on_fx_beat = on_fx_beat, .on_fx_toggle = on_fx_toggle,
        .on_lib_load = on_lib_load, .on_lib_sort = on_lib_sort,
    };
    dj_ui_set_callbacks(&cb);

    load_track(0, 0);
    load_track(1, 6);
    dj_ui_set_hotcue(0, 0, true, 11000, 3);
    dj_ui_set_hotcue(0, 7, true, 0, 5);
    dj_ui_set_hotcue(1, 0, true, 0, 0);
    dj_ui_set_hotcue(1, 2, true, 64000, 2);
    dj_ui_set_hotcue(1, 6, true, 118000, 6);

    push_library();
    dj_ui_library_set_info("LOCAL USB", 52, 1, 7);
    dj_ui_library_set_deck_status(0, "READY");
    push_fx();

    lv_timer_create(tick, 33, NULL);
}
