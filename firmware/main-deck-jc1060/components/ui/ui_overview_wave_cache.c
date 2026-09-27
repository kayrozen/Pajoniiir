#include "ui_overview_wave_cache.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ui_overview_renderer.h"

#define Q16_ONE 65536LL

/* v285: strip columns are rendered into this internal-RAM tile and copied to
 * the PSRAM strip one row at a time. The renderer draws column by column
 * (bars, grid lines, cue lines) and over several passes; straight into PSRAM
 * that was ~14k cache-line misses per full rebuild, ~40 ms on the P4 with the
 * panel scanning the same PSRAM. Taller strips render in place, as before.
 * LVGL task only (the bridge), like the strips. */
#define WAVE_TILE_W 32
#define WAVE_TILE_MAX_H 128
static uint16_t s_tile[WAVE_TILE_W * WAVE_TILE_MAX_H];

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int wrap_px(const ui_overview_wave_cache_t *cache, int x)
{
    int width = cache ? cache->strip_width_px : 0;
    if (width <= 0) {
        return 0;
    }
    x %= width;
    return x < 0 ? x + width : x;
}

static uint32_t q16_to_ms_round(int64_t value_q16)
{
    if (value_q16 <= 0) {
        return 0;
    }
    return (uint32_t)((value_q16 + (Q16_ONE / 2)) / Q16_ONE);
}

static void report_reset(ui_overview_wave_cache_report_t *report)
{
    if (!report) {
        return;
    }
    *report = (ui_overview_wave_cache_report_t){
        .kind = UI_OVERVIEW_WAVE_CACHE_NONE,
        .scroll_dx_px = 0,
        .columns_rendered = 0,
        .blit_required = false,
        .blit_count = 0,
        .blit_height_px = 0,
    };
}

static void record_stats(ui_overview_wave_cache_t *cache,
                         const ui_overview_wave_cache_report_t *report)
{
    if (!cache || !report) {
        return;
    }
    if (report->kind < UI_OVERVIEW_WAVE_CACHE_KIND_COUNT) {
        cache->stats.update_count[report->kind]++;
    }
    cache->stats.total_columns_rendered += report->columns_rendered;
    cache->stats.total_blits += report->blit_count;
}

static void build_blit_segments(ui_overview_wave_cache_t *cache,
                                ui_overview_wave_cache_report_t *report)
{
    if (!cache || !report || cache->strip_width_px <= 0 ||
        cache->view_width_px <= 0 || cache->height_px <= 0) {
        return;
    }

    int physical_start = wrap_px(cache, cache->ring_head_px + cache->view_origin_px);
    int first = cache->view_width_px;
    if (first > cache->strip_width_px - physical_start) {
        first = cache->strip_width_px - physical_start;
    }

    report->blit_required = true;
    report->blit_count = 1;
    report->blit_height_px = (uint16_t)cache->height_px;
    report->blit[0] = (ui_overview_wave_cache_blit_t){
        .src_x_px = (uint16_t)physical_start,
        .dst_x_px = 0,
        .width_px = (uint16_t)first,
    };

    if (first < cache->view_width_px) {
        report->blit_count = 2;
        report->blit[1] = (ui_overview_wave_cache_blit_t){
            .src_x_px = 0,
            .dst_x_px = (uint16_t)first,
            .width_px = (uint16_t)(cache->view_width_px - first),
        };
    }
}

void ui_overview_wave_cache_reset(ui_overview_wave_cache_t *cache)
{
    if (!cache) {
        return;
    }

    uint16_t *pixels = cache->pixels;
    int stride_px = cache->stride_px;
    int width_px = cache->width_px;
    int height_px = cache->height_px;
    int strip_width_px = cache->strip_width_px;
    int view_width_px = cache->view_width_px;
    int margin_px = cache->margin_px;
    const uint16_t *palette = cache->palette;
    size_t palette_count = cache->palette_count;
    bool regular_beat_cap_bottom = cache->regular_beat_cap_bottom;
    bool progressive = cache->progressive;

    memset(cache, 0, sizeof(*cache));
    cache->pixels = pixels;
    cache->stride_px = stride_px;
    cache->width_px = width_px;
    cache->height_px = height_px;
    cache->strip_width_px = strip_width_px;
    cache->view_width_px = view_width_px;
    cache->margin_px = margin_px;
    cache->palette = palette;
    cache->palette_count = palette_count;
    cache->regular_beat_cap_bottom = regular_beat_cap_bottom;
    cache->progressive = progressive;
}

void ui_overview_wave_cache_reset_stats(ui_overview_wave_cache_t *cache)
{
    if (!cache) {
        return;
    }
    memset(&cache->stats, 0, sizeof(cache->stats));
}

void ui_overview_wave_cache_get_stats(const ui_overview_wave_cache_t *cache,
                                      ui_overview_wave_cache_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (!cache) {
        return;
    }
    memcpy(out_stats->update_count,
           cache->stats.update_count,
           sizeof(out_stats->update_count));
    out_stats->total_columns_rendered = cache->stats.total_columns_rendered;
    out_stats->total_blits = cache->stats.total_blits;
}

bool ui_overview_wave_cache_bind_strip(ui_overview_wave_cache_t *cache,
                                       uint16_t *pixels,
                                       int stride_px,
                                       int strip_width_px,
                                       int view_width_px,
                                       int height_px,
                                       int margin_px,
                                       const uint16_t *palette,
                                       size_t palette_count)
{
    if (!cache || !pixels || strip_width_px <= 0 || view_width_px <= 0 ||
        height_px <= 0 || margin_px < 0 || stride_px < strip_width_px ||
        view_width_px > strip_width_px || !palette || palette_count == 0) {
        return false;
    }

    memset(cache, 0, sizeof(*cache));
    cache->pixels = pixels;
    cache->stride_px = stride_px;
    cache->width_px = view_width_px;
    cache->height_px = height_px;
    cache->strip_width_px = strip_width_px;
    cache->view_width_px = view_width_px;
    cache->margin_px = margin_px;
    cache->palette = palette;
    cache->palette_count = palette_count;
    return true;
}

bool ui_overview_wave_cache_bind(ui_overview_wave_cache_t *cache,
                                 uint16_t *pixels,
                                 int stride_px,
                                 int width_px,
                                 int height_px,
                                 const uint16_t *palette,
                                 size_t palette_count)
{
    return ui_overview_wave_cache_bind_strip(cache,
                                             pixels,
                                             stride_px,
                                             width_px,
                                             width_px,
                                             height_px,
                                             0,
                                             palette,
                                             palette_count);
}

void ui_overview_wave_cache_set_regular_beat_cap_bottom(ui_overview_wave_cache_t *cache,
                                                        bool enabled)
{
    if (!cache) {
        return;
    }
    if (cache->regular_beat_cap_bottom == enabled) {
        return;
    }
    cache->regular_beat_cap_bottom = enabled;
    cache->valid = false;
}

void ui_overview_wave_cache_set_loop(ui_overview_wave_cache_t *cache,
                                     bool active,
                                     uint32_t start_ms,
                                     uint32_t end_ms)
{
    if (!cache) {
        return;
    }
    if (cache->loop_active == active &&
        cache->loop_start_ms == start_ms &&
        cache->loop_end_ms == end_ms) {
        return;
    }
    cache->loop_active = active;
    cache->loop_start_ms = start_ms;
    cache->loop_end_ms = end_ms;
    cache->valid = false;
}

static bool cues_equal(const anlz_cue_t *a, const anlz_cue_t *b, uint8_t count)
{
    /* Field-wise: anlz_cue_t has padding, so memcmp could report a change
     * every frame and force a full strip rebuild. */
    for (uint8_t i = 0; i < count; i++) {
        if (a[i].type != b[i].type || a[i].index != b[i].index ||
            a[i].start_ms != b[i].start_ms || a[i].end_ms != b[i].end_ms) {
            return false;
        }
    }
    return true;
}

void ui_overview_wave_cache_set_cues(ui_overview_wave_cache_t *cache,
                                     const anlz_cue_t *cues,
                                     uint8_t cue_count)
{
    if (!cache) {
        return;
    }
    if (!cues) {
        cue_count = 0u;
    } else if (cue_count > ANLZ_MAX_CUES) {
        cue_count = (uint8_t)ANLZ_MAX_CUES;
    }
    if (cache->cues_set &&
        cache->cue_count == cue_count &&
        cues_equal(cache->cues, cues, cue_count)) {
        return;
    }
    cache->cues_set = true;
    cache->cue_count = cue_count;
    for (uint8_t i = 0; i < cue_count; i++) {
        cache->cues[i] = cues[i];
    }
    cache->valid = false;
}

static bool source_matches(const ui_overview_wave_cache_t *cache,
                           const ui_waveform_source_t *source,
                           uint32_t duration_ms,
                           const anlz_metadata_t *meta,
                           uint32_t window_ms)
{
    return cache && cache->valid && source &&
           cache->source_samples == source->samples &&
           cache->source_sample_count == source->sample_count &&
           cache->source_kind == source->kind &&
           cache->duration_ms == duration_ms &&
           cache->meta == meta &&
           cache->window_ms == window_ms;
}

static void cache_store_key(ui_overview_wave_cache_t *cache,
                            const ui_waveform_source_t *source,
                            uint32_t duration_ms,
                            const anlz_metadata_t *meta,
                            uint32_t center_ms,
                            uint32_t window_ms)
{
    cache->source_samples = source->samples;
    cache->source_sample_count = source->sample_count;
    cache->source_kind = source->kind;
    cache->duration_ms = duration_ms;
    cache->meta = meta;
    cache->center_ms = center_ms;
    cache->window_ms = window_ms;
    cache->valid = true;
}

static uint32_t strip_window_ms(const ui_overview_wave_cache_t *cache)
{
    return q16_to_ms_round((int64_t)cache->strip_width_px *
                           cache->ms_per_px_q16);
}

static uint32_t strip_center_ms(const ui_overview_wave_cache_t *cache)
{
    int64_t center_q16 = cache->strip_start_ms_q16 +
                         (((int64_t)cache->strip_width_px *
                           cache->ms_per_px_q16) / 2);
    return q16_to_ms_round(center_q16);
}

static void draw_columns(ui_overview_wave_cache_t *cache,
                         const ui_waveform_source_t *source,
                         uint32_t duration_ms,
                         const anlz_metadata_t *meta,
                         uint16_t *pixels,
                         int stride_px,
                         int dest_x,
                         int logical_x,
                         int column_count)
{
    const anlz_cue_t *cues = cache->cues_set ? cache->cues
                                             : (meta ? meta->cues : NULL);
    uint8_t cue_count = cache->cues_set ? cache->cue_count
                                        : (meta ? meta->cue_count : 0u);
    /* clears the columns itself */
    ui_overview_renderer_draw_main_rgb565_column_span_cues(pixels,
                                                           stride_px,
                                                           cache->height_px,
                                                           dest_x,
                                                           logical_x,
                                                           column_count,
                                                           cache->strip_width_px,
                                                           source,
                                                           duration_ms,
                                                           meta,
                                                           strip_center_ms(cache),
                                                           strip_window_ms(cache),
                                                           cache->palette,
                                                           cache->palette_count,
                                                           cache->regular_beat_cap_bottom,
                                                           cache->loop_active,
                                                           cache->loop_start_ms,
                                                           cache->loop_end_ms,
                                                           cues,
                                                           cue_count);
}

static void render_physical_span(ui_overview_wave_cache_t *cache,
                                 const ui_waveform_source_t *source,
                                 uint32_t duration_ms,
                                 const anlz_metadata_t *meta,
                                 int physical_x,
                                 int logical_x,
                                 int column_count)
{
    while (column_count > 0) {
        int dest_x = wrap_px(cache, physical_x);
        int chunk = column_count;
        if (chunk > cache->strip_width_px - dest_x) {
            chunk = cache->strip_width_px - dest_x;
        }

        if (cache->height_px > WAVE_TILE_MAX_H) {
            draw_columns(cache, source, duration_ms, meta, cache->pixels,
                         cache->stride_px, dest_x, logical_x, chunk);
        } else {
            for (int done = 0; done < chunk; done += WAVE_TILE_W) {
                int w = chunk - done;
                if (w > WAVE_TILE_W) {
                    w = WAVE_TILE_W;
                }
                draw_columns(cache, source, duration_ms, meta, s_tile,
                             WAVE_TILE_W, 0, logical_x + done, w);
                for (int y = 0; y < cache->height_px; y++) {
                    memcpy(&cache->pixels[y * cache->stride_px + dest_x + done],
                           &s_tile[y * WAVE_TILE_W], (size_t)w * sizeof(uint16_t));
                }
            }
        }
        physical_x += chunk;
        logical_x += chunk;
        column_count -= chunk;
    }
}

static void rebuild_full(ui_overview_wave_cache_t *cache,
                         const ui_waveform_source_t *source,
                         uint32_t duration_ms,
                         const anlz_metadata_t *meta,
                         uint32_t center_ms,
                         uint32_t window_ms,
                         ui_overview_wave_cache_report_t *report)
{
    cache->ring_head_px = 0;
    cache->view_origin_px = (cache->strip_width_px - cache->view_width_px) / 2;
    cache->ms_per_px_q16 = ((int64_t)window_ms * Q16_ONE) /
                           (int64_t)cache->view_width_px;
    cache->strip_start_ms_q16 = ((int64_t)center_ms * Q16_ONE) -
                                ((int64_t)(cache->view_origin_px +
                                           (cache->view_width_px / 2)) *
                                 cache->ms_per_px_q16);

    /* v286: progressive: the view now, the margins over the next updates */
    if (cache->progressive && cache->margin_px > 0) {
        cache->filled_lo_px = cache->view_origin_px;
        cache->filled_hi_px = cache->view_origin_px + cache->view_width_px;
    } else {
        cache->filled_lo_px = 0;
        cache->filled_hi_px = cache->strip_width_px;
    }
    render_physical_span(cache, source, duration_ms, meta,
                         cache->filled_lo_px, cache->filled_lo_px,
                         cache->filled_hi_px - cache->filled_lo_px);

    report->kind = UI_OVERVIEW_WAVE_CACHE_FULL;
    report->scroll_dx_px = 0;
    report->columns_rendered = (uint16_t)(cache->filled_hi_px - cache->filled_lo_px);
    build_blit_segments(cache, report);
}

static bool strip_complete(const ui_overview_wave_cache_t *cache)
{
    return cache->filled_lo_px <= 0 && cache->filled_hi_px >= cache->strip_width_px;
}

/* Logical columns [logical_x, logical_x + count) of the unscrolled strip. */
static void render_logical_span(ui_overview_wave_cache_t *cache,
                                const ui_waveform_source_t *source,
                                uint32_t duration_ms,
                                const anlz_metadata_t *meta,
                                int logical_x,
                                int count)
{
    if (count > 0) {
        render_physical_span(cache, source, duration_ms, meta,
                             cache->ring_head_px + logical_x, logical_x, count);
    }
}

/* v286: one step of a progressive rebuild. The strip geometry is the
 * rebuild's; the view moves to desired_origin (inside the strip) with its
 * missing columns rendered first, then up to FILL_BATCH margin columns,
 * right side first. The filled span stays contiguous. */
static void fill_step(ui_overview_wave_cache_t *cache,
                      const ui_waveform_source_t *source,
                      uint32_t duration_ms,
                      const anlz_metadata_t *meta,
                      int desired_origin,
                      ui_overview_wave_cache_report_t *report)
{
    int dx = desired_origin - cache->view_origin_px;
    int view_end = desired_origin + cache->view_width_px;
    int rendered = 0;

    if (desired_origin < cache->filled_lo_px) {
        rendered = cache->filled_lo_px - desired_origin;
        render_logical_span(cache, source, duration_ms, meta, desired_origin, rendered);
        cache->filled_lo_px = desired_origin;
    } else if (view_end > cache->filled_hi_px) {
        rendered = view_end - cache->filled_hi_px;
        render_logical_span(cache, source, duration_ms, meta, cache->filled_hi_px, rendered);
        cache->filled_hi_px = view_end;
    }

    int budget = UI_OVERVIEW_WAVE_CACHE_FILL_BATCH_PX - rendered;
    if (budget > 0 && cache->filled_hi_px < cache->strip_width_px) {
        int n = min_int(budget, cache->strip_width_px - cache->filled_hi_px);
        render_logical_span(cache, source, duration_ms, meta, cache->filled_hi_px, n);
        cache->filled_hi_px += n;
        rendered += n;
        budget -= n;
    }
    if (budget > 0 && cache->filled_lo_px > 0) {
        int n = min_int(budget, cache->filled_lo_px);
        cache->filled_lo_px -= n;
        render_logical_span(cache, source, duration_ms, meta, cache->filled_lo_px, n);
        rendered += n;
    }

    cache->view_origin_px = desired_origin;
    report->kind = UI_OVERVIEW_WAVE_CACHE_FILL;
    report->scroll_dx_px = dx;
    report->columns_rendered = (uint16_t)rendered;
    /* margins only: the visible pixels did not change, nothing to redraw */
    if (dx != 0) {
        build_blit_segments(cache, report);
    }
}

static int desired_view_origin_px(const ui_overview_wave_cache_t *cache,
                                  uint32_t center_ms)
{
    int64_t desired_left_q16 = ((int64_t)center_ms * Q16_ONE) -
                               (((int64_t)cache->view_width_px *
                                 cache->ms_per_px_q16) / 2);
    int64_t delta_q16 = desired_left_q16 - cache->strip_start_ms_q16;
    if (delta_q16 >= 0) {
        return (int)((delta_q16 + (cache->ms_per_px_q16 / 2)) /
                     cache->ms_per_px_q16);
    }
    return (int)((delta_q16 - (cache->ms_per_px_q16 / 2)) /
                 cache->ms_per_px_q16);
}

static bool origin_inside_safe_margin(const ui_overview_wave_cache_t *cache,
                                      int origin_px)
{
    int safe_margin = cache->margin_px / 2;
    return origin_px >= safe_margin &&
           origin_px + cache->view_width_px <=
               cache->strip_width_px - safe_margin;
}

static int max_edge_batch_px(const ui_overview_wave_cache_t *cache)
{
    int max_batch = cache->strip_width_px - cache->view_width_px;
    if (cache->margin_px > 0 && max_batch > cache->margin_px) {
        max_batch = cache->margin_px;
    }
    if (max_batch < 1) {
        max_batch = 1;
    }
    return max_batch;
}

static int edge_batch_px(const ui_overview_wave_cache_t *cache,
                         int desired_shift,
                         int required_to_fit)
{
    int max_batch = max_edge_batch_px(cache);
    if (required_to_fit > max_batch) {
        return 0;
    }

    int batch = abs(desired_shift);
    if (batch < UI_OVERVIEW_WAVE_CACHE_EDGE_BATCH_PX) {
        batch = UI_OVERVIEW_WAVE_CACHE_EDGE_BATCH_PX;
    }
    if (batch < required_to_fit) {
        batch = required_to_fit;
    }
    if (batch > max_batch) {
        batch = max_batch;
    }
    return batch;
}

static void advance_right_edge(ui_overview_wave_cache_t *cache,
                               const ui_waveform_source_t *source,
                               uint32_t duration_ms,
                               const anlz_metadata_t *meta,
                               int desired_origin,
                               int required_to_fit,
                               ui_overview_wave_cache_report_t *report)
{
    int old_origin = cache->view_origin_px;
    int batch = edge_batch_px(cache,
                              desired_origin - cache->view_origin_px,
                              required_to_fit);
    cache->ring_head_px = wrap_px(cache, cache->ring_head_px + batch);
    cache->strip_start_ms_q16 += (int64_t)batch * cache->ms_per_px_q16;
    cache->view_origin_px = desired_origin - batch;

    render_physical_span(cache,
                         source,
                         duration_ms,
                         meta,
                         cache->ring_head_px + cache->strip_width_px - batch,
                         cache->strip_width_px - batch,
                         batch);

    report->kind = UI_OVERVIEW_WAVE_CACHE_EDGE;
    report->scroll_dx_px = desired_origin - old_origin;
    report->columns_rendered = (uint16_t)batch;
    build_blit_segments(cache, report);
}

static void advance_left_edge(ui_overview_wave_cache_t *cache,
                              const ui_waveform_source_t *source,
                              uint32_t duration_ms,
                              const anlz_metadata_t *meta,
                              int desired_origin,
                              int required_to_fit,
                              ui_overview_wave_cache_report_t *report)
{
    int old_origin = cache->view_origin_px;
    int batch = edge_batch_px(cache,
                              cache->view_origin_px - desired_origin,
                              required_to_fit);
    cache->ring_head_px = wrap_px(cache, cache->ring_head_px - batch);
    cache->strip_start_ms_q16 -= (int64_t)batch * cache->ms_per_px_q16;
    cache->view_origin_px = desired_origin + batch;

    render_physical_span(cache,
                         source,
                         duration_ms,
                         meta,
                         cache->ring_head_px,
                         0,
                         batch);

    report->kind = UI_OVERVIEW_WAVE_CACHE_EDGE;
    report->scroll_dx_px = desired_origin - old_origin;
    report->columns_rendered = (uint16_t)batch;
    build_blit_segments(cache, report);
}

static bool update_args_valid(const ui_overview_wave_cache_t *cache,
                              const ui_waveform_source_t *source,
                              uint32_t duration_ms,
                              uint32_t window_ms)
{
    return cache && cache->pixels && cache->stride_px >= cache->strip_width_px &&
           cache->strip_width_px > 0 && cache->view_width_px > 0 &&
           cache->view_width_px <= cache->strip_width_px &&
           cache->height_px > 0 && cache->palette && cache->palette_count > 0 &&
           source && source->samples && source->sample_count > 0 &&
           source->kind != UI_WAVEFORM_SOURCE_NONE &&
           duration_ms > 0 && window_ms > 0;
}

typedef enum {
    PLAN_NONE,
    PLAN_FULL,
    PLAN_OFFSET,
    PLAN_RIGHT_EDGE,
    PLAN_LEFT_EDGE,
    PLAN_FILL,
} update_plan_t;

/* What ui_overview_wave_cache_update will do; valid arguments only. */
static update_plan_t update_plan(const ui_overview_wave_cache_t *cache,
                                 const ui_waveform_source_t *source,
                                 uint32_t duration_ms,
                                 const anlz_metadata_t *meta,
                                 uint32_t center_ms,
                                 uint32_t window_ms,
                                 int *out_desired_origin,
                                 int *out_required_to_fit)
{
    bool full = !source_matches(cache, source, duration_ms, meta, window_ms);
    if (full || cache->margin_px <= 0) {
        return (!full && cache->center_ms == center_ms) ? PLAN_NONE : PLAN_FULL;
    }

    int desired_origin = desired_view_origin_px(cache, center_ms);
    int dx = desired_origin - cache->view_origin_px;
    *out_desired_origin = desired_origin;
    *out_required_to_fit = 0;
    /* v286: a progressive rebuild in progress keeps its geometry while the
     * view stays inside the strip; scrolling needs the complete ring. */
    if (!strip_complete(cache)) {
        return desired_origin >= 0 &&
               desired_origin + cache->view_width_px <= cache->strip_width_px
                   ? PLAN_FILL : PLAN_FULL;
    }
    if (dx == 0) {
        return PLAN_NONE;
    }
    /* v285: a short step back (jog, scratch, snapped centre jitter) that
     * stays inside the safe margin has its pixels in the strip already; only
     * real backward jumps (loop wrap, cue, seek) rebuild. */
    if (abs(dx) <= UI_OVERVIEW_WAVE_CACHE_EDGE_BATCH_PX &&
        origin_inside_safe_margin(cache, desired_origin)) {
        return PLAN_OFFSET;
    }
    if (abs(dx) >= cache->strip_width_px || (dx < 0 && abs(dx) > 4)) {
        return PLAN_FULL;
    }
    int required_to_fit;
    if (dx > 0) {
        required_to_fit = desired_origin + cache->view_width_px -
                          cache->strip_width_px;
        if (required_to_fit < 0) {
            required_to_fit = 0;
        }
    } else {
        required_to_fit = desired_origin < 0 ? -desired_origin : 0;
    }
    if (required_to_fit > max_edge_batch_px(cache)) {
        return PLAN_FULL;
    }
    *out_required_to_fit = required_to_fit;
    return dx > 0 ? PLAN_RIGHT_EDGE : PLAN_LEFT_EDGE;
}

bool ui_overview_wave_cache_needs_full(const ui_overview_wave_cache_t *cache,
                                       const ui_waveform_source_t *source,
                                       uint32_t duration_ms,
                                       const anlz_metadata_t *meta,
                                       uint32_t center_ms,
                                       uint32_t window_ms)
{
    if (!update_args_valid(cache, source, duration_ms, window_ms)) {
        return false;
    }
    int desired_origin = 0;
    int required_to_fit = 0;
    return update_plan(cache, source, duration_ms, meta, center_ms, window_ms,
                       &desired_origin, &required_to_fit) == PLAN_FULL;
}

bool ui_overview_wave_cache_update(ui_overview_wave_cache_t *cache,
                                   const ui_waveform_source_t *source,
                                   uint32_t duration_ms,
                                   const anlz_metadata_t *meta,
                                   uint32_t center_ms,
                                   uint32_t window_ms,
                                   ui_overview_wave_cache_report_t *out_report)
{
    report_reset(out_report);
    if (!update_args_valid(cache, source, duration_ms, window_ms)) {
        if (cache) {
            ui_overview_wave_cache_report_t none_report;
            report_reset(&none_report);
            record_stats(cache, &none_report);
        }
        return false;
    }

    ui_overview_wave_cache_report_t report;
    report_reset(&report);
    int desired_origin = 0;
    int required_to_fit = 0;
    update_plan_t plan = update_plan(cache, source, duration_ms, meta,
                                     center_ms, window_ms,
                                     &desired_origin, &required_to_fit);

    switch (plan) {
    case PLAN_NONE:
#ifdef UI_OVERVIEW_WAVE_CACHE_TESTING
        if (cache->margin_px > 0 && cache->valid && cache->source_generation != 0) {
            cache->source_generation = 0;
            report.kind = UI_OVERVIEW_WAVE_CACHE_OFFSET;
            report.scroll_dx_px = 0;
            report.columns_rendered = 0;
            build_blit_segments(cache, &report);
            if (out_report) {
                *out_report = report;
            }
            return true;
        }
#endif
        record_stats(cache, &report);
        return false;
    case PLAN_FULL:
        rebuild_full(cache, source, duration_ms, meta,
                     center_ms, window_ms, &report);
        break;
    case PLAN_OFFSET: {
        int dx = desired_origin - cache->view_origin_px;
        cache->view_origin_px = desired_origin;
        report.kind = UI_OVERVIEW_WAVE_CACHE_OFFSET;
        report.scroll_dx_px = dx;
        report.columns_rendered = 0;
        build_blit_segments(cache, &report);
        break;
    }
    case PLAN_RIGHT_EDGE:
        advance_right_edge(cache, source, duration_ms, meta,
                           desired_origin, required_to_fit, &report);
        break;
    case PLAN_LEFT_EDGE:
        advance_left_edge(cache, source, duration_ms, meta,
                          desired_origin, required_to_fit, &report);
        break;
    case PLAN_FILL:
        fill_step(cache, source, duration_ms, meta, desired_origin, &report);
        break;
    }

    cache_store_key(cache, source, duration_ms, meta, center_ms, window_ms);
    record_stats(cache, &report);
    if (out_report) {
        *out_report = report;
    }
    return true;
}

void ui_overview_wave_cache_set_progressive(ui_overview_wave_cache_t *cache, bool enabled)
{
    if (cache) {
        cache->progressive = enabled;
    }
}

bool ui_overview_wave_cache_filling(const ui_overview_wave_cache_t *cache)
{
    return cache && cache->valid && cache->margin_px > 0 && !strip_complete(cache);
}

#ifdef UI_OVERVIEW_WAVE_CACHE_TESTING
void ui_overview_wave_cache_test_force_view_origin(ui_overview_wave_cache_t *cache, int origin_px)
{
    if (!cache) {
        return;
    }
    cache->view_origin_px = origin_px;
    if (cache->valid && cache->ms_per_px_q16 > 0) {
        cache->strip_start_ms_q16 = ((int64_t)cache->center_ms * Q16_ONE) -
                                    (((int64_t)cache->view_width_px *
                                      cache->ms_per_px_q16) / 2) -
                                    ((int64_t)origin_px * cache->ms_per_px_q16);
        cache->source_generation = 1;
    }
}
#endif
