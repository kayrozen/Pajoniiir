/* Demo: fake waveforms, library, playback. Replace with your MIDI/network data source.
 *
 * Simulator copy of docs/recherche/lvgl/dj_ui_demo.c, extended to drive every
 * dj_ui setter. Deck 1 uses the built-in PEAKS surfaces; Deck 2 uses IMAGE
 * surfaces fed from RGB565 buffers the way the firmware wave cache /
 * draw_mini pipeline will feed them. Everything advances from LVGL timers only,
 * so screenshots are deterministic. */
#include "dj_ui_demo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PPS 50
#define STRIP_MARGIN 128             /* like the firmware wave cache margin */
#define STRIP_MAX_W  (1024 + 2 * STRIP_MARGIN)
#define STRIP_MAX_H  160
#define MINI_MAX_W   512
#define MINI_MAX_H   100

static const dj_track_t lib_src[8] = {
    { "Etherwood - TANGARA.mp3",     "Etherwood",          "7A", 174, 223000, "DB" },
    { "Bru-C - Differently (Fe...",  "Bru-C",              "6A", 170, 149000, NULL },
    { "London Elektricity - Ec...",  "London Elektricity", "6A", 174, 255000, "NET" },
    { "Brodie - Rig Fairy (fea...",  "Brodie",             "7A", 175, 192000, "META" },
    { "Julie London - Cry Me A...",  "Julie London",       "9A", 126, 177000, NULL },
    { "WINK - cantBREATHE.mp3",      "WINK",               "8A", 174, 248000, "42%" },
    { "Gardna - Body Groovin'.mp3",  "Gardna",             "3A", 165, 167000, "DB" },
    { "Kleu - Insane.mp3",           "Kleu",               "9A", 176, 271000, NULL },
};

static uint8_t order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static dj_track_t page[8];
static dj_sort_t cur_sort = DJ_SORT_NONE;
static bool cur_desc;
static int8_t loaded_id[2] = { 0, 6 };

static uint8_t *peaks[2], *cores[2];
static uint32_t count[2], len_ms[2], pos_ms[2], cue_ms[2];
static float tempo[2] = { 1.2f, -0.4f };
static float bpm[2];
static bool playing[2] = { true, false }, mt_on[2] = { true, false }, cue_held[2];
static uint8_t fx_beat = 1;
static bool fx_on = true;

/* pending load (load gate) */
static int8_t load_deck = -1;
static uint8_t load_id, load_pct;
static lv_timer_t *load_timer, *tick_timer;

/* Deck 2 IMAGE surfaces */
static uint16_t strip_px[STRIP_MAX_W * STRIP_MAX_H], mini_px[MINI_MAX_W * MINI_MAX_H];
static lv_image_dsc_t strip_img, mini_img;
static int32_t strip_w, strip_h, strip_origin_px;   /* strip column 0 = track pixel strip_origin_px */
static float strip_mspp;

/* settings state */
static uint8_t master_trim, rec_secs;
static bool main_pcm, local_on, blackout_test, link_on = true, recording;

static float wave(float seed, int i)
{
    float x = i * 0.21f + seed;
    float v = 0.55f + 0.3f * sinf(x) * sinf(x * 0.37f + seed) + 0.2f * sinf(x * 2.7f + seed * 3);
    v *= 0.55f + 0.45f * fabsf(sinf(i * 0.013f + seed));
    v = fabsf(v);
    return v < 0.06f ? 0.06f : (v > 1.f ? 1.f : v);
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

static int cmp_field(const void *a, const void *b)
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

static int cmp_sort(const void *a, const void *b)
{
    return cur_desc ? -cmp_field(a, b) : cmp_field(a, b);
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

/* ---- Deck 2 image surfaces: stand-ins for the ANLZ wave cache and draw_mini ---- */

static uint8_t peak_at(uint8_t d, float ms)
{
    if (ms < 0) return 0;
    uint32_t i = (uint32_t)(ms * PPS / 1000.f);
    return i < count[d] ? peaks[d][i] : 0;
}

/* Re-render the zoom strip only when the playhead leaves the margin, then pan
 * it with src_x, like ui_overview_wave_cache. */
static void render_strip(bool force)
{
    lv_area_t a;
    if (!dj_ui_wave_get_area(1, DJ_WAVE_ZOOM, &a)) return;
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    if (w + 2 * STRIP_MARGIN > STRIP_MAX_W || h > STRIP_MAX_H) return;
    strip_mspp = (float)dj_ui_wave_get_window_ms(1) / (float)w;
    int32_t pos_px = (int32_t)((float)pos_ms[1] / strip_mspp);
    int32_t view_x = pos_px - w / 2 - strip_origin_px;
    if (force || strip_w != w + 2 * STRIP_MARGIN || view_x < 0 || view_x > 2 * STRIP_MARGIN) {
        strip_w = w + 2 * STRIP_MARGIN;
        strip_h = h;
        strip_origin_px = pos_px - w / 2 - STRIP_MARGIN;
        const uint16_t bg = rgb565(0x08090b), hi = rgb565(0xee8fd0), lo = rgb565(0x7a3f6a);
        for (int32_t x = 0; x < strip_w; x++) {
            int32_t hh = peak_at(1, (float)(strip_origin_px + x) * strip_mspp) * (h / 2 - 6) / 255;
            for (int32_t y = 0; y < h; y++) {
                int32_t dy = abs(y - h / 2);
                strip_px[y * strip_w + x] = dy > hh ? bg : (dy > hh / 2 ? lo : hi);
            }
        }
        strip_img.header.magic = LV_IMAGE_HEADER_MAGIC;
        strip_img.header.cf = LV_COLOR_FORMAT_RGB565;
        strip_img.header.w = (uint32_t)strip_w;
        strip_img.header.h = (uint32_t)strip_h;
        strip_img.header.stride = (uint32_t)strip_w * 2;
        strip_img.data = (const uint8_t *)strip_px;
        strip_img.data_size = (uint32_t)(strip_w * strip_h * 2);
        view_x = pos_px - w / 2 - strip_origin_px;
        dj_ui_wave_set_image(1, DJ_WAVE_ZOOM, NULL, 0);   /* new pixels behind the same descriptor */
    }
    dj_ui_wave_set_image(1, DJ_WAVE_ZOOM, &strip_img, view_x);
}

static void render_mini(void)
{
    lv_area_t a;
    if (!dj_ui_wave_get_area(1, DJ_WAVE_MINI, &a)) return;
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    if (w > MINI_MAX_W || h > MINI_MAX_H || !len_ms[1]) return;
    const uint16_t bg = rgb565(0x08090b), fg = rgb565(0xee8fd0);
    for (int32_t x = 0; x < w; x++) {
        uint8_t m = 0;
        uint32_t t0 = (uint32_t)((uint64_t)x * len_ms[1] / w), t1 = (uint32_t)((uint64_t)(x + 1) * len_ms[1] / w);
        for (uint32_t t = t0; t < t1; t += 1000 / PPS) if (peak_at(1, (float)t) > m) m = peak_at(1, (float)t);
        int32_t hh = m * (h - 6) / 255;
        for (int32_t y = 0; y < h; y++) mini_px[y * w + x] = (y >= h - 3 - hh && y < h - 3) ? fg : bg;
    }
    mini_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    mini_img.header.cf = LV_COLOR_FORMAT_RGB565;
    mini_img.header.w = (uint32_t)w;
    mini_img.header.h = (uint32_t)h;
    mini_img.header.stride = (uint32_t)w * 2;
    mini_img.data = (const uint8_t *)mini_px;
    mini_img.data_size = (uint32_t)(w * h * 2);
    dj_ui_wave_set_image(1, DJ_WAVE_MINI, NULL, 0);
    dj_ui_wave_set_image(1, DJ_WAVE_MINI, &mini_img, 0);
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
    cue_ms[d] = pos_ms[d];
    bpm[d] = t->bpm;
    dj_ui_set_track(d, t->title, t->artist, "USB 1", id + 1, 52, t->len_ms);
    dj_ui_set_key(d, t->key);
    dj_ui_set_bpm(d, t->bpm, 120);
    dj_ui_set_tempo(d, tempo[d]);
    dj_ui_set_waveform(d, peaks[d], cores[d], count[d], PPS);
    dj_ui_set_artwork(d, NULL);
    dj_ui_set_cue_point(d, true, cue_ms[d]);
    dj_ui_set_loop(d, false, 0, 0);
    for (uint8_t k = 0; k < 8; k++) {
        dj_ui_set_hotcue(d, k, false, 0, 0);
        dj_ui_set_hotcue_loop(d, k, false);
    }
    if (d == 1) {
        render_mini();
        render_strip(true);
    }
}

static void push_fx(void)
{
    static const float mult[4] = { 0.25f, 0.5f, 1.f, 2.f };
    uint16_t ms = (uint16_t)(60000.f / (bpm[0] * (1 + tempo[0] / 100)) * mult[fx_beat]);
    dj_ui_set_fx("ECHO", 1, fx_beat, ms, 68, fx_on);
}

static float eff_bpm(uint8_t d) { return bpm[d] * (1 + tempo[d] / 100); }

/* phase 0..3 of the bar at the current position (grid starts at 120 ms) */
static uint8_t beat_phase(uint8_t d)
{
    float beat = 60000.f / bpm[d];
    long n = (long)floorf(((float)pos_ms[d] - 120.f) / beat);
    return (uint8_t)(((n % 4) + 4) % 4);
}

static void push_link(void)
{
    dj_ui_set_link_enabled(link_on);
    if (!link_on) {
        dj_ui_set_link_peers(NULL, 0);
        dj_ui_set_field(DJ_F_LINK_STATUS, "DJ LINK: OFF", DJ_TONE_MUTED);
        dj_ui_set_link_master(NULL);
        return;
    }
    const dj_ui_link_peer_t peers[3] = {
        { 1, "CDJ-3000", 174.2f, true, true, true },
        { 2, "CDJ-3000", 0.f, false, false, false },
        { 3, "XDJ-XZ", 128.0f, false, true, false },
    };
    dj_ui_set_link_peers(peers, 3);
    dj_ui_set_field(DJ_F_LINK_STATUS, "DJ LINK: ON P4 - CDJ-3000 #1 174.2 BPM ON AIR", DJ_TONE_OK);
}

static void push_master(void)
{
    if (!link_on) return;
    const dj_ui_link_master_t m = { 1, eff_bpm(0), (uint8_t)(beat_phase(0) + 1), true };
    dj_ui_set_link_master(&m);
}

static void push_settings(void)
{
    static const char *trim[3] = { "MASTER: 0 dB", "MASTER: -3 dB", "MASTER: -6 dB" };
    dj_ui_set_field(DJ_F_MASTER, trim[master_trim], master_trim ? DJ_TONE_WARN : DJ_TONE_NORMAL);
    dj_ui_set_field(DJ_F_OUT_MAIN, main_pcm ? "MAIN: PCM5102A RCA" : "MAIN: USB (DDJ)", DJ_TONE_INFO);
    dj_ui_set_field(DJ_F_LOCAL, local_on ? "LOCAL: ES8311 monitor" : "LOCAL: disabled",
                    local_on ? DJ_TONE_OK : DJ_TONE_MUTED);
    dj_ui_set_field(DJ_F_UI_RENDER, blackout_test ? "TEST: UI OFF on PLAY" : "UI render: normal",
                    blackout_test ? DJ_TONE_WARN : DJ_TONE_NORMAL);
    dj_ui_set_recording(recording);
    char b[40];
    snprintf(b, sizeof b, "REC %02u:%02u  %u MB", rec_secs / 60u, rec_secs % 60u, rec_secs / 6u);
    dj_ui_set_field(DJ_F_REC_STATUS, recording || rec_secs ? b : "REC --:--", recording ? DJ_TONE_ERROR : DJ_TONE_MUTED);
}

/* ---- callbacks ---- */

static void load_step(lv_timer_t *t)
{
    (void)t;
    char b[24];
    load_pct = (uint8_t)(load_pct + 20);
    if (load_pct < 100) {
        snprintf(b, sizeof b, "LOADING %u%%", load_pct);
        dj_ui_library_set_status(b, DJ_TONE_INFO);
        dj_ui_library_set_progress(load_pct);
        snprintf(b, sizeof b, "D%d LOAD %u%%", load_deck + 1, load_pct);
        dj_ui_set_status(b, DJ_TONE_INFO);
        return;
    }
    uint8_t d = (uint8_t)load_deck;
    lv_timer_pause(load_timer);
    loaded_id[d] = (int8_t)load_id;
    load_track(d, load_id);
    push_library();
    snprintf(b, sizeof b, "D%u LOADED FROM #3", d + 1u);
    dj_ui_library_set_status(b, DJ_TONE_OK);
    dj_ui_library_set_progress(-1);
    dj_ui_library_set_deck_status(d, "LOADED");
    dj_ui_library_set_load_enabled(true);
    dj_ui_set_status("TRACK LOADED", DJ_TONE_OK);
    if (d == 0) push_fx();
    load_deck = -1;
}

static void on_lib_load(uint8_t deck, uint8_t row)
{
    /* load gate: one load at a time, dj_ui blocks LOAD while disabled */
    load_deck = (int8_t)deck;
    load_id = order[row];
    load_pct = 0;
    dj_ui_library_set_load_enabled(false);
    dj_ui_library_set_status("LOADING FROM #3", DJ_TONE_INFO);
    dj_ui_library_set_progress(0);
    lv_timer_resume(load_timer);
}

/* Second tap on the sorted column reverses it, like the firmware library. */
static void on_lib_sort(dj_sort_t s)
{
    cur_desc = cur_sort == s ? !cur_desc : false;
    cur_sort = s;
    dj_ui_library_set_sort(cur_sort, cur_desc);
    push_library();
}

static void on_lib_source(void)
{
    static bool cancel;
    cancel = !cancel;
    dj_ui_library_set_source_label(cancel ? "CANCEL" : "SOURCE");
    dj_ui_library_set_info(cancel ? "USB CDJ-3000 #3" : "LOCAL USB", 52, 1, 7);
}

static void on_hotcue(uint8_t deck, uint8_t idx)
{
    static bool set[2][8] = { { true, false, false, false, false, false, false, true },
                              { true, false, true, false, false, false, true, false } };
    set[deck][idx] = !set[deck][idx];
    dj_ui_set_hotcue(deck, idx, set[deck][idx], pos_ms[deck], idx);
}

static void on_fx_beat(uint8_t i) { fx_beat = i; push_fx(); }
static void on_fx_toggle(void) { fx_on = !fx_on; push_fx(); }

static void on_play(uint8_t d)
{
    playing[d] = !playing[d];
    cue_held[d] = false;
    dj_ui_set_transport(d, playing[d], false);
    dj_ui_set_status(playing[d] ? (d ? "D2 PLAY" : "D1 PLAY") : (d ? "D2 PAUSE" : "D1 PAUSE"), DJ_TONE_NORMAL);
}

/* CDJ-style CUE: while playing jump back to the cue point and pause, while
 * paused set the cue point at the playhead. */
static void on_cue(uint8_t d)
{
    if (playing[d]) {
        playing[d] = false;
        pos_ms[d] = cue_ms[d];
        dj_ui_set_position(d, pos_ms[d]);
    } else {
        cue_ms[d] = pos_ms[d];
        dj_ui_set_cue_point(d, true, cue_ms[d]);
    }
    cue_held[d] = true;
    dj_ui_set_transport(d, playing[d], true);
}

static void on_master_tempo(uint8_t d) { mt_on[d] = !mt_on[d]; dj_ui_set_master_tempo(d, mt_on[d]); }

static void on_seek(uint8_t d, uint32_t ms, dj_wave_t wave)
{
    (void)wave;
    pos_ms[d] = ms;
    dj_ui_set_position(d, ms);
}

static void on_target(uint8_t d) { dj_ui_set_target(d); }

static void on_field(dj_field_t f)
{
    switch (f) {
    case DJ_F_MASTER:    master_trim = (uint8_t)((master_trim + 1) % 3); break;
    case DJ_F_OUT_MAIN:  main_pcm = !main_pcm; break;
    case DJ_F_LOCAL:     local_on = !local_on; break;
    case DJ_F_UI_RENDER: blackout_test = !blackout_test; break;
    default:             return;
    }
    push_settings();
}

static void on_link(bool on) { link_on = on; push_link(); push_master(); }

static void on_record(void)
{
    recording = !recording;
    if (recording) rec_secs = 0;
    push_settings();
}

static void on_wake(void) { dj_ui_set_status("WAKE", DJ_TONE_INFO); }

static void tick(lv_timer_t *t)
{
    (void)t;
    static uint32_t n;
    n++;
    for (uint8_t d = 0; d < 2; d++) {
        if (playing[d]) {
            pos_ms[d] += (uint32_t)(33 * (1 + tempo[d] / 100));
            if (pos_ms[d] >= len_ms[d]) pos_ms[d] = 0;
        }
        dj_ui_set_position(d, pos_ms[d]);
        dj_ui_set_beat(d, true, beat_phase(d), beat_phase(d) == 0);
        dj_ui_set_vu(d, playing[d] ? peak_at(d, (float)pos_ms[d]) : 0);
    }
    render_strip(false);
    push_master();
    if (recording && n % 30 == 0) {
        rec_secs++;
        push_settings();
    }
}

void dj_ui_demo_start(void)
{
    static const dj_ui_callbacks_t cb = {
        .on_hotcue = on_hotcue, .on_fx_beat = on_fx_beat, .on_fx_toggle = on_fx_toggle,
        .on_lib_load = on_lib_load, .on_lib_sort = on_lib_sort, .on_lib_source = on_lib_source,
        .on_play = on_play, .on_cue = on_cue, .on_master_tempo = on_master_tempo, .on_seek = on_seek,
        .on_target = on_target, .on_field = on_field, .on_link = on_link, .on_record = on_record,
        .on_wake = on_wake,
    };
    dj_ui_set_callbacks(&cb);

    /* the IMAGE surfaces need the final layout for their pixel size */
    lv_obj_update_layout(lv_screen_active());
    dj_ui_wave_set_source(1, DJ_WAVE_ZOOM, DJ_WAVE_SRC_IMAGE);
    dj_ui_wave_set_source(1, DJ_WAVE_MINI, DJ_WAVE_SRC_IMAGE);

    load_track(0, 0);
    load_track(1, 6);
    dj_ui_set_hotcue(0, 0, true, 11000, 3);
    dj_ui_set_hotcue(0, 7, true, 0, 5);
    dj_ui_set_hotcue(1, 0, true, 0, 0);
    dj_ui_set_hotcue(1, 2, true, 64000, 2);
    dj_ui_set_hotcue(1, 6, true, 118000, 6);
    dj_ui_set_hotcue_loop(1, 6, true);
    dj_ui_set_hotcue(0, 1, true, pos_ms[0] + 2500, 1);
    dj_ui_set_loop(1, true, pos_ms[1] - 1500, pos_ms[1] + 1400);
    cue_ms[1] = pos_ms[1] - 2600;
    dj_ui_set_cue_point(1, true, cue_ms[1]);
    for (uint8_t d = 0; d < 2; d++) {
        dj_ui_set_transport(d, playing[d], false);
        dj_ui_set_master_tempo(d, mt_on[d]);
    }

    push_library();
    dj_ui_library_set_info("LOCAL USB", 52, 1, 7);
    dj_ui_library_set_deck_status(0, "READY");
    dj_ui_library_set_status("SORT: LOCAL ONLY", DJ_TONE_MUTED);
    push_fx();

    dj_ui_set_field(DJ_F_SYS_CONTROLLER, "Controller (USB1): DDJ-400 connected", DJ_TONE_OK);
    dj_ui_set_field(DJ_F_MIX_MIXER, "MIXER: DDJ-400", DJ_TONE_NORMAL);
    dj_ui_set_field(DJ_F_MIX_CUE, "CUE: DDJ-400", DJ_TONE_NORMAL);
    dj_ui_set_field(DJ_F_OUT_CUE, "CUE: USB", DJ_TONE_INFO);
    dj_ui_set_field(DJ_F_SYS_FW, "P4: 249 [ota_0]", DJ_TONE_OK);
    push_settings();
    push_link();
    dj_ui_set_status("D1 PLAY", DJ_TONE_NORMAL);

    load_timer = lv_timer_create(load_step, 100, NULL);
    lv_timer_pause(load_timer);
    tick_timer = lv_timer_create(tick, 33, NULL);
}

void dj_ui_demo_stop(void)
{
    lv_timer_delete(load_timer);
    lv_timer_delete(tick_timer);
    load_timer = tick_timer = NULL;
}

uint32_t dj_ui_demo_position(uint8_t deck) { return deck < 2 ? pos_ms[deck] : 0; }
bool dj_ui_demo_playing(uint8_t deck) { return deck < 2 && playing[deck]; }
