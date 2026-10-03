// dj_ui presentation bridge, UI migration phase 1. See ui_djui_bridge.h.
#include "ui_djui_bridge.h"
#include "lvgl_private.h"   /* lv_display_t inv_areas: perf counter, direct strips */
#include "ui_djui_text.h"
#include "ui_overview_motion.h"
#include "ui_overview_palette.h"
#include "ui_overview_wave_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#else
#include <time.h>
#endif

typedef struct {
    bool init;
    bool loaded;
    char title[96];
    char artist[64];
    uint16_t bpm;
    float tempo_pct;
    uint32_t duration_ms;
    uint32_t position_ms;
    bool playing;
    const uint8_t *wave_src;           /* waveform_low rendered into img */
    uint32_t wave_sum;                 /* cheap content check: same buffer, new track */
    int32_t wave_w;                    /* mini columns the PWAV preview spans (v271) */
    const uint8_t *color_src;          /* v313: PWV4 rendered into img, NULL = mono */
    uint32_t color_len;
    uint32_t color_sum;                /* same buffer address, another track */
    uint16_t *px;
    int32_t w, h;
    lv_image_dsc_t img;
    /* Overview (phase 5) */
    uint32_t bpm_x100;
    char key[8];
    int8_t vu_level;                   /* legacy 15-segment level, -1 = unset */
    ui_overview_wave_cache_t cache;    /* zoom ring strip, bound at create */
    uint16_t *strip;
    int32_t zw, zh;
    lv_image_dsc_t strip_img;
    bool strip_shown;
    /* v287 direct strips (ui_djui_bridge_set_direct_blit) */
    lv_area_t za;                      /* zoom strip area, screen coords */
    int32_t src_x;                     /* ring column at the view's left edge, as pushed */
    bool direct_on;                    /* zoom is EXTERNAL: post_refresh paints it */
    bool direct_dirty;                 /* strip, markers or screen changed since the blit */
    uint8_t direct_waits;              /* blits put off for a pending LVGL refresh */
    dj_ui_zoom_marks_t marks;          /* markers the last blit burned */
    uint32_t art_key;                  /* last pushed thumbnail */
    const uint16_t *art;
} bridge_deck_t;

static bridge_deck_t s_deck[DJ_DECKS];
static uint32_t s_status_until_ms;
static bool s_status_timed;
static const uint16_t s_palette[] = UI_OVERVIEW_WAVE_RGB565_PALETTE;
static uint32_t s_budget_px = UI_DJUI_FRAME_BUDGET_PX;
static uint8_t s_strip_first;          /* deck served first, alternating */
static bool s_covered_saver, s_covered_blackout;  /* Overview hidden under a full-screen layer */
static ui_djui_bridge_perf_t s_perf;
static lv_display_t *s_disp;
static ui_djui_direct_blit_t s_direct_blit;
static uint16_t *s_mark_save;          /* ring columns under the burned markers */
static size_t s_mark_save_px;
static ui_djui_fx_view_t s_fx;          /* last pushed, name in s_fx_name */
static char s_fx_name[12];
static bool s_fx_pushed;

static void *bridge_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    /* Once at create; PSRAM keeps it away from the scarce internal RAM.
     * v287: cache-line aligned, the zoom strips are PPA sources. */
    return heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

/* Perf counters only. */
static int64_t bridge_now_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    return 0;
#endif
}

static void perf_max(uint32_t *max, int64_t since_us)
{
    int64_t us = bridge_now_us() - since_us;
    if (us > (int64_t)*max) *max = (uint32_t)us;
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x001fu));
}

static uint32_t wave_sum(const uint8_t *w)
{
    uint32_t s = 0;
    for (int i = 0; i < UI_DJUI_WAVEFORM_LOW_LEN; i++) s = s * 31u + w[i];
    return s;
}

/* v271: the waveforms cover the analysis span; a file that runs past it
 * keeps its tail blank rather than stretching them over the track length. */
static uint32_t wave_span_ms(const ui_djui_deck_view_t *v)
{
    return v->wave_span_ms > 0 && v->wave_span_ms < v->duration_ms ? v->wave_span_ms : v->duration_ms;
}

static int32_t mini_wave_w(const bridge_deck_t *b, const ui_djui_deck_view_t *v)
{
    if (!v->loaded || !v->duration_ms) return b->w;
    return (int32_t)((uint64_t)b->w * wave_span_ms(v) / v->duration_ms);
}

/* Full-track mini waveform from the 400-column PWAV preview, bottom-anchored
 * like the legacy mini canvas, over the first wave_w columns. Runs on track
 * change only. */
static void render_mini(uint8_t deck, const uint8_t *wave, int32_t wave_w,
                        const uint8_t *color, uint32_t color_len)
{
    bridge_deck_t *b = &s_deck[deck];
    if (!b->px) return;
    const uint16_t bg = rgb565(0x08090b), fg = rgb565(deck ? 0xee8fd0 : 0x4fd1a8);
    /* v313: rekordbox's PWV4 colour preview when the track has one: column
     * height from its byte 0 (scaled to the track's loudest), colour from
     * its bass / mid / treble mix (anlz_color_preview_column). */
    const uint8_t peak = color ? anlz_color_preview_peak(color, color_len) : 0u;
    for (int32_t x = 0; x < b->w; x++) {
        int32_t hh = 0;
        uint16_t c = fg;
        anlz_color_column_t cc;
        if (peak && x < wave_w &&
            anlz_color_preview_column(color, color_len, (uint32_t)x, (uint32_t)wave_w, &cc)) {
            hh = cc.height * (b->h - 4) / peak;
            if (cc.r | cc.g | cc.b) {
                c = rgb565(((uint32_t)cc.r << 16) | ((uint32_t)cc.g << 8) | cc.b);
            }
        } else if (wave && x < wave_w) {
            int32_t col = x * UI_DJUI_WAVEFORM_LOW_LEN / wave_w;
            hh = (wave[col] & 0x1f) * (b->h - 4) / 31;
        }
        for (int32_t y = 0; y < b->h; y++) {
            b->px[y * b->w + x] = (y >= b->h - 2 - hh && y < b->h - 2) ? c : bg;
        }
    }
    /* same descriptor, new pixels: drop and re-set so dj_ui invalidates */
    dj_ui_wave_set_image(deck, DJ_WAVE_MINI, NULL, 0);
    dj_ui_wave_set_image(deck, DJ_WAVE_MINI, &b->img, 0);
}

/* Invalidated pixels per LVGL refresh, summed over the areas (an upper bound:
 * LVGL merges overlaps before drawing). */
static void perf_disp_cb(lv_event_t *e)
{
    /* RENDER_START fires once per refresh after the areas were joined: what
     * is left is exactly what LVGL renders (INVALIDATE_AREA would also count
     * areas later dropped as contained, and rounding probes). */
    if (lv_event_get_code(e) != LV_EVENT_RENDER_START) return;
    const lv_display_t *disp = lv_event_get_target(e);
    uint32_t px = 0;
    for (uint32_t i = 0; i < disp->inv_p; i++) {
        if (!disp->inv_area_joined[i]) px += (uint32_t)lv_area_get_size(&disp->inv_areas[i]);
    }
    s_perf.inv_px_last = px;
    if (px > s_perf.inv_px_max) s_perf.inv_px_max = px;
}

static void image_dsc(lv_image_dsc_t *img, uint16_t *px, int32_t w, int32_t h)
{
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_RGB565;
    img->header.w = (uint32_t)w;
    img->header.h = (uint32_t)h;
    img->header.stride = (uint32_t)w * sizeof(uint16_t);
    img->data = (const uint8_t *)px;
    img->data_size = (uint32_t)(w * h * (int32_t)sizeof(uint16_t));
}

/* Zoom: our ANLZ wave cache as a ring strip one view wide plus a scroll margin
 * each side. It burns the beat grid, hot cues and the active loop; dj_ui adds
 * the playhead, the cue-point triangle and the armed-loop trail on top. */
static bool create_strip(uint8_t d)
{
    bridge_deck_t *b = &s_deck[d];
    dj_ui_wave_set_source(d, DJ_WAVE_ZOOM, DJ_WAVE_SRC_IMAGE);
    dj_ui_wave_set_marks(d, DJ_WAVE_ZOOM, DJ_MARK_PLAYHEAD | DJ_MARK_CUE_POINT | DJ_MARK_LOOP_ARMED);
    lv_area_t a;
    if (!dj_ui_wave_get_area(d, DJ_WAVE_ZOOM, &a)) return false;
    b->za = a;
    b->zw = lv_area_get_width(&a);
    b->zh = lv_area_get_height(&a);
    int32_t sw = b->zw + 2 * UI_OVERVIEW_WAVE_CACHE_MARGIN_PX;
    b->strip = bridge_alloc((size_t)sw * (size_t)b->zh * sizeof(uint16_t));
    if (!b->strip) return false;
    memset(b->strip, 0, (size_t)sw * (size_t)b->zh * sizeof(uint16_t));
    image_dsc(&b->strip_img, b->strip, sw, b->zh);
    if (!ui_overview_wave_cache_bind_strip(&b->cache, b->strip, sw, sw, b->zw, b->zh,
                                           UI_OVERVIEW_WAVE_CACHE_MARGIN_PX, s_palette,
                                           sizeof s_palette / sizeof s_palette[0])) {
        return false;
    }
    /* downbeat caps on each deck's inner edge, as the stacked legacy strips */
    ui_overview_wave_cache_set_regular_beat_cap_bottom(&b->cache, d == 0);
    /* v286: rebuild the view first, the margins over the next frames */
    ui_overview_wave_cache_set_progressive(&b->cache, true);
    s_perf.zoom_w = (uint32_t)b->zw;
    s_perf.zoom_h = (uint32_t)b->zh;
    return true;
}

bool ui_djui_bridge_create(lv_obj_t *parent)
{
    bool ok = true;
    dj_ui_create(parent);
    lv_obj_update_layout(parent);
    memset(s_deck, 0, sizeof(s_deck));
    ui_djui_bridge_settings_invalidate();
    ui_djui_bridge_hotcues_invalidate();
    ui_djui_bridge_library_invalidate();
    s_strip_first = 0;
    s_fx_pushed = false;
    memset(&s_perf, 0, sizeof s_perf);
    lv_display_t *disp = lv_obj_get_display(parent);
    if (disp && disp != s_disp) {
        lv_display_add_event_cb(disp, perf_disp_cb, LV_EVENT_RENDER_START, NULL);
        s_disp = disp;
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        bridge_deck_t *b = &s_deck[d];
        b->vu_level = -1;
        if (!create_strip(d)) ok = false;
        dj_ui_wave_set_source(d, DJ_WAVE_MINI, DJ_WAVE_SRC_IMAGE);
        lv_area_t a;
        if (dj_ui_wave_get_area(d, DJ_WAVE_MINI, &a)) {
            b->w = lv_area_get_width(&a);
            b->h = lv_area_get_height(&a);
        }
        if (b->w > 0 && b->h > 0) {
            b->px = bridge_alloc((size_t)b->w * (size_t)b->h * sizeof(uint16_t));
        }
        if (!b->px) {
            ok = false;
            continue;
        }
        image_dsc(&b->img, b->px, b->w, b->h);
        render_mini(d, NULL, b->w, NULL, 0u);
    }
    /* v287: the columns under the direct strips' markers: the armed-loop
     * shade spans at most half the view, the playhead and cue triangle a few */
    if (!s_mark_save && s_deck[0].strip) {
        s_mark_save_px = (size_t)(s_deck[0].zw / 2 + 32) * (size_t)s_deck[0].zh;
        s_mark_save = bridge_alloc(s_mark_save_px * sizeof(uint16_t));
        if (!s_mark_save) s_mark_save_px = 0;
    }
    return ok;
}

static void update_deck(uint8_t d, const ui_djui_deck_view_t *v)
{
    bridge_deck_t *b = &s_deck[d];
    const char *title = v->loaded && v->title ? v->title : "No Track";
    const char *artist = v->loaded && v->artist ? v->artist : "";
    uint32_t duration = v->loaded ? v->duration_ms : 0;

    if (!b->init || b->loaded != v->loaded || b->duration_ms != duration ||
        strncmp(b->title, title, sizeof(b->title) - 1) != 0 ||
        strncmp(b->artist, artist, sizeof(b->artist) - 1) != 0) {
        snprintf(b->title, sizeof(b->title), "%s", title);
        snprintf(b->artist, sizeof(b->artist), "%s", artist);
        b->loaded = v->loaded;
        b->duration_ms = duration;
        /* Raw copies stay for the per-frame compare; dj_ui gets drawable text. */
        char title_fit[sizeof b->title], artist_fit[sizeof b->artist];
        ui_djui_text_fit(title_fit, sizeof title_fit, b->title);
        ui_djui_text_fit(artist_fit, sizeof artist_fit, b->artist);
        dj_ui_set_track(d, title_fit, artist_fit, "", 0, 0, duration);
    }
    uint32_t art_key = v->loaded ? v->art_key : 0;
    const uint16_t *art = art_key ? v->art : NULL;
    if (!b->init || b->art_key != art_key || b->art != art) {
        b->art_key = art_key;
        b->art = art;
        dj_ui_set_artwork_pixels(d, art);
    }
    const char *key = v->loaded && v->key && v->key[0] ? v->key : "--";
    if (!b->init || strncmp(b->key, key, sizeof(b->key) - 1) != 0) {
        snprintf(b->key, sizeof(b->key), "%s", key);
        dj_ui_set_key(d, b->key);
    }
    /* beat-grid BPM keeps the legacy two decimals; the file BPM is the fallback */
    uint16_t bpm = v->loaded ? v->bpm : 0;
    uint32_t bpm_x100 = v->loaded ? (v->bpm_x100 ? v->bpm_x100 : (uint32_t)bpm * 100u) : 0;
    if (!b->init || b->bpm_x100 != bpm_x100) {
        b->bpm = bpm;
        b->bpm_x100 = bpm_x100;
        dj_ui_set_bpm(d, (float)bpm_x100 / 100.0f, 0);
    }
    if (!b->init || b->tempo_pct != v->tempo_pct) {
        b->tempo_pct = v->tempo_pct;
        dj_ui_set_tempo(d, v->tempo_pct);
    }
    uint32_t pos = v->loaded ? v->position_ms : 0;
    if (!b->init || b->position_ms != pos) {
        b->position_ms = pos;
        dj_ui_set_position(d, pos);
    }
    if (!b->init || b->playing != v->playing) {
        b->playing = v->playing;
        dj_ui_set_transport(d, v->playing, false);
    }
    /* dj_ui drops unchanged values itself */
    dj_ui_set_load_progress(d, v->load_active ? (int16_t)v->load_percent : -1, v->load_db);
    dj_ui_set_master_tempo(d, v->master_tempo);
    dj_ui_set_sync(d, v->sync);
    dj_ui_set_cue_point(d, v->loaded && v->cue_point_set, v->cue_point_ms);
    uint32_t memory_ms[DJ_MEMORY_CUES];
    uint8_t memory_n = 0;
    for (uint8_t i = 0; v->loaded && v->memory && i < v->memory_count && i < DJ_MEMORY_CUES; i++) {
        memory_ms[memory_n++] = v->memory[i].start_ms;
    }
    dj_ui_set_memory_cues(d, memory_ms, memory_n);
    dj_ui_set_loop(d, v->loaded && v->loop_active, v->loop_start_ms, v->loop_end_ms);
    dj_ui_set_loop_armed(d, v->loaded && v->loop_armed, v->loop_armed_ms);
    dj_ui_set_beat(d, v->loaded && v->beat_valid, v->beat_phase, v->beat_downbeat);
    /* legacy meter: 15 segments, any signal lights one, one segment of decay */
    int level = (int)((uint32_t)v->vu_peak * DJ_VU_SEGS / 32768u);
    if (v->vu_peak > 0 && level < 1) level = 1;
    if (level > DJ_VU_SEGS) level = DJ_VU_SEGS;
    if (b->vu_level >= 0 && level < b->vu_level) level = b->vu_level - 1;
    if (level != b->vu_level) {
        b->vu_level = (int8_t)level;
        dj_ui_set_vu(d, (uint8_t)(level * 255 / DJ_VU_SEGS));
    }
    const uint8_t *wave = v->loaded ? v->waveform_low : NULL;
    uint32_t sum = wave ? wave_sum(wave) : 0;
    int32_t wave_w = mini_wave_w(b, v);
    const uint8_t *color = v->loaded && v->meta ? v->meta->color_preview : NULL;
    const uint32_t color_len = color ? v->meta->color_preview_len : 0u;
    uint32_t color_sum = 0u;
    for (uint32_t i = 0; i < color_len; i++) color_sum = color_sum * 31u + color[i];
    if (!b->init || b->wave_src != wave || b->wave_sum != sum || b->wave_w != wave_w ||
        b->color_src != color || b->color_len != color_len || b->color_sum != color_sum) {
        b->color_sum = color_sum;
        b->wave_src = wave;
        b->wave_sum = sum;
        b->wave_w = wave_w;
        b->color_src = color;
        b->color_len = color_len;
        int64_t t0 = bridge_now_us();
        render_mini(d, wave, wave_w, color, color_len);
        s_perf.mini_renders++;
        perf_max(&s_perf.mini_us_max, t0);
    }
    b->init = true;
}

static bool strip_has_source(const bridge_deck_t *b, const ui_djui_deck_view_t *v)
{
    return b->strip && v->loaded && v->wave.kind != UI_WAVEFORM_SOURCE_NONE && v->wave.samples &&
           v->wave.sample_count && v->duration_ms && v->window_ms;
}

/* Feeds the burned markers (a change flips the cache invalid) and tells
 * whether ui_overview_wave_cache_update has work. */
static bool strip_pending(bridge_deck_t *b, const ui_djui_deck_view_t *v, uint32_t center_ms)
{
    ui_overview_wave_cache_t *c = &b->cache;
    ui_overview_wave_cache_set_loop(c, v->loop_active, v->loop_start_ms, v->loop_end_ms);
    ui_overview_wave_cache_set_cues(c, v->cues, v->cues ? v->cue_count : 0);
    ui_overview_wave_cache_set_memory_cues(c, v->memory, v->memory ? v->memory_count : 0);
    return !c->valid || ui_overview_wave_cache_filling(c) || c->center_ms != center_ms || c->window_ms != v->window_ms ||
           c->duration_ms != wave_span_ms(v) || c->source_samples != v->wave.samples ||
           c->source_sample_count != v->wave.sample_count || c->source_kind != v->wave.kind;
}

static void strip_push(uint8_t d)
{
    bridge_deck_t *b = &s_deck[d];
    const ui_overview_wave_cache_t *c = &b->cache;
    int32_t src_x = (c->ring_head_px + c->view_origin_px) % c->strip_width_px;
    if (src_x < 0) src_x += c->strip_width_px;
    /* the time under the view centre as the cache rendered it */
    int64_t center_q16 = c->strip_start_ms_q16 +
                         (int64_t)(c->view_origin_px + c->view_width_px / 2) * c->ms_per_px_q16;
    uint32_t center = center_q16 <= 0 ? 0u : (uint32_t)((center_q16 + 32768) >> 16);
    b->src_x = src_x;
    b->strip_shown = true;
    if (s_direct_blit) {
        /* v287: LVGL neither draws nor invalidates it; post_refresh blits */
        if (!b->direct_on) dj_ui_wave_set_source(d, DJ_WAVE_ZOOM, DJ_WAVE_SRC_EXTERNAL);
        b->direct_on = true;
        b->direct_dirty = true;
        dj_ui_wave_set_strip(d, &b->strip_img, src_x, center);
        return;
    }
    dj_ui_wave_set_strip(d, &b->strip_img, src_x, center);
    /* same src_x/centre after a rebuild (cue or loop change): new pixels */
    dj_ui_wave_invalidate(d, DJ_WAVE_ZOOM);
}

static void strip_clear(uint8_t d)
{
    bridge_deck_t *b = &s_deck[d];
    if (!b->strip_shown) return;
    b->strip_shown = false;
    ui_overview_wave_cache_reset(&b->cache);
    dj_ui_wave_set_strip(d, NULL, 0, 0);
    if (b->direct_on) {
        /* back to LVGL, which paints the empty surface */
        b->direct_on = false;
        b->direct_dirty = false;
        dj_ui_wave_set_source(d, DJ_WAVE_ZOOM, DJ_WAVE_SRC_IMAGE);
    }
}

/* Zoom strips under the frame budget: a deck whose update would pass it waits
 * for the next frame; the order alternates so neither deck starves. */
static void update_strips(const ui_djui_frame_t *frame)
{
    uint32_t used = 0;
    bool full_done = false;
    s_perf.updates++;
    for (uint8_t k = 0; k < DJ_DECKS; k++) {
        uint8_t d = (uint8_t)((s_strip_first + k) % DJ_DECKS);
        bridge_deck_t *b = &s_deck[d];
        const ui_djui_deck_view_t *v = &frame->deck[d];
        if (!strip_has_source(b, v)) {
            strip_clear(d);
            continue;
        }
        dj_ui_wave_set_window_ms(d, v->window_ms);
        /* whole-pixel steps, as the legacy zoom: no subpixel re-render */
        uint32_t center = v->center_ms > v->duration_ms ? v->duration_ms : v->center_ms;
        center = ui_overview_motion_snap_center_ms(center, v->window_ms, (int)b->zw);
        if (center > v->duration_ms) center = v->duration_ms;
        if (!strip_pending(b, v, center)) continue;
        uint32_t cost = (uint32_t)(b->zw * b->zh);
        if (used > 0 && used + cost > s_budget_px) {
            s_perf.strip_deferred++;
            continue;
        }
        /* v285: two rebuilds in one frame (tab return, both decks loaded)
         * doubled the frame. The other deck rebuilds next frame, first in
         * the alternating order. v286: a rebuild renders the view only. */
        bool full = ui_overview_wave_cache_needs_full(&b->cache, &v->wave, wave_span_ms(v), v->meta,
                                                      center, v->window_ms);
        if (full && full_done) {
            s_perf.strip_deferred++;
            s_perf.strip_full_deferred++;
            continue;
        }
        ui_overview_wave_cache_report_t report;
        int64_t t0 = bridge_now_us();
        bool updated = ui_overview_wave_cache_update(&b->cache, &v->wave, wave_span_ms(v), v->meta,
                                                     center, v->window_ms, &report);
        if (updated && report.kind == UI_OVERVIEW_WAVE_CACHE_FULL) {
            full_done = true;
            s_perf.strip_full++;
            perf_max(&s_perf.strip_full_us_max, t0);
        } else if (updated && report.kind == UI_OVERVIEW_WAVE_CACHE_FILL) {
            s_perf.strip_fill++;
            perf_max(&s_perf.strip_fill_us_max, t0);
        }
        if (!updated || !report.blit_required) continue;
        strip_push(d);
        used += cost;
        s_perf.strip_redraws++;
    }
    s_strip_first = (uint8_t)((s_strip_first + 1) % DJ_DECKS);
    s_perf.strip_px_last = used;
    if (used > s_perf.strip_px_max) s_perf.strip_px_max = used;
}

void ui_djui_bridge_set_frame_budget_px(uint32_t px)
{
    s_budget_px = px ? px : UI_DJUI_FRAME_BUDGET_PX;
}

void ui_djui_bridge_get_perf(ui_djui_bridge_perf_t *out)
{
    if (out) *out = s_perf;
}

void ui_djui_bridge_reset_perf(void)
{
    uint32_t w = s_perf.zoom_w, h = s_perf.zoom_h;
    memset(&s_perf, 0, sizeof s_perf);
    s_perf.zoom_w = w;
    s_perf.zoom_h = h;
}

static void update_fx(const ui_djui_fx_view_t *v)
{
    const char *name = v->name ? v->name : "-";
    if (s_fx_pushed && strncmp(s_fx_name, name, sizeof s_fx_name) == 0 && s_fx.channel == v->channel &&
        s_fx.beat_index == v->beat_index && s_fx.time_ms == v->time_ms && s_fx.level_pct == v->level_pct &&
        s_fx.on == v->on) {
        return;
    }
    s_fx = *v;
    snprintf(s_fx_name, sizeof s_fx_name, "%s", name);
    s_fx_pushed = true;
    dj_ui_set_fx(s_fx_name, v->channel, v->beat_index, v->time_ms, v->level_pct, v->on);
}

void ui_djui_bridge_update(const ui_djui_frame_t *frame)
{
    if (!frame) return;
    int64_t t0 = bridge_now_us();
    update_fx(&frame->fx);
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        /* Every frame, hidden or not: the caller drops its old ANLZ snapshot
         * once the new one is here, so a later snapshot may reuse the address. */
        if (s_deck[d].cache.meta != frame->deck[d].meta) ui_overview_wave_cache_reset(&s_deck[d].cache);
        update_deck(d, &frame->deck[d]);
    }
    perf_max(&s_perf.decks_us_max, t0);
    if (frame->overview_visible && !s_covered_saver && !s_covered_blackout) {
        t0 = bridge_now_us();
        update_strips(frame);
        perf_max(&s_perf.strips_us_max, t0);
    }
    if (s_status_timed && (int32_t)(frame->now_ms - s_status_until_ms) >= 0) {
        s_status_timed = false;
        dj_ui_set_status(NULL, DJ_TONE_MUTED);
        dj_ui_library_set_status(NULL, DJ_TONE_MUTED);
    }
}

void ui_djui_bridge_status_hold(const char *text, dj_tone_t tone, uint32_t hold_ms, uint32_t now_ms)
{
    /* Holds may carry a DJ Link player name. */
    char fit[64];
    if (text) {
        ui_djui_text_fit(fit, sizeof fit, text);
        text = fit;
    }
    dj_ui_set_status(text, tone);
    /* "LOAD BUSY", "SORT: LOCAL ONLY"... also land next to the LOAD buttons. */
    dj_ui_library_set_status(text, tone);
    s_status_timed = hold_ms != 0;
    s_status_until_ms = now_ms + hold_ms;
}

void ui_djui_bridge_set_screensaver(bool on)
{
    s_covered_saver = on;
    dj_ui_set_screensaver(on);
}

/* ---- v287 direct zoom strips ----
 * v286 left LVGL copying both 696x119 strips through its draw buffer every
 * frame (~41 ms per refresh on the P4). Now the zoom is an EXTERNAL surface:
 * after each LVGL refresh the view goes from the ring straight to the
 * framebuffer, the markers dj_ui would draw burned into the ring columns for
 * the copy and put back after it. */

typedef struct { int32_t x0, x1; } col_span_t;

/* View columns [x0, x1] as runs of ring columns; returns the run count. */
static int ring_runs(const bridge_deck_t *b, int32_t x0, int32_t x1, int32_t rx[2], int32_t rn[2])
{
    int32_t sw = b->cache.strip_width_px, n = x1 - x0 + 1;
    rx[0] = (b->src_x + x0) % sw;
    rn[0] = LV_MIN(n, sw - rx[0]);
    if (rn[0] == n) return 1;
    rx[1] = 0;
    rn[1] = n - rn[0];
    return 2;
}

/* View columns [x0, x1], rows [y0, y1] filled with c at opa, as LVGL's fill. */
static void strip_fill(bridge_deck_t *b, int32_t x0, int32_t x1, int32_t y0, int32_t y1, uint16_t c, uint8_t opa)
{
    x0 = LV_MAX(x0, 0);
    x1 = LV_MIN(x1, b->zw - 1);
    y0 = LV_MAX(y0, 0);
    y1 = LV_MIN(y1, b->zh - 1);
    if (x0 > x1 || y0 > y1) return;
    int32_t rx[2], rn[2];
    int runs = ring_runs(b, x0, x1, rx, rn);
    for (int32_t y = y0; y <= y1; y++) {
        uint16_t *row = b->strip + (size_t)y * (size_t)b->cache.stride_px;
        for (int k = 0; k < runs; k++) {
            uint16_t *p = row + rx[k];
            for (int32_t i = 0; i < rn[k]; i++) p[i] = opa >= LV_OPA_MAX ? c : lv_color_16_16_mix(c, p[i], opa);
        }
    }
}

/* Copies the full-height ring columns under disjoint spans to s_mark_save, or
 * back from it. */
static void strip_keep(bridge_deck_t *b, const col_span_t *s, int n, bool restore)
{
    uint16_t *buf = s_mark_save;
    for (int j = 0; j < n; j++) {
        int32_t rx[2], rn[2];
        int runs = ring_runs(b, s[j].x0, s[j].x1, rx, rn);
        for (int32_t y = 0; y < b->zh; y++) {
            uint16_t *row = b->strip + (size_t)y * (size_t)b->cache.stride_px;
            for (int k = 0; k < runs; k++) {
                size_t bytes = (size_t)rn[k] * sizeof(uint16_t);
                if (restore) memcpy(row + rx[k], buf, bytes);
                else memcpy(buf, row + rx[k], bytes);
                buf += rn[k];
            }
        }
    }
}

/* Columns the markers touch, clipped, sorted and merged; returns the count. */
static int mark_spans(const bridge_deck_t *b, const dj_ui_zoom_marks_t *m, col_span_t s[3], size_t *cols)
{
    col_span_t t[3];
    int n = 0, out = 0;
    if (m->armed_x1 <= m->armed_x2) t[n++] = (col_span_t){ m->armed_x1, m->armed_x2 };
    if (m->playhead_x1 <= m->playhead_x2) t[n++] = (col_span_t){ m->playhead_x1, m->playhead_x2 };
    if (m->cue_x != INT32_MIN) t[n++] = (col_span_t){ m->cue_x - m->cue_half_w, m->cue_x + m->cue_half_w };
    for (int i = 0; i < n; i++) {
        t[i].x0 = LV_MAX(t[i].x0, 0);
        t[i].x1 = LV_MIN(t[i].x1, b->zw - 1);
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && t[j].x0 < t[j - 1].x0; j--) {
            col_span_t x = t[j];
            t[j] = t[j - 1];
            t[j - 1] = x;
        }
    }
    *cols = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].x0 > t[i].x1) continue;
        if (out > 0 && t[i].x0 <= s[out - 1].x1 + 1) {
            s[out - 1].x1 = LV_MAX(s[out - 1].x1, t[i].x1);
        } else {
            s[out++] = t[i];
        }
    }
    for (int i = 0; i < out; i++) *cols += (size_t)(s[i].x1 - s[i].x0 + 1);
    return out;
}

static bool direct_paint(uint8_t d)
{
    bridge_deck_t *b = &s_deck[d];
    dj_ui_zoom_marks_t m = b->marks;
    col_span_t s[3];
    size_t cols;
    int n = mark_spans(b, &m, s, &cols);
    if (cols * (size_t)b->zh > s_mark_save_px) {
        /* cannot happen with dj_ui's geometry; the waveform still goes out */
        m.armed_x2 = m.armed_x1 - 1;
        m.armed_line = -1;
        n = mark_spans(b, &m, s, &cols);
    }
    strip_keep(b, s, n, false);
    /* zoom_draw's order: armed shade, loop-in line, playhead, cue triangle */
    strip_fill(b, m.armed_x1, m.armed_x2, 0, b->zh - 1, m.armed_color, m.armed_opa);
    if (m.armed_line >= 0) strip_fill(b, m.armed_line, m.armed_line, 0, b->zh - 1, m.line_color, LV_OPA_COVER);
    strip_fill(b, m.playhead_x1, m.playhead_x2, 0, b->zh - 1, m.playhead_color, LV_OPA_COVER);
    if (m.cue_x != INT32_MIN) {
        for (int32_t r = 0; r < m.cue_h && m.cue_half_w - r >= 0; r++) {
            strip_fill(b, m.cue_x - m.cue_half_w + r, m.cue_x + m.cue_half_w - r, r, r, m.cue_color, LV_OPA_COVER);
        }
    }
    int32_t sw = b->cache.strip_width_px, w1 = LV_MIN(b->zw, sw - b->src_x);
    bool ok = s_direct_blit(b->za.x1, b->za.y1, b->strip, sw, b->zh, b->src_x, w1);
    if (ok && w1 < b->zw) ok = s_direct_blit(b->za.x1 + w1, b->za.y1, b->strip, sw, b->zh, 0, b->zw - w1);
    strip_keep(b, s, n, true);
    return ok;
}

/* An LVGL refresh still due over the strip paints its background there and
 * flags a repaint anyway: blit after it rather than twice. */
static bool refresh_pending_over(const lv_area_t *a)
{
    if (!s_disp) return false;
    for (uint32_t i = 0; i < s_disp->inv_p; i++) {
        const lv_area_t *r = &s_disp->inv_areas[i];
        if (r->x1 <= a->x2 && r->x2 >= a->x1 && r->y1 <= a->y2 && r->y2 >= a->y1) return true;
    }
    return false;
}

void ui_djui_bridge_set_direct_blit(ui_djui_direct_blit_t blit)
{
    if (!s_mark_save) blit = NULL;
    s_direct_blit = blit;
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        bridge_deck_t *b = &s_deck[d];
        if (!b->strip_shown || b->direct_on == (blit != NULL)) continue;
        b->direct_on = blit != NULL;
        b->direct_dirty = b->direct_on;
        dj_ui_wave_set_source(d, DJ_WAVE_ZOOM, b->direct_on ? DJ_WAVE_SRC_EXTERNAL : DJ_WAVE_SRC_IMAGE);
    }
}

bool ui_djui_bridge_direct_area(uint8_t deck, lv_area_t *out)
{
    if (deck >= DJ_DECKS || !out || !s_deck[deck].strip) return false;
    *out = s_deck[deck].za;
    return true;
}

void ui_djui_bridge_post_refresh(uint32_t repainted)
{
    for (uint8_t d = 0; d < DJ_DECKS && s_direct_blit; d++) {
        bridge_deck_t *b = &s_deck[d];
        if (!b->direct_on) continue;
        if (repainted & (1u << d)) {
            b->direct_dirty = true;
            s_perf.direct_repaints++;
        }
        dj_ui_zoom_marks_t m;
        if (dj_ui_wave_zoom_marks(d, &m) && memcmp(&m, &b->marks, sizeof m) != 0) {
            b->marks = m;
            b->direct_dirty = true;
        }
        if (!b->direct_dirty || !dj_ui_wave_exposed(d, DJ_WAVE_ZOOM)) continue;
        /* bounded: a timer invalidating over the strip every cycle must not
         * keep it off screen */
        if (b->direct_waits < 2 && refresh_pending_over(&b->za)) {
            b->direct_waits++;
            continue;
        }
        int64_t t0 = bridge_now_us();
        if (!direct_paint(d)) {
            /* LVGL draws the strips again, for good */
            s_perf.direct_fails++;
            ui_djui_bridge_set_direct_blit(NULL);
            return;
        }
        b->direct_dirty = false;
        b->direct_waits = 0;
        s_perf.direct_blits++;
        perf_max(&s_perf.direct_us_max, t0);
    }
}

/* ---- Settings (phase 2) ---- */

#define FIELD_TEXT_LEN 96
#define PEER_NAME_LEN  24

typedef struct {
    char text[DJ_F_COUNT][FIELD_TEXT_LEN];
    int8_t tone[DJ_F_COUNT];            /* -1 = never pushed */
    int16_t brightness;                 /* -1 = never pushed, same for the tri-states below */
    int8_t wireless, link_enabled, recording, blackout;
    int16_t peer_count;
    dj_ui_link_peer_t peers[DJ_LINK_ROWS];
    char peer_name[DJ_LINK_ROWS][PEER_NAME_LEN];
    int8_t master_valid;
    dj_ui_link_master_t master;
} bridge_settings_t;

static bridge_settings_t s_set;

uint8_t ui_djui_bridge_brightness_clamp(uint8_t pct)
{
    if (pct < UI_DJUI_BRIGHTNESS_MIN) return UI_DJUI_BRIGHTNESS_MIN;
    return pct > 100u ? 100u : pct;
}

void ui_djui_bridge_settings_invalidate(void)
{
    memset(&s_set, 0, sizeof(s_set));
    memset(s_set.tone, -1, sizeof(s_set.tone));
    s_set.brightness = -1;
    s_set.wireless = s_set.link_enabled = s_set.recording = s_set.blackout = -1;
    s_set.peer_count = -1;
    s_set.master_valid = -1;
}

static void field(dj_field_t f, const char *text, dj_tone_t tone)
{
    if (s_set.tone[f] == (int8_t)tone && strncmp(s_set.text[f], text, FIELD_TEXT_LEN - 1) == 0) return;
    snprintf(s_set.text[f], FIELD_TEXT_LEN, "%s", text);
    s_set.tone[f] = (int8_t)tone;
    dj_ui_set_field(f, s_set.text[f], tone);
}

static bool flag_changed(int8_t *cache, bool v)
{
    if (*cache == (int8_t)v) return false;
    *cache = (int8_t)v;
    return true;
}

/* "12.3 GB" / "512.0 MB", as the legacy Settings screen. */
static void format_size(uint64_t bytes, char *out, size_t cap)
{
    const uint64_t gib = 1024ull * 1024ull * 1024ull, mib = 1024ull * 1024ull;
    uint64_t scale = bytes >= gib ? gib : mib;
    snprintf(out, cap, "%llu.%llu %s", (unsigned long long)(bytes / scale),
             (unsigned long long)(((bytes % scale) * 10ull) / scale), bytes >= gib ? "GB" : "MB");
}

static void update_link(const ui_djui_settings_view_t *v)
{
    if (flag_changed(&s_set.link_enabled, v->link_enabled)) dj_ui_set_link_enabled(v->link_enabled);
    field(DJ_F_LINK_STATUS, v->link_status ? v->link_status : (v->link_enabled ? "DJ LINK: ON" : "DJ LINK: OFF"),
          v->link_tone);

    uint8_t count = v->link_peer_count < DJ_LINK_ROWS ? v->link_peer_count : DJ_LINK_ROWS;
    bool peers_changed = s_set.peer_count != count;
    for (uint8_t r = 0; r < count && !peers_changed; r++) {
        const dj_ui_link_peer_t *a = &v->link_peers[r], *b = &s_set.peers[r];
        peers_changed = a->number != b->number || a->bpm != b->bpm || a->master != b->master ||
                        a->on_air != b->on_air || a->playing != b->playing ||
                        strncmp(a->name ? a->name : "", s_set.peer_name[r], PEER_NAME_LEN - 1) != 0;
    }
    if (peers_changed) {
        s_set.peer_count = count;
        for (uint8_t r = 0; r < count; r++) {
            s_set.peers[r] = v->link_peers[r];
            snprintf(s_set.peer_name[r], PEER_NAME_LEN, "%s", v->link_peers[r].name ? v->link_peers[r].name : "");
            s_set.peers[r].name = s_set.peer_name[r];
        }
        dj_ui_set_link_peers(s_set.peers, count);
    }

    const dj_ui_link_master_t *m = &v->link_master, *c = &s_set.master;
    if (s_set.master_valid != (int8_t)v->link_master_valid ||
        (v->link_master_valid && (m->player != c->player || m->bpm != c->bpm ||
                                  m->beat != c->beat || m->confirmed != c->confirmed))) {
        s_set.master_valid = (int8_t)v->link_master_valid;
        s_set.master = *m;
        dj_ui_set_link_master(v->link_master_valid ? &s_set.master : NULL);
    }
}

void ui_djui_bridge_settings_update(const ui_djui_settings_view_t *v)
{
    if (!v) return;
    char buf[FIELD_TEXT_LEN];

    uint8_t pct = ui_djui_bridge_brightness_clamp(v->brightness_pct);
    if (s_set.brightness != pct) {
        s_set.brightness = pct;
        dj_ui_set_brightness(pct);
    }
    if (flag_changed(&s_set.wireless, v->wireless_on)) dj_ui_set_wireless(v->wireless_on);
    if (flag_changed(&s_set.blackout, v->blackout_now)) {
        s_covered_blackout = v->blackout_now;
        dj_ui_set_blackout(v->blackout_now);
    }

    field(DJ_F_MASTER, v->master_trim ? v->master_trim : "MASTER: 0 dB", DJ_TONE_NORMAL);
    field(DJ_F_OUT_MAIN, v->main_out_usb ? "MAIN: USB (DDJ)" : "MAIN: PCM5102A RCA",
          v->main_out_usb ? DJ_TONE_INFO : DJ_TONE_OK);
    field(DJ_F_UI_RENDER, v->ui_blackout_play ? "TEST: UI OFF on PLAY" : "UI render: normal",
          v->ui_blackout_play ? DJ_TONE_WARN : DJ_TONE_NORMAL);
    field(DJ_F_LOCAL, v->local_monitor ? "LOCAL: ES8311 monitor" : "LOCAL: disabled", DJ_TONE_MUTED);

    const char *name = v->controller_name && v->controller_name[0] ? v->controller_name : "USB";
    snprintf(buf, sizeof buf, "CUE: %s", name);
    field(DJ_F_OUT_CUE, buf, DJ_TONE_INFO);
    snprintf(buf, sizeof buf, "MIXER: %s", name);
    field(DJ_F_MIX_MIXER, buf, DJ_TONE_NORMAL);
    field(DJ_F_MIX_CUE, v->cue_mode ? v->cue_mode : "CUE: STEREO", DJ_TONE_NORMAL);
    field(DJ_F_MIX_JOG, v->jog_cdj ? "JOG: CDJ" : "JOG: VINYL", DJ_TONE_NORMAL);
    field(DJ_F_LOAD_LOCK, v->load_lock ? "LOAD LOCK: ON" : "LOAD LOCK: OFF",
          v->load_lock ? DJ_TONE_WARN : DJ_TONE_NORMAL);
    field(DJ_F_LINK_SYNC, v->link_sync ? "LINK SYNC: ON" : "LINK SYNC: OFF",
          v->link_sync ? DJ_TONE_INFO : DJ_TONE_NORMAL);
    snprintf(buf, sizeof buf, "TEMPO: +/-%u%%",
             (unsigned)(v->tempo_range_pct ? v->tempo_range_pct : 10u));
    field(DJ_F_MIX_TEMPO, buf, DJ_TONE_NORMAL);
    field(DJ_F_SYS_CONTROLLER,
          v->controller_connected ? "Controller (USB1): Connected" : "Controller (USB1): Disconnected",
          v->controller_connected ? DJ_TONE_OK : DJ_TONE_ERROR);

    if (v->sd_state == UI_DJUI_SD_MOUNTED) {
        char free_s[24], total_s[24];
        format_size(v->sd_free_bytes, free_s, sizeof free_s);
        format_size(v->sd_total_bytes, total_s, sizeof total_s);
        snprintf(buf, sizeof buf, "Mounted: %s free / %s", free_s, total_s);
        field(DJ_F_SYS_SD, buf, DJ_TONE_OK);
    } else if (v->sd_state == UI_DJUI_SD_OFFLINE) {
        field(DJ_F_SYS_SD, "Offline (/sd unavailable)", DJ_TONE_ERROR);
    } else {
        field(DJ_F_SYS_SD, "Checking /sd...", DJ_TONE_MUTED);
    }
    if (v->sd_log_valid) {
        snprintf(buf, sizeof buf, "SD Log: %s  %luKB  drop %lu", v->sd_log_available ? "OK" : "off",
                 (unsigned long)v->sd_log_kb, (unsigned long)v->sd_log_dropped);
        field(DJ_F_SYS_SD_LOG, buf, v->sd_log_dropped > 0u ? DJ_TONE_ERROR : DJ_TONE_MUTED);
    } else {
        field(DJ_F_SYS_SD_LOG, "SD Log: --", DJ_TONE_MUTED);
    }
    if (v->fw_version) {
        snprintf(buf, sizeof buf, "P4: %s [%s]", v->fw_version, v->fw_partition ? v->fw_partition : "?");
        field(DJ_F_SYS_FW, buf, DJ_TONE_OK);
    } else {
        field(DJ_F_SYS_FW, "P4: firmware status unavailable", DJ_TONE_WARN);
    }
    if (v->reset_reason) {
        snprintf(buf, sizeof buf, "Last reset: %s", v->reset_reason);
        field(DJ_F_SYS_RESET, buf, v->reset_bad ? DJ_TONE_ERROR : DJ_TONE_MUTED);
    } else {
        field(DJ_F_SYS_RESET, "", DJ_TONE_MUTED);
    }

    update_link(v);

    bool rec_active = v->rec_state == UI_DJUI_REC_ACTIVE;
    if (flag_changed(&s_set.recording, rec_active)) dj_ui_set_recording(rec_active);
    switch (v->rec_state) {
    case UI_DJUI_REC_ACTIVE:
        snprintf(buf, sizeof buf, "REC %02u:%02u  %lu MB", (unsigned)(v->rec_secs / 60u),
                 (unsigned)(v->rec_secs % 60u), (unsigned long)v->rec_mb);
        field(DJ_F_REC_STATUS, buf, DJ_TONE_ERROR);
        break;
    case UI_DJUI_REC_ERROR:
        field(DJ_F_REC_STATUS, "SD error - press to reset", DJ_TONE_ERROR);
        break;
    case UI_DJUI_REC_OFF:
        field(DJ_F_REC_STATUS, "Recorder disabled", DJ_TONE_MUTED);
        break;
    default:
        field(DJ_F_REC_STATUS, "REC --:--", DJ_TONE_MUTED);
        break;
    }
    field(DJ_F_REC_DEST, "-> /sd/recordings", DJ_TONE_MUTED);
}

/* ---- Hot Cues (phase 3) ---- */

typedef struct {
    ui_djui_hotcue_t slot[DJ_DECKS][DJ_HOTCUES];
    bool pushed[DJ_DECKS][DJ_HOTCUES];
    int16_t target;                     /* -1 = never pushed */
} bridge_hotcues_t;

static bridge_hotcues_t s_hc;

void ui_djui_bridge_hotcues_invalidate(void)
{
    memset(&s_hc, 0, sizeof(s_hc));
    s_hc.target = -1;
    for (int f = DJ_F_HC_CUES; f <= DJ_F_HC_TARGET; f++) s_set.tone[f] = -1;
}

static bool hotcue_equal(const ui_djui_hotcue_t *a, const ui_djui_hotcue_t *b)
{
    if (a->set != b->set) return false;
    return !a->set || (a->loop == b->loop && a->pos_ms == b->pos_ms && a->end_ms == b->end_ms);
}

void ui_djui_bridge_hotcues_update(const ui_djui_hotcues_view_t *v)
{
    if (!v) return;
    uint8_t target = v->target < DJ_DECKS ? v->target : 0;
    if (s_hc.target != target) {
        s_hc.target = target;
        dj_ui_set_target(target);
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        for (uint8_t k = 0; k < DJ_HOTCUES; k++) {
            ui_djui_hotcue_t c = v->slot[d][k];
            if (!c.set) c = (ui_djui_hotcue_t){ 0 };
            if (s_hc.pushed[d][k] && hotcue_equal(&s_hc.slot[d][k], &c)) continue;
            s_hc.slot[d][k] = c;
            s_hc.pushed[d][k] = true;
            dj_ui_set_hotcue(d, k, c.set, c.pos_ms, k);
            dj_ui_set_hotcue_loop(d, k, c.set && c.loop);
        }
    }

    /* Status chips follow the target deck, like the Hot Cues cards. */
    uint8_t cues = 0, loops = 0;
    for (uint8_t k = 0; k < DJ_HOTCUES; k++) {
        if (!s_hc.slot[target][k].set) continue;
        cues++;
        if (s_hc.slot[target][k].loop) loops++;
    }
    char buf[FIELD_TEXT_LEN];
    snprintf(buf, sizeof buf, "CUES %u/%u", (unsigned)cues, (unsigned)DJ_HOTCUES);
    field(DJ_F_HC_CUES, buf, cues ? DJ_TONE_OK : DJ_TONE_MUTED);
    snprintf(buf, sizeof buf, "LOOPS %u", (unsigned)loops);
    field(DJ_F_HC_LOOPS, buf, loops ? DJ_TONE_WARN : DJ_TONE_MUTED);
    field(DJ_F_HC_ANLZ, v->anlz[target] ? "ANLZ DATA" : "NO ANLZ", v->anlz[target] ? DJ_TONE_INFO : DJ_TONE_MUTED);
    field(DJ_F_HC_TARGET, target ? "TARGET D2" : "TARGET D1", DJ_TONE_NORMAL);
}

bool ui_djui_bridge_hotcue_get(uint8_t deck, uint8_t index, ui_djui_hotcue_t *out)
{
    if (!out || deck >= DJ_DECKS || index >= DJ_HOTCUES) return false;
    *out = s_hc.slot[deck][index];
    return true;
}

/* ---- Library (v256) ---- */

typedef struct {
    char source[UI_DJUI_LIB_TEXT_LEN];
    char deck_status[16];
    char source_label[16];
    char unit[16];
    char playlists_label[16];
    int32_t total, page, pages;         /* -1 = never pushed, same below */
    int16_t selected, loaded[DJ_DECKS];
    int16_t status_deck;
    int8_t load_enabled;
    int8_t load_locked[DJ_DECKS];
    int16_t progress;                   /* INT16_MIN = never pushed */
    int16_t sort;                       /* dj_sort_t, -1 = never pushed */
    bool sort_desc;
} bridge_library_t;

static bridge_library_t s_lib;

static bool lib_text(char *dst, size_t len, const char *src)
{
    if (!src) src = "";
    if (strncmp(dst, src, len - 1) == 0) return false;
    snprintf(dst, len, "%s", src);
    return true;
}

void ui_djui_bridge_library_invalidate(void)
{
    memset(&s_lib, 0, sizeof(s_lib));
    s_lib.total = s_lib.page = s_lib.pages = -1;
    s_lib.selected = s_lib.loaded[0] = s_lib.loaded[1] = INT16_MIN;
    s_lib.status_deck = -1;
    s_lib.load_enabled = -1;
    s_lib.load_locked[0] = s_lib.load_locked[1] = -1;
    s_lib.progress = INT16_MIN;
    s_lib.sort = -1;
    /* never-matching strings: the first update writes all three labels */
    s_lib.source[0] = s_lib.deck_status[0] = s_lib.source_label[0] = '\x01';
    s_lib.unit[0] = s_lib.playlists_label[0] = '\x01';
}

void ui_djui_bridge_library_set_rows(const dj_track_t *rows, uint8_t count)
{
    dj_ui_library_set_rows(rows, count);
    s_lib.selected = s_lib.loaded[0] = s_lib.loaded[1] = INT16_MIN;
}

void ui_djui_bridge_library_set_row_art(uint8_t row, const uint16_t *px)
{
    dj_ui_library_set_row_art(row, px);
}

void ui_djui_bridge_library_update(const ui_djui_library_view_t *v)
{
    if (!v) return;
    bool info = lib_text(s_lib.source, sizeof s_lib.source, v->source);
    info = lib_text(s_lib.unit, sizeof s_lib.unit, v->unit ? v->unit : "TRACKS") || info;
    if (info || s_lib.total != v->total || s_lib.page != v->page || s_lib.pages != v->pages) {
        s_lib.total = v->total;
        s_lib.page = v->page;
        s_lib.pages = v->pages;
        char source_fit[UI_DJUI_LIB_TEXT_LEN];
        ui_djui_text_fit(source_fit, sizeof source_fit, s_lib.source);
        dj_ui_library_set_info_unit(source_fit, v->total, s_lib.unit, v->page, v->pages);
    }
    if (s_lib.sort != (int16_t)v->sort || s_lib.sort_desc != v->sort_desc) {
        s_lib.sort = (int16_t)v->sort;
        s_lib.sort_desc = v->sort_desc;
        dj_ui_library_set_sort(v->sort, v->sort_desc);
    }
    if (s_lib.selected != v->selected) {
        s_lib.selected = v->selected;
        dj_ui_library_set_selected(v->selected);
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        if (s_lib.loaded[d] == v->loaded[d]) continue;
        s_lib.loaded[d] = v->loaded[d];
        dj_ui_library_set_loaded(d, v->loaded[d]);
    }
    bool status = lib_text(s_lib.deck_status, sizeof s_lib.deck_status, v->deck_status);
    if (status || s_lib.status_deck != v->status_deck) {
        s_lib.status_deck = v->status_deck;
        dj_ui_library_set_deck_status(v->status_deck, s_lib.deck_status);
    }
    if (s_lib.load_enabled != (int8_t)v->load_enabled) {
        s_lib.load_enabled = (int8_t)v->load_enabled;
        dj_ui_library_set_load_enabled(v->load_enabled);
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        if (s_lib.load_locked[d] == (int8_t)v->load_locked[d]) continue;
        s_lib.load_locked[d] = (int8_t)v->load_locked[d];
        dj_ui_library_set_load_locked(d, v->load_locked[d]);
    }
    if (s_lib.progress != v->progress) {
        s_lib.progress = v->progress;
        dj_ui_library_set_progress(v->progress);
    }
    if (lib_text(s_lib.source_label, sizeof s_lib.source_label,
                 v->source_label ? v->source_label : "SOURCE")) {
        dj_ui_library_set_source_label(s_lib.source_label);
    }
    if (lib_text(s_lib.playlists_label, sizeof s_lib.playlists_label,
                 v->playlists_label ? v->playlists_label : "PLAYLISTS")) {
        dj_ui_library_set_playlists_label(s_lib.playlists_label);
    }
}
