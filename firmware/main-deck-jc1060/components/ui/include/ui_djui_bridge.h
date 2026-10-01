// dj_ui presentation bridge (UI migration phases 1-6; the only UI since v293).
//
// Maps plain per-frame data onto the dj_ui set_* API. Deliberately free of
// deck_core / audio_engine types so the PC simulator can drive it too; ui.c
// fills ui_djui_frame_t from its ui_frame_context_t. LVGL task only.
#pragma once

#include "dj_ui.h"
#include "rekordbox_anlz.h"
#include "ui_waveform_model.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_DJUI_WAVEFORM_LOW_LEN 400   /* PWAV: bits[4:0] = height 0..31 */

typedef struct {
    bool loaded;
    const char *title, *artist;         /* copied by dj_ui on change */
    uint16_t bpm;                       /* file BPM, 0 = unknown */
    float tempo_pct;
    uint32_t duration_ms;               /* track length: timeline, seeks, time labels */
    uint32_t wave_span_ms;              /* time the waveforms cover (<= duration_ms), v271 */
    uint32_t position_ms;
    bool playing;
    const uint8_t *waveform_low;        /* UI_DJUI_WAVEFORM_LOW_LEN bytes, NULL = none */

    /* ---- Overview (phase 5) ---- */
    uint32_t bpm_x100;                  /* base (unpitched) BPM x100, 0 = bpm; dj_ui applies tempo_pct */
    const char *key;                    /* NULL/"" = "--" */
    bool master_tempo;
    bool cue_point_set;
    uint32_t cue_point_ms;
    bool loop_active;                   /* in+out set: burned into the zoom strip */
    uint32_t loop_start_ms, loop_end_ms;
    bool loop_armed;                    /* loop-in pressed, out pending */
    uint32_t loop_armed_ms;
    bool beat_valid, beat_downbeat;
    uint8_t beat_phase;                 /* 0..3 */
    uint16_t vu_peak;                   /* deck_peak_display, 0..32768 */
    /* Zoom strip: our ANLZ wave cache, rendered by the bridge into a PSRAM
     * ring that dj_ui draws as an IMAGE. wave.kind NONE = empty zoom. meta
     * and the samples must stay alive while pushed (the caller holds the
     * ANLZ snapshot); cues are copied. center_ms is the playhead time; the
     * bridge snaps it to the zoom pixel grid. */
    ui_waveform_source_t wave;
    const anlz_metadata_t *meta;
    uint32_t center_ms, window_ms;
    const anlz_cue_t *cues;             /* merged hot cues burned into the strip */
    uint8_t cue_count;
    /* Deck header thumbnail (DJ_ART_DECK_PX^2 RGB565), copied by dj_ui when
     * (art_key, art) changes. NULL = "ART" placeholder, e.g. still decoding. */
    uint32_t art_key;
    const uint16_t *art;
} ui_djui_deck_view_t;

/* Beat FX panel (v264). Plain values, formatted by the caller. */
typedef struct {
    const char *name;                   /* "ECHO"; NULL = "-" */
    uint8_t channel;                    /* 1, 2, 0 = both decks */
    uint8_t beat_index;                 /* 0..3 = 1/4..2, other = no button lit */
    uint16_t time_ms;                   /* 0 = "- ms" */
    uint8_t level_pct;                  /* 0..100 */
    bool on;
} ui_djui_fx_view_t;

typedef struct {
    uint32_t now_ms;
    /* Overview tab selected: zoom strips are only rendered then, and not
     * under the screensaver or the blackout frame (the bridge tracks both).
     * LVGL repaints the kept strip itself on return. */
    bool overview_visible;
    ui_djui_deck_view_t deck[DJ_DECKS];
    ui_djui_fx_view_t fx;
} ui_djui_frame_t;

/* Zoom strip pixels one bridge update may invalidate (both decks). An update
 * that would pass it is deferred to the next frame, decks alternating; one
 * strip always goes through. 0 restores the default. */
#define UI_DJUI_FRAME_BUDGET_PX 286000u
void ui_djui_bridge_set_frame_budget_px(uint32_t px);

typedef struct {
    uint32_t updates;                   /* bridge updates with the Overview visible */
    uint32_t strip_redraws;             /* zoom strips pushed to dj_ui */
    uint32_t strip_deferred;            /* strip updates postponed by the budget */
    uint32_t strip_px_last, strip_px_max;       /* per bridge update */
    uint32_t inv_px_last, inv_px_max;   /* px LVGL rendered per refresh (joined areas) */
    uint32_t zoom_w, zoom_h;            /* strip view size */
    /* v284: where ui_djui_bridge_update spends its time (max per update, us) */
    uint32_t decks_us_max, strips_us_max;
    uint32_t mini_renders, mini_us_max; /* full-track mini waveform redraws */
    uint32_t strip_full, strip_full_us_max;     /* zoom strips rebuilt from scratch */
    uint32_t strip_full_deferred;       /* v285: second rebuild in a frame, moved on */
    uint32_t strip_fill, strip_fill_us_max;     /* v286: margin steps after a rebuild */
    uint32_t direct_blits, direct_us_max;       /* v287: strips composed + blitted by post_refresh */
    uint32_t direct_repaints;           /* LVGL drew over a direct strip */
    uint32_t direct_fails;              /* blit failed: back to LVGL-drawn strips */
} ui_djui_bridge_perf_t;

void ui_djui_bridge_get_perf(ui_djui_bridge_perf_t *out);
void ui_djui_bridge_reset_perf(void);

/* Builds the dj_ui tree on parent and preallocates the mini images and the
 * zoom strips (once, PSRAM on firmware). Returns false when a buffer is
 * missing; that waveform then stays empty, everything else still works. */
bool ui_djui_bridge_create(lv_obj_t *parent);
/* Per frame. Only changed values reach dj_ui; no allocation. */
void ui_djui_bridge_update(const ui_djui_frame_t *frame);
/* Timed status line (ui_status_hold equivalent). hold_ms 0 = until replaced. */
void ui_djui_bridge_status_hold(const char *text, dj_tone_t tone, uint32_t hold_ms, uint32_t now_ms);
void ui_djui_bridge_set_screensaver(bool on);

/* ---- v287 direct zoom strips ----
 * Copies columns [src_x, src_x + w) of an RGB565 strip (src_w px per row, h
 * rows) to the screen at (x, y), outside LVGL. False = not painted. */
typedef bool (*ui_djui_direct_blit_t)(int32_t x, int32_t y, const uint16_t *src, int32_t src_w,
                                      int32_t h, int32_t src_x, int32_t w);
/* With a blit function the zoom strips leave LVGL's draw path: dj_ui shows
 * them as EXTERNAL surfaces and ui_djui_bridge_post_refresh() paints them,
 * the playhead, cue triangle and armed-loop trail burned in. NULL returns to
 * LVGL-drawn IMAGE strips, as does the first failed blit. Call after create. */
void ui_djui_bridge_set_direct_blit(ui_djui_direct_blit_t blit);
/* Deck d's zoom strip area (screen coords), for the caller's repaint tracking. */
bool ui_djui_bridge_direct_area(uint8_t deck, lv_area_t *out);
/* After every LVGL handler cycle, LVGL task. Bit d of repainted: LVGL drew
 * over deck d's strip area since the last call. Blits what changed or was
 * lost, while the zoom is exposed; no allocation. */
void ui_djui_bridge_post_refresh(uint32_t repainted);

/* ---- Settings (phase 2) ----
 * Plain Settings data; the bridge formats the dj_ui texts and tones and only
 * pushes what changed. Strings are read during the call only. */
#define UI_DJUI_BRIGHTNESS_MIN 10u

typedef enum { UI_DJUI_SD_CHECKING, UI_DJUI_SD_OFFLINE, UI_DJUI_SD_MOUNTED } ui_djui_sd_state_t;
typedef enum { UI_DJUI_REC_OFF, UI_DJUI_REC_IDLE, UI_DJUI_REC_ACTIVE, UI_DJUI_REC_ERROR } ui_djui_rec_state_t;

typedef struct {
    uint8_t brightness_pct;
    bool wireless_on;
    const char *master_trim;            /* "MASTER: -3 dB" */
    bool main_out_usb;
    bool ui_blackout_play;              /* v204 test mode armed */
    bool blackout_now;                  /* armed and a deck plays: "UI OFF" frame */
    bool local_monitor;                 /* ES8311 monitor built in */
    const char *controller_name;        /* active profile short name; NULL/"" = "USB" */
    bool controller_connected;
    const char *cue_mode;               /* "CUE: STEREO" */
    bool jog_cdj;                       /* v267: jog mode, false = VINYL */
    uint8_t tempo_range_pct;            /* v275: tempo fader range 6/10/16; 0 = 10 */
    bool load_lock;                     /* v293: no LOAD onto a playing deck */

    ui_djui_sd_state_t sd_state;
    uint64_t sd_free_bytes, sd_total_bytes;
    bool sd_log_valid, sd_log_available;
    uint32_t sd_log_kb, sd_log_dropped;

    const char *fw_version, *fw_partition;  /* NULL = unavailable */
    const char *reset_reason;           /* NULL = hidden */
    bool reset_bad;                     /* panic / WDT / brownout */

    bool link_enabled;                  /* switch position */
    const char *link_status;            /* dj_link_format_status() text */
    dj_tone_t link_tone;
    uint8_t link_peer_count;
    dj_ui_link_peer_t link_peers[DJ_LINK_ROWS];
    bool link_master_valid;             /* false = "LINK OFF" in the top bar */
    dj_ui_link_master_t link_master;

    ui_djui_rec_state_t rec_state;
    uint32_t rec_secs, rec_mb;
} ui_djui_settings_view_t;

/* Clamp for on_brightness: dj_ui's slider, NVS and the backlight share 10..100. */
uint8_t ui_djui_bridge_brightness_clamp(uint8_t pct);
/* Forget what was pushed, so the next settings_update rewrites every field. */
void ui_djui_bridge_settings_invalidate(void);
void ui_djui_bridge_settings_update(const ui_djui_settings_view_t *v);

/* ---- Hot Cues (phase 3) ----
 * The merged per-slot view the legacy Hot Cues tab shows (hot_cue_store >
 * ANLZ > empty, ui_hot_cue_view_merge), for both decks. Pad colour = slot. */
typedef struct {
    bool set;
    bool loop;                          /* ANLZ/store loop type: "LOOP A" */
    uint32_t pos_ms, end_ms;
} ui_djui_hotcue_t;

typedef struct {
    ui_djui_hotcue_t slot[DJ_DECKS][DJ_HOTCUES];
    bool anlz[DJ_DECKS];                /* Rekordbox ANLZ metadata present */
    uint8_t target;                     /* performance target deck */
} ui_djui_hotcues_view_t;

/* Forget what was pushed, so the next hotcues_update rewrites every slot. */
void ui_djui_bridge_hotcues_invalidate(void);
void ui_djui_bridge_hotcues_update(const ui_djui_hotcues_view_t *v);
/* The slot as last pushed, i.e. what the operator tapped. False when out of
 * range; an empty slot returns true with out->set false. */
bool ui_djui_bridge_hotcue_get(uint8_t deck, uint8_t index, ui_djui_hotcue_t *out);

/* ---- Library (v256, phase 4 v258) ----
 * The visible page of ui_library's model (catalog or DJ Link peer list).
 * Rows go through library_set_rows only when the page content changed; the
 * rest is diffed per frame. Strings are read during the call only; source,
 * status holds and deck titles pass through ui_djui_text_fit here, row text
 * is fitted by the caller when it builds the page. */
#define UI_DJUI_LIB_TEXT_LEN 64

typedef struct {
    const char *source;                 /* "LOCAL USB", "USB CDJ-3000 #2 LOADING 40/120" */
    uint16_t total, page, pages;        /* page 1-based, 0/0 when empty */
    const char *unit;                   /* what total counts; NULL = "TRACKS" */
    int8_t selected;                    /* row on this page, -1 = none */
    int8_t loaded[DJ_DECKS];            /* row holding each deck's track, -1 = not here */
    uint8_t status_deck;                /* DJ_DECKS = "DECK 1+2" */
    const char *deck_status;            /* "ACTIVE" / "READY" */
    bool load_enabled;                  /* load gate idle */
    bool load_locked[DJ_DECKS];         /* v293: LOAD LOCK on and the deck plays */
    int16_t progress;                   /* peer download %, <0 hides the bar */
    const char *source_label;           /* "SOURCE" / "CANCEL" */
    const char *playlists_label;        /* "PLAYLISTS" / "ALL TRACKS" / "BACK"; NULL = "PLAYLISTS" */
    dj_sort_t sort;                     /* catalog order; NONE = load order / peer list */
    bool sort_desc;
} ui_djui_library_view_t;

/* Forget what was pushed, so the next library_update rewrites everything. */
void ui_djui_bridge_library_invalidate(void);
/* New page content (count <= DJ_LIB_ROWS). Re-pushes selection and loaded rows
 * next update, since dj_ui drops a selection past the new row count. */
void ui_djui_bridge_library_set_rows(const dj_track_t *rows, uint8_t count);
void ui_djui_bridge_library_update(const ui_djui_library_view_t *v);
/* Thumbnail that arrived after the page was pushed; copied, NULL = none. */
void ui_djui_bridge_library_set_row_art(uint8_t row, const uint16_t *px);

#ifdef __cplusplus
}
#endif
