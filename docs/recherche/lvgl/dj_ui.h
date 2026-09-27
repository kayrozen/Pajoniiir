/* DJ deck display — LVGL v9, 1024x600, 4 onglets (Overview, Library, Hot Cues, Settings) */
#ifndef DJ_UI_H
#define DJ_UI_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_DECKS    2
#define DJ_HOTCUES  8
#define DJ_LIB_ROWS 8

typedef enum { DJ_TAB_OVERVIEW, DJ_TAB_LIBRARY, DJ_TAB_HOTCUES, DJ_TAB_SETTINGS } dj_tab_t;
typedef enum { DJ_SORT_NONE, DJ_SORT_ARTIST, DJ_SORT_NAME, DJ_SORT_BPM, DJ_SORT_KEY } dj_sort_t;
typedef enum { DJ_TONE_NORMAL, DJ_TONE_MUTED, DJ_TONE_OK, DJ_TONE_WARN, DJ_TONE_INFO, DJ_TONE_ERROR } dj_tone_t;

/* Status/text fields on Settings and Hot Cues screens (dj_ui_set_field) */
typedef enum {
    DJ_F_MASTER, DJ_F_MASTER_HINT,
    DJ_F_OUT_MAIN, DJ_F_OUT_CUE, DJ_F_UI_RENDER, DJ_F_LOCAL,
    DJ_F_SYS_CONTROLLER, DJ_F_SYS_SD, DJ_F_SYS_SD_LOG, DJ_F_SYS_FW, DJ_F_SYS_RESET,
    DJ_F_MIX_MIXER, DJ_F_MIX_FADERS, DJ_F_MIX_XFADER, DJ_F_MIX_PFL, DJ_F_MIX_CUE,
    DJ_F_HC_CUES, DJ_F_HC_LOOPS, DJ_F_HC_ANLZ, DJ_F_HC_TARGET,
    DJ_F_COUNT
} dj_field_t;

typedef struct {
    const char *title, *artist, *key;
    float bpm;
    uint32_t len_ms;
} dj_track_t;

/* Touch events -> your app. Any pointer may be NULL. */
typedef struct {
    void (*on_tab)(dj_tab_t tab);
    void (*on_hotcue)(uint8_t deck, uint8_t index);     /* pad tapped (Overview or Hot Cues) */
    void (*on_fx_beat)(uint8_t index);                  /* 0..3 = 1/4, 1/2, 1, 2 */
    void (*on_fx_toggle)(void);
    void (*on_lib_select)(uint8_t row);
    void (*on_lib_load)(uint8_t deck, uint8_t row);
    void (*on_lib_sort)(dj_sort_t sort);                /* DJ_SORT_NONE = sort cleared */
    void (*on_lib_page)(int8_t dir);                    /* -1 PREV, +1 NEXT */
    void (*on_brightness)(uint8_t pct);
    void (*on_wireless)(bool on);
} dj_ui_callbacks_t;

void dj_ui_create(lv_obj_t *parent);                    /* e.g. lv_screen_active() */
void dj_ui_set_callbacks(const dj_ui_callbacks_t *cb);
void dj_ui_show_tab(dj_tab_t tab);

/* ---- Decks (deck = 0 or 1). Call from the LVGL thread or inside lv_lock()/lv_unlock(). ---- */
void dj_ui_set_track(uint8_t deck, const char *title, const char *artist, const char *source,
                     uint16_t track_no, uint16_t track_count, uint32_t len_ms);
void dj_ui_set_key(uint8_t deck, const char *key);
void dj_ui_set_bpm(uint8_t deck, float track_bpm, uint32_t first_beat_ms); /* original BPM of the file */
void dj_ui_set_tempo(uint8_t deck, float percent);                          /* shown BPM = bpm * (1 + tempo/100) */
void dj_ui_set_position(uint8_t deck, uint32_t pos_ms);
/* peaks/cores 0..255, points_per_sec points per second. cores = bright inner part (NULL = 55%).
 * Buffers are NOT copied: keep them alive while the track is loaded. */
void dj_ui_set_waveform(uint8_t deck, const uint8_t *peaks, const uint8_t *cores,
                        uint32_t count, uint16_t points_per_sec);
void dj_ui_set_hotcue(uint8_t deck, uint8_t index, bool set, uint32_t pos_ms, uint8_t color); /* color 0..7 */
void dj_ui_set_artwork(uint8_t deck, const void *src);   /* lv_image_dsc_t* or "S:/path.png", 34x34. NULL = placeholder */
void dj_ui_set_beat_grid_visible(bool visible);

/* ---- FX panel ---- */
void dj_ui_set_fx(const char *name, uint8_t channel, uint8_t beat_index,
                  uint16_t time_ms, uint8_t level, bool on);

/* ---- Library ---- */
void dj_ui_library_set_rows(const dj_track_t *rows, uint8_t count);        /* max DJ_LIB_ROWS, text is copied */
void dj_ui_library_set_loaded(uint8_t deck, int8_t row);                   /* -1 = not on this page */
void dj_ui_library_set_info(const char *source, uint16_t total, uint16_t page, uint16_t pages);
void dj_ui_library_set_deck_status(uint8_t deck, const char *status);

/* ---- Settings / status ---- */
void dj_ui_set_field(dj_field_t field, const char *text, dj_tone_t tone);  /* text NULL = keep text */
void dj_ui_set_brightness(uint8_t pct);
void dj_ui_set_wireless(bool on);

/* ---- Demo (dj_ui_demo.c): fake data + animation ---- */
void dj_ui_demo_start(void);

#ifdef __cplusplus
}
#endif
#endif
