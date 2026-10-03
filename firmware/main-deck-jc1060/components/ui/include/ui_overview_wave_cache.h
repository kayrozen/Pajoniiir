#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rekordbox_anlz.h"
#include "ui_waveform_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_OVERVIEW_WAVE_CACHE_NONE = 0,
    UI_OVERVIEW_WAVE_CACHE_FULL,
    UI_OVERVIEW_WAVE_CACHE_SCROLL,
    UI_OVERVIEW_WAVE_CACHE_OFFSET,
    UI_OVERVIEW_WAVE_CACHE_EDGE,
    UI_OVERVIEW_WAVE_CACHE_FILL,        /* v286: progressive rebuild, margins */
    UI_OVERVIEW_WAVE_CACHE_KIND_COUNT,
} ui_overview_wave_cache_update_kind_t;

#define UI_OVERVIEW_WAVE_CACHE_MAX_BLITS 2
#define UI_OVERVIEW_WAVE_CACHE_MARGIN_PX 128
#define UI_OVERVIEW_WAVE_CACHE_EDGE_BATCH_PX 32
/* v286: margin columns a progressive rebuild renders per update */
#define UI_OVERVIEW_WAVE_CACHE_FILL_BATCH_PX 64

typedef struct {
    uint16_t src_x_px;
    uint16_t dst_x_px;
    uint16_t width_px;
} ui_overview_wave_cache_blit_t;

typedef struct {
    bool valid;
    const uint8_t *source_samples;
    uint32_t source_sample_count;
    ui_waveform_source_kind_t source_kind;
    uint32_t duration_ms;
    uint32_t center_ms;
    uint32_t window_ms;
    const anlz_metadata_t *meta;
    uint16_t *pixels;
    int stride_px;
    int width_px;
    int height_px;
    int strip_width_px;
    int view_width_px;
    int margin_px;
    int ring_head_px;
    int view_origin_px;
    /* v286: progressive rebuilds (opt-in). A rebuild renders the view only;
     * later updates fill the margins. [filled_lo_px, filled_hi_px) are the
     * logical strip columns holding pixels; the whole strip when complete. */
    bool progressive;
    int filled_lo_px;
    int filled_hi_px;
    int64_t strip_start_ms_q16;
    int64_t ms_per_px_q16;
    uint32_t source_generation;
    const uint16_t *palette;
    size_t palette_count;
    bool regular_beat_cap_bottom;
    bool loop_active;
    uint32_t loop_start_ms;
    uint32_t loop_end_ms;
    /* v244: merged hot cue list (local pads + ANLZ). When cues_set is false
     * the renderer falls back to meta->cues. Cleared by reset. */
    bool cues_set;
    uint8_t cue_count;
    anlz_cue_t cues[ANLZ_MAX_CUES];
    /* v309: memory cue starts burned as bottom triangles (copied). */
    uint8_t memory_count;
    anlz_memory_cue_t memory[ANLZ_MAX_MEMORY_CUES];
    struct {
        uint32_t update_count[UI_OVERVIEW_WAVE_CACHE_KIND_COUNT];
        uint32_t total_columns_rendered;
        uint32_t total_blits;
    } stats;
} ui_overview_wave_cache_t;

typedef struct {
    ui_overview_wave_cache_update_kind_t kind;
    int scroll_dx_px;
    uint16_t columns_rendered;
    bool blit_required;
    uint8_t blit_count;
    uint16_t blit_height_px;
    ui_overview_wave_cache_blit_t blit[UI_OVERVIEW_WAVE_CACHE_MAX_BLITS];
} ui_overview_wave_cache_report_t;

typedef struct {
    uint32_t update_count[UI_OVERVIEW_WAVE_CACHE_KIND_COUNT];
    uint32_t total_columns_rendered;
    uint32_t total_blits;
} ui_overview_wave_cache_stats_t;

void ui_overview_wave_cache_reset(ui_overview_wave_cache_t *cache);
void ui_overview_wave_cache_reset_stats(ui_overview_wave_cache_t *cache);
void ui_overview_wave_cache_get_stats(const ui_overview_wave_cache_t *cache,
                                      ui_overview_wave_cache_stats_t *out_stats);

bool ui_overview_wave_cache_bind(ui_overview_wave_cache_t *cache,
                                 uint16_t *pixels,
                                 int stride_px,
                                 int width_px,
                                 int height_px,
                                 const uint16_t *palette,
                                 size_t palette_count);

bool ui_overview_wave_cache_bind_strip(ui_overview_wave_cache_t *cache,
                                       uint16_t *pixels,
                                       int stride_px,
                                       int strip_width_px,
                                       int view_width_px,
                                       int height_px,
                                       int margin_px,
                                       const uint16_t *palette,
                                       size_t palette_count);

void ui_overview_wave_cache_set_regular_beat_cap_bottom(ui_overview_wave_cache_t *cache,
                                                        bool enabled);

/* Set the active-loop region drawn as an amber highlight on the main waveform.
 * A change to the loop invalidates the cache so the strip is fully re-rendered. */
void ui_overview_wave_cache_set_loop(ui_overview_wave_cache_t *cache,
                                     bool active,
                                     uint32_t start_ms,
                                     uint32_t end_ms);

/* v244: set the hot cue markers drawn on the main waveform (copied). A change
 * to the list invalidates the cache so the strip is fully re-rendered. */
void ui_overview_wave_cache_set_cues(ui_overview_wave_cache_t *cache,
                                     const anlz_cue_t *cues,
                                     uint8_t cue_count);

/* v309: set the memory cues burned on the main waveform (copied, at most
 * ANLZ_MAX_MEMORY_CUES). A change invalidates the cache like set_cues. */
void ui_overview_wave_cache_set_memory_cues(ui_overview_wave_cache_t *cache,
                                            const anlz_memory_cue_t *cues,
                                            uint8_t count);

bool ui_overview_wave_cache_update(ui_overview_wave_cache_t *cache,
                                   const ui_waveform_source_t *source,
                                   uint32_t duration_ms,
                                   const anlz_metadata_t *meta,
                                   uint32_t center_ms,
                                   uint32_t window_ms,
                                   ui_overview_wave_cache_report_t *out_report);

/* v285: true when ui_overview_wave_cache_update with these arguments would
 * rebuild the whole strip. Lets a caller keep to one rebuild per frame. */
bool ui_overview_wave_cache_needs_full(const ui_overview_wave_cache_t *cache,
                                       const ui_waveform_source_t *source,
                                       uint32_t duration_ms,
                                       const anlz_metadata_t *meta,
                                       uint32_t center_ms,
                                       uint32_t window_ms);

/* v286: a strip rebuild renders the visible columns only, then each update
 * renders up to UI_OVERVIEW_WAVE_CACHE_FILL_BATCH_PX margin columns (right
 * side first, playback runs that way) before scrolling by offset/edge again.
 * The view is pixel-identical to a whole-strip rebuild. Survives reset. */
void ui_overview_wave_cache_set_progressive(ui_overview_wave_cache_t *cache, bool enabled);
/* v286: a progressive rebuild still has margin columns to render, so
 * ui_overview_wave_cache_update has work even when the centre did not move. */
bool ui_overview_wave_cache_filling(const ui_overview_wave_cache_t *cache);

#ifdef UI_OVERVIEW_WAVE_CACHE_TESTING
void ui_overview_wave_cache_test_force_view_origin(ui_overview_wave_cache_t *cache, int origin_px);
#endif

#ifdef __cplusplus
}
#endif
