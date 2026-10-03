#include "ui.h"
#include "lvgl.h"
#include "ui_theme.h"   // centralised colour palette (COL_*); needs lvgl.h above
#include "esp_log.h"
#include "deck_core.h"
#include "library.h"
#include "ui_beat_indicator.h"
#include "ui_controls.h"
#include "ui_deck_anlz_store.h"
#include "ui_diagnostics.h"
#include "ui_frame_context.h"
#include "ui_library.h"
#include "ui_overview.h"
#include "ui_lvgl_backend.h"
#include "ui_overview_perf.h"
#include "ui_settings.h"
#include "splash_screen.h"
#include "ui_idle.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
// Declare mock deck functions for simulator UI actions
void ui_simulator_deck_set_position(uint32_t position_ms);
void ui_simulator_deck_set_playing(bool playing);
void ui_simulator_deck_toggle_master_tempo(void);
void ui_simulator_deck_toggle_play(void);
#endif

#ifndef WIN32
// ── Firmware-only: LVGL ↔ MIPI-DSI panel plumbing ────────────────────────────
#include "audio_engine.h"
#include "control_link.h"
#include "media_catalog.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// UI canvas geometry: 800x480 by default; the JC1060 build passes
// UI_HOR_RES/UI_VER_RES as compile definitions (1024x600, see
// components/ui/CMakeLists.txt and the tests/ui_simulator_e2e_jc1060 gate).
#ifndef UI_HOR_RES
#define UI_HOR_RES   800
#define UI_VER_RES   480
#endif
#endif /* !WIN32 */

#ifndef WIN32
#include "sdkconfig.h"
#endif
/* dj_ui is the only presentation layer (UI migration phase 6, v293). */
#include "ui_artwork.h"
#include "ui_djui_bridge.h"
#include "ui_hot_cue_view.h"
#include "ui_audible_position.h"
#include "ui_beat_fx_format.h"
#include "ui_position_interpolator.h"

#ifdef WIN32
#define UI_UPDATE_PERIOD_MS 16u
#endif

/* ── Idle screensaver ─────────────────────────────────────────────────────── */

/* Two minutes, matching the plan. Step 5 moves this to a persisted Settings
 * entry with an Off position; until then it is the compile-time default so the
 * behaviour can be exercised on hardware. */
#define UI_IDLE_DEFAULT_TIMEOUT_MS (2u * 60u * 1000u)

static ui_idle_t s_idle;
/* Written by any task, read by the UI task. A lost concurrent set only delays
 * the dismissal by one 16 ms tick, so a plain volatile flag is sufficient and
 * keeps deck_core_queue_event free of locks. */
static volatile bool s_idle_activity_flag;
static volatile bool s_idle_shown_pub;

bool ui_activity_notice(void)
{
    s_idle_activity_flag = true;
    return s_idle_shown_pub;
}

static const char *TAG = "ui";

typedef enum {
    UI_TAB_OVERVIEW = 0,
    UI_TAB_LIBRARY,
    UI_TAB_HOT_CUES,
    UI_TAB_SETTINGS,
    UI_TAB_COUNT,
} ui_tab_t;

// ─── UI State and Variables ──────────────────────────────────────────────────
static lv_obj_t *s_main_screen = NULL;
static int       s_active_tab = 0;

// Sub-screen elements
static ui_deck_track_info_t s_deck_track_info[DECK_CORE_DECK_COUNT];
static ui_deck_anlz_store_t s_deck_anlz_store;
static ui_controls_state_t s_controls;

// UI update timing diagnostics
static ui_overview_perf_counter_t s_ui_update_interval_perf;
static ui_overview_perf_counter_t s_ui_update_duration_perf;

static uint8_t ui_deck_index(uint8_t deck)
{
    return deck < DECK_CORE_DECK_COUNT ? deck : DECK_CORE_COMPAT_DECK;
}

static uint8_t ui_deck_control_id(uint8_t deck, uint8_t deck1_id, uint8_t deck2_id)
{
    return ui_deck_index(deck) == CTRL_DECK_2 ? deck2_id : deck1_id;
}

static void ui_copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    size_t i = 0;
    while (i + 1u < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void ui_deck_track_info_clear(uint8_t deck)
{
    uint8_t idx = ui_deck_index(deck);
    memset(&s_deck_track_info[idx], 0, sizeof(s_deck_track_info[idx]));
}

static void ui_deck_track_info_set(uint8_t deck,
                                   const char *title,
                                   const char *artist,
                                   const char *key,
                                   uint16_t bpm,
                                   uint32_t duration_ms)
{
    uint8_t idx = ui_deck_index(deck);
    ui_deck_track_info_t *info = &s_deck_track_info[idx];
    memset(info, 0, sizeof(*info));
    ui_copy_str(info->title,
                sizeof(info->title),
                title && title[0] ? title : "Unknown Title");
    ui_copy_str(info->artist,
                sizeof(info->artist),
                artist && artist[0] ? artist : "Unknown Artist");
    ui_copy_str(info->key, sizeof(info->key), key ? key : "");
    info->bpm = bpm;
    info->duration_ms = duration_ms;
    info->valid = true;
}

/* v307: a DJ Link track's artist, arrived after its load. */
static void ui_deck_track_artist_set(uint8_t deck, const char *artist)
{
    ui_deck_track_info_t *info = &s_deck_track_info[ui_deck_index(deck)];
    if (info->valid && artist && artist[0]) {
        ui_copy_str(info->artist, sizeof(info->artist), artist);
    }
}

/* Time base of the waveforms and the PVBR table: the Rekordbox analysis. */
static uint32_t ui_deck_wave_span_ms(uint8_t deck)
{
    /* v285: the catalog fallback takes the library mutex (held across USB
     * scans); ask it only when no track is loaded. */
    uint32_t span_ms = ui_library_deck_duration_ms(deck, UINT32_MAX);
    if (span_ms != UINT32_MAX) return span_ms;
    uint32_t fallback = 0;
    if (deck == CTRL_DECK_1) {
        (void)library_get_summary(library_selected_track_index(), NULL, &fallback);
    }
    return fallback;
}

/* v271: track length, past the analysis span once the engine has decoded a
 * longer file (audio_track_length.h). Read per frame, so it applies live. */
static uint32_t ui_deck_duration_ms(uint8_t deck)
{
    uint32_t span_ms = ui_deck_wave_span_ms(deck);
#ifndef WIN32
    uint32_t file_ms = span_ms > 0u ? audio_engine_deck_track_length_ms(deck) : 0u;
    if (file_ms > span_ms) return file_ms;
#endif
    return span_ms;
}

static uint16_t ui_deck_bpm(uint8_t deck)
{
    uint16_t loaded_bpm = ui_library_deck_bpm(deck, 0);
    if (loaded_bpm > 0) return loaded_bpm;
    uint16_t fallback = 120;
    if (deck == CTRL_DECK_1) {
        uint16_t bpm = 0;
        if (library_get_summary(library_selected_track_index(), &bpm, NULL) == ESP_OK) {
            fallback = bpm;
        }
    }
    return fallback;
}

static anlz_snapshot_t *ui_deck_anlz_acquire(uint8_t deck)
{
    uint8_t idx = ui_deck_index(deck);
    return ui_deck_anlz_store_acquire(&s_deck_anlz_store, idx);
}

static void ui_performance_seek(uint8_t deck, uint32_t position_ms)
{
#ifndef WIN32
    ESP_LOGW(TAG, "deck %u seek %lu ms src=TOUCH_HOT_CUE", (unsigned)deck + 1u,
             (unsigned long)position_ms);
    audio_engine_deck_seek(deck, position_ms);
#else
    (void)deck;
    ui_simulator_deck_set_position(position_ms);
#endif
}

/* Hot cue recall: starts a paused deck, leaves a playing one playing. PLAY is
 * a toggle in deck_core, so it is only queued when the deck is paused. */
static void ui_performance_play(uint8_t deck)
{
#ifndef WIN32
    if (audio_engine_deck_is_playing(deck)) {
        ESP_LOGW(TAG, "touch D%u hot cue play: already playing", (unsigned)deck + 1u);
        return;
    }
    ESP_LOGW(TAG, "touch D%u hot cue play: queue PLAY", (unsigned)deck + 1u);
    ctrl_event_t ev = {
        .type  = CTRL_EV_BUTTON,
        .id    = ui_deck_control_id(deck, CTRL_ID_DECK1_PLAY, CTRL_ID_DECK2_PLAY),
        .deck  = deck,
        .value = 1,
        .seq   = 0
    };
    deck_core_queue_event(&ev);
#else
    (void)deck;
    ui_simulator_deck_set_playing(true);
#endif
}

static void ui_performance_set_loop(uint8_t deck, uint32_t start_ms, uint32_t end_ms)
{
#ifndef WIN32
    audio_engine_deck_set_loop(deck, start_ms, end_ms);
#else
    (void)deck;
    (void)start_ms;
    (void)end_ms;
#endif
}

static void ui_performance_clear_loop(uint8_t deck)
{
#ifndef WIN32
    audio_engine_deck_clear_loop(deck);
#else
    (void)deck;
#endif
}

static void ui_deck_anlz_set_from_current(uint8_t deck, const anlz_metadata_t *meta)
{
    uint8_t idx = ui_deck_index(deck);
    if (!meta || !ui_deck_anlz_store_set(&s_deck_anlz_store, idx, meta)) {
        ui_deck_anlz_store_clear(&s_deck_anlz_store, idx);
        ESP_LOGW(TAG, "Deck %u ANLZ metadata unavailable", (unsigned)idx + 1u);
    }
}

// ─── dj_ui Actions ───────────────────────────────────────────────────────────

static void ui_overview_action_play_pause(uint8_t deck)
{
#ifdef WIN32
    (void)deck;
    ui_simulator_deck_toggle_play();
    deck_state_t state = deck_core_get_state();
    ESP_LOGI(TAG, "Simulator Play/Pause: %s", state.playing ? "PLAYING" : "PAUSED");
#else
    ESP_LOGW(TAG, "touch D%u PLAY", (unsigned)deck + 1u);
    ctrl_event_t ev = {
        .type  = CTRL_EV_BUTTON,
        .id    = ui_deck_control_id(deck, CTRL_ID_DECK1_PLAY, CTRL_ID_DECK2_PLAY),
        .deck  = deck,
        .value = 1,
        .seq   = 0
    };
    deck_core_queue_event(&ev);
#endif
}

static void ui_overview_action_cue(uint8_t deck)
{
#ifdef WIN32
    (void)deck;
    ui_simulator_deck_set_playing(false);
    ui_simulator_deck_set_position(0);
#else
    /* A tap is a whole press: without the release a paused-on-cue tap leaves
     * deck_core in cue preview, and the next PLAY only latches it. */
    ESP_LOGW(TAG, "touch D%u CUE", (unsigned)deck + 1u);
    ctrl_event_t ev = {
        .type  = CTRL_EV_BUTTON,
        .id    = ui_deck_control_id(deck, CTRL_ID_DECK1_CUE, CTRL_ID_DECK2_CUE),
        .deck  = deck,
        .value = 1,
        .seq   = 0
    };
    deck_core_queue_event(&ev);
    ev.value = 0;
    deck_core_queue_event(&ev);
#endif
}

#define UI_TOUCH_SEEK_MIN_MOVE_MS 20u

/* v269: the source (legacy waveform, dj_ui zoom or mini) is logged so a
 * post-seek desync report can be tied to the tap that caused it. */
static void ui_touch_seek(uint8_t deck, uint32_t target_ms, const char *src)
{
#ifndef WIN32
    /* One tap, one seek; a tap on the playhead itself changes nothing. */
    const uint32_t current_ms = audio_engine_deck_position_ms(deck);
    const uint32_t diff_ms = target_ms > current_ms ? target_ms - current_ms : current_ms - target_ms;
    if (diff_ms < UI_TOUCH_SEEK_MIN_MOVE_MS) {
        ESP_LOGW(TAG, "deck %u seek %lu ms src=%s skipped: at %lu ms",
                 (unsigned)deck + 1u, (unsigned long)target_ms, src, (unsigned long)current_ms);
        return;
    }
    ESP_LOGW(TAG, "deck %u seek %lu ms src=%s from %lu ms", (unsigned)deck + 1u,
             (unsigned long)target_ms, src, (unsigned long)current_ms);
    audio_engine_deck_seek(deck, target_ms);
#else
    (void)deck;
    (void)src;
    ui_simulator_deck_set_position(target_ms);
#endif
}

static void ui_overview_action_toggle_master_tempo(uint8_t deck)
{
    ESP_LOGW(TAG, "touch D%u MASTER TEMPO", (unsigned)deck + 1u);
    deck_core_toggle_master_tempo(deck);
}

// ─── Global Interface Functions ──────────────────────────────────────────────

#ifdef WIN32
static void ui_timer_cb(lv_timer_t *timer) {
    (void)timer;
    ui_update();
}
#else
static void ui_frame_cb(void *user_ctx)
{
    (void)user_ctx;
    ui_update();
}
#endif

#ifndef WIN32
static void ui_perf_log_us(const char *label, const ui_overview_perf_report_t *report)
{
    if (!label || !report) {
        return;
    }

    ESP_LOGI(TAG, "%s: last=%u us avg=%u us max=%u us samples=%u",
             label,
             (unsigned)report->last_us,
             (unsigned)report->avg_us,
             (unsigned)report->max_us,
             (unsigned)report->samples);
}

/* v283: worst case of each ui_update() step over one perf report window, to
 * split a slow frame callback. Laps are taken only with diagnostics on. */
typedef enum {
    UI_STEP_COMMANDS,
    UI_STEP_CONTEXT,
    UI_STEP_LIBRARY,
    UI_STEP_DECK_VIEWS,
    UI_STEP_BRIDGE,
    UI_STEP_PANELS,
    UI_STEP_COUNT,
} ui_step_t;

static const char *const s_step_names[UI_STEP_COUNT] = {
    "cmds", "ctx", "library", "decks", "bridge", "panels",
};
static uint32_t s_step_max_us[UI_STEP_COUNT];
static int64_t s_step_mark_us;

static void ui_step_start(void)
{
    s_step_mark_us = ui_diagnostics_enabled() ? esp_timer_get_time() : 0;
}

static void ui_step_lap(ui_step_t step)
{
    if (s_step_mark_us == 0) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    uint32_t us = (uint32_t)(now_us - s_step_mark_us);
    if (us > s_step_max_us[step]) {
        s_step_max_us[step] = us;
    }
    s_step_mark_us = now_us;
}

/* LVGL task, from the backend's perf report: format only, no I/O. */
static void ui_perf_report(char *buf, size_t size)
{
    size_t off = 0;
    int n = snprintf(buf, size, "ui_update step max:");
    for (int i = 0; i < UI_STEP_COUNT && n > 0 && (off += (size_t)n) < size; i++) {
        n = snprintf(buf + off, size - off, " %s %u us", s_step_names[i], (unsigned)s_step_max_us[i]);
        s_step_max_us[i] = 0;
    }
    if (n > 0) off += (size_t)n;
    /* v285: frames that took the last engine position/status instead of
     * waiting for the decoder's mutex. */
    static uint32_t s_ae_misses_seen;
    uint32_t ae_misses = audio_engine_nowait_lock_misses();
    if (off < size) {
        n = snprintf(buf + off, size - off, " (ae busy x%lu)",
                     (unsigned long)(ae_misses - s_ae_misses_seen));
        if (n > 0) off += (size_t)n;
    }
    s_ae_misses_seen = ae_misses;
    ui_djui_bridge_perf_t perf;
    ui_djui_bridge_get_perf(&perf);
    ui_djui_bridge_reset_perf();
    ui_artwork_stats_t art;
    ui_artwork_take_stats(&art);
    if (off < size) {
        snprintf(buf + off, size - off,
                 "\ndj_ui zoom %lux%lu: updates=%lu redraws=%lu deferred=%lu strip_px max=%lu"
                 " inv_px last=%lu max=%lu\n"
                 "dj_ui bridge us max: decks %lu (mini x%lu %lu) strips %lu (full x%lu %lu, deferred x%lu, fill x%lu %lu)\n"
                 "dj_ui direct strips: blits x%lu us max %lu, lvgl repaints x%lu, fails x%lu\n"
                 "dj_ui artwork: queued=%lu full=%lu decoded=%lu none=%lu skipped=%lu"
                 " read_us max=%lu decode_us max=%lu poll_us max=%lu",
                 (unsigned long)perf.zoom_w, (unsigned long)perf.zoom_h, (unsigned long)perf.updates,
                 (unsigned long)perf.strip_redraws, (unsigned long)perf.strip_deferred,
                 (unsigned long)perf.strip_px_max, (unsigned long)perf.inv_px_last,
                 (unsigned long)perf.inv_px_max,
                 (unsigned long)perf.decks_us_max, (unsigned long)perf.mini_renders,
                 (unsigned long)perf.mini_us_max, (unsigned long)perf.strips_us_max,
                 (unsigned long)perf.strip_full, (unsigned long)perf.strip_full_us_max,
                 (unsigned long)perf.strip_full_deferred,
                 (unsigned long)perf.strip_fill, (unsigned long)perf.strip_fill_us_max,
                 (unsigned long)perf.direct_blits, (unsigned long)perf.direct_us_max,
                 (unsigned long)perf.direct_repaints, (unsigned long)perf.direct_fails,
                 (unsigned long)art.queued, (unsigned long)art.queue_full,
                 (unsigned long)art.decoded, (unsigned long)art.none,
                 (unsigned long)art.skipped, (unsigned long)art.read_us_max,
                 (unsigned long)art.decode_us_max,
                 (unsigned long)art.poll_us_max);
    }
}
#else
typedef enum {
    UI_STEP_COMMANDS,
    UI_STEP_CONTEXT,
    UI_STEP_LIBRARY,
    UI_STEP_DECK_VIEWS,
    UI_STEP_BRIDGE,
    UI_STEP_PANELS,
} ui_step_t;

static void ui_step_start(void) {}
static void ui_step_lap(ui_step_t step) { (void)step; }
#endif

static void ui_splash_screen_finished_cb(void)
{
    ESP_LOGI(TAG, "Splash screen finished, loading main UI...");
    if (s_main_screen) {
        lv_screen_load(s_main_screen);
    }
}

static dj_tone_t ui_djui_tone(lv_color_t color)
{
    if (lv_color_eq(color, COL_GREEN)) return DJ_TONE_OK;
    if (lv_color_eq(color, COL_AMBER)) return DJ_TONE_WARN;
    if (lv_color_eq(color, COL_RED)) return DJ_TONE_ERROR;
    if (lv_color_eq(color, COL_ACCENT)) return DJ_TONE_INFO;
    return DJ_TONE_NORMAL;
}

static void ui_djui_status_hold(const char *text, lv_color_t color, uint32_t hold_ms)
{
    ui_djui_bridge_status_hold(text, ui_djui_tone(color), hold_ms, lv_tick_get());
}

static void ui_djui_on_wake(void)
{
    (void)ui_activity_notice();
}

static void ui_djui_on_tab(dj_tab_t tab)
{
    /* dj_tab_t has the UI_TAB order: ui_is_library_active() and
     * ui_is_overview_active() follow the dj_ui page for controller browse. */
    s_active_tab = (int)tab;
    ui_settings_djui_set_visible(tab == DJ_TAB_SETTINGS);
}

/* Controller SHOW_LIBRARY / TOGGLE_LIBRARY_VIEW, LVGL task. */
static void ui_djui_show_tab(dj_tab_t tab)
{
    ui_lvgl_lock();
    dj_ui_show_tab(tab);
    ui_djui_on_tab(tab);
    ui_lvgl_unlock();
}

/* Phase 3: a dj_ui pad (Overview row or Hot Cues card) runs the legacy
 * hot_cue_event_cb sequence on the slot the operator saw. */
static void ui_djui_on_hotcue(uint8_t deck, uint8_t index)
{
    ui_djui_hotcue_t cue;
    deck = ui_deck_index(deck);
    ESP_LOGW(TAG, "touch D%u hot cue %c (dj_ui)", (unsigned)deck + 1u, 'A' + index);
    if (!ui_djui_bridge_hotcue_get(deck, index, &cue) || !cue.set) {
        ESP_LOGI(TAG, "D%u Hot Cue %c is empty, ignoring click", (unsigned)deck + 1u, 'A' + index);
        return;
    }
    if (cue.loop && cue.end_ms > cue.pos_ms) {
        ui_controls_set_loop_shadow(&s_controls, deck, true, cue.pos_ms, cue.end_ms, 0);
        ui_performance_seek(deck, cue.pos_ms);
        ui_performance_set_loop(deck, cue.pos_ms, cue.end_ms);
        ui_performance_play(deck);
        ESP_LOGI(TAG, "D%u Hot Loop %c active: %lu - %lu ms", (unsigned)deck + 1u, 'A' + index,
                 (unsigned long)cue.pos_ms, (unsigned long)cue.end_ms);
    } else {
        ui_controls_set_loop_shadow(&s_controls, deck, false, 0, 0, 0);
        ui_performance_clear_loop(deck);
        ui_performance_seek(deck, cue.pos_ms);
        ui_performance_play(deck);
        ESP_LOGI(TAG, "D%u Hot Cue %c triggered at %lu ms", (unsigned)deck + 1u, 'A' + index,
                 (unsigned long)cue.pos_ms);
    }
}

/* v264: the FX panel queues the same semantic events as the controller's
 * BEAT FX section; deck_core stays the only owner of the Beat FX state and
 * the panel shows its snapshot back. Target and beat are absolute values, so
 * a stale snapshot never walks past the wanted setting. */
static void ui_djui_queue_fx(ctrl_event_type_t type, uint8_t id, int16_t value)
{
#ifndef WIN32
    ctrl_event_t ev = { .type = type, .id = id, .deck = CTRL_DECK_1, .value = value, .seq = 0 };
    if (deck_core_queue_event(&ev) != ESP_OK) {
        ESP_LOGW(TAG, "touch FX 0x%02x dropped", (unsigned)id);
    }
#else
    (void)type;
    (void)id;
    (void)value;
#endif
}

static void ui_djui_on_fx_beat(uint8_t index)
{
    ESP_LOGW(TAG, "touch FX beat %u", (unsigned)index);
    ui_djui_queue_fx(CTRL_EV_PITCH, CTRL_ID_BEAT_FX_BEAT_SET, (int16_t)index);
}

static void ui_djui_on_fx_toggle(void)
{
    ESP_LOGW(TAG, "touch FX ON");
    ui_djui_queue_fx(CTRL_EV_BUTTON, CTRL_ID_BEAT_FX_ON, 1);
}

static void ui_djui_on_fx_select(void)
{
    ESP_LOGW(TAG, "touch FX select");
    ui_djui_queue_fx(CTRL_EV_BUTTON, CTRL_ID_BEAT_FX_SELECT_NEXT, 1);
}

static void ui_djui_on_fx_channel(void)
{
    deck_core_beat_fx_state_t fx = deck_core_get_beat_fx_state();
    int16_t next = fx.target == CTRL_BEAT_FX_TARGET_CH1   ? CTRL_BEAT_FX_TARGET_CH2
                 : fx.target == CTRL_BEAT_FX_TARGET_CH2   ? CTRL_BEAT_FX_TARGET_BOTH
                                                          : CTRL_BEAT_FX_TARGET_CH1;
    ESP_LOGW(TAG, "touch FX target %d", (int)next);
    ui_djui_queue_fx(CTRL_EV_BUTTON, CTRL_ID_BEAT_FX_TARGET, next);
}

/* Slider 0..100 -> the controller's 0..127 depth; the panel's rounding back
 * ((depth * 100 + 63) / 127, ui_beat_fx_format) returns the same percent. */
static void ui_djui_on_fx_level(uint8_t pct)
{
    if (pct > 100u) pct = 100u;
    ui_djui_queue_fx(CTRL_EV_PITCH, CTRL_ID_BEAT_FX_DEPTH, (int16_t)(((uint32_t)pct * 127u + 50u) / 100u));
}

static void ui_djui_fx_view(const deck_core_beat_fx_state_t *fx, ui_djui_fx_view_t *out, char *name, size_t name_len)
{
    ui_beat_fx_overview_text_t text;
    ui_beat_fx_format_overview(fx, &text);
    snprintf(name, name_len, "%s", text.effect);
    unsigned depth = fx->depth > 127u ? 127u : fx->depth;
    *out = (ui_djui_fx_view_t){
        .name = name,
        .channel = fx->target == CTRL_BEAT_FX_TARGET_CH1 ? 1u : fx->target == CTRL_BEAT_FX_TARGET_CH2 ? 2u : 0u,
        .beat_index = (uint8_t)fx->beat,
        .time_ms = fx->time_ms,
        .level_pct = (uint8_t)((depth * 100u + 63u) / 127u),
        .on = fx->enabled,
    };
}

static void ui_djui_on_lib_sort(dj_sort_t sort)
{
    ui_library_djui_on_sort((uint8_t)sort);
}

static void ui_djui_on_target(uint8_t deck)
{
    if (ui_controls_set_active_deck(&s_controls, ui_deck_index(deck))) {
        ui_djui_status_hold(deck == CTRL_DECK_1 ? "TARGET D1" : "TARGET D2",
                            deck == CTRL_DECK_1 ? COL_ACCENT : COL_GREEN, 1200);
    }
}

/* Both decks every frame: two seqlock copies and an 8-slot merge; the
 * bridge only pushes slots that changed. */
static void ui_djui_hotcues_update(const ui_frame_context_t *ctx)
{
    ui_djui_hotcues_view_t view = { .target = ctx->active_deck };
    for (uint8_t deck = 0; deck < DJ_DECKS && deck < DECK_CORE_DECK_COUNT; deck++) {
        deck_core_hot_cues_t store;
        bool has_store = deck_core_get_hot_cues(deck, &store);
        anlz_cue_t cues[ANLZ_MAX_CUES];
        uint8_t count = ui_hot_cue_view_merge(has_store ? &store : NULL, ctx->deck_meta[deck], cues);
        for (uint8_t j = 0; j < count; j++) {
            if (cues[j].index >= DJ_HOTCUES) continue;
            view.slot[deck][cues[j].index] = (ui_djui_hotcue_t){
                .set = true,
                .loop = cues[j].type == ANLZ_CUE_LOOP,
                .pos_ms = cues[j].start_ms,
                .end_ms = cues[j].end_ms,
            };
        }
        view.anlz[deck] = ctx->deck_meta[deck] != NULL;
    }
    ui_djui_bridge_hotcues_update(&view);
}

/* Everything ui_init() does for the legacy layout except building its widgets:
 * the library keeps its catalog/load pipeline (null-tolerant without a table),
 * with only the widget-free actions wired. */
static void ui_djui_on_play(uint8_t deck);
static void ui_djui_on_cue(uint8_t deck);
static void ui_djui_on_master_tempo(uint8_t deck);
static void ui_djui_on_sync(uint8_t deck);
static void ui_djui_on_seek(uint8_t deck, uint32_t pos_ms, dj_wave_t wave);

#if defined(CONFIG_UI_DJUI_DIRECT_STRIPS) && !defined(WIN32)
/* v287: the zoom strips go from the bridge's ring straight to the framebuffer
 * (PPA) after each LVGL refresh, out of LVGL's draw path; the backend reports
 * which of them an LVGL flush drew over. */
static bool ui_djui_direct_blit(int32_t x, int32_t y, const uint16_t *src, int32_t src_w,
                                int32_t h, int32_t src_x, int32_t w)
{
    ui_overlay_rect_t r = { .x = x, .y = y, .w = w, .h = h };
    return ui_lvgl_backend_blit_rgb565_ppa270_region(&r, src, (uint32_t)src_w, (uint32_t)h,
                                                     (uint32_t)src_x, 0, (uint32_t)w, (uint32_t)h,
                                                     (size_t)src_w * (size_t)h * sizeof(uint16_t),
                                                     NULL) == ESP_OK;
}

static void ui_djui_post_refresh(uint32_t repainted, void *user_ctx)
{
    (void)user_ctx;
    ui_djui_bridge_post_refresh(repainted);
}

static void ui_djui_direct_strips_init(void)
{
    ui_overlay_rect_t rect[DJ_DECKS];
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        lv_area_t a;
        if (d >= UI_LVGL_BACKEND_DIRECT_SLOTS || !ui_djui_bridge_direct_area(d, &a)) {
            ESP_LOGW(TAG, "dj_ui: zoom strips stay LVGL-drawn (no D%u strip)", (unsigned)(d + 1u));
            return;
        }
        rect[d] = (ui_overlay_rect_t){ .x = a.x1, .y = a.y1,
                                       .w = lv_area_get_width(&a), .h = lv_area_get_height(&a) };
    }
    if (ui_lvgl_backend_set_post_refresh_callback(ui_djui_post_refresh, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "dj_ui: zoom strips stay LVGL-drawn (backend already running)");
        return;
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        ui_lvgl_backend_set_direct_rect(d, &rect[d]);
    }
    ui_djui_bridge_set_direct_blit(ui_djui_direct_blit);
    ESP_LOGI(TAG, "dj_ui: zoom strips %dx%d blitted directly (PPA)", rect[0].w, rect[0].h);
}
#endif

static esp_err_t ui_djui_init(void)
{
    ui_library_config_t library_config = {
        .actions = {
            .status_hold = ui_djui_status_hold,
            .clear_deck_track_info = ui_deck_track_info_clear,
            .set_deck_track_info = ui_deck_track_info_set,
            .set_deck_anlz = ui_deck_anlz_set_from_current,
            .set_deck_artist = ui_deck_track_artist_set,
        },
    };
    ui_library_init(&library_config);

    s_main_screen = lv_screen_active();
    ESP_ERROR_CHECK(media_catalog_init());

    if (!ui_djui_bridge_create(s_main_screen)) {
        ESP_LOGW(TAG, "dj_ui: mini waveform buffers unavailable");
    }
#if defined(CONFIG_UI_DJUI_DIRECT_STRIPS) && !defined(WIN32)
    ui_djui_direct_strips_init();
#endif
    ui_settings_djui_init();
    static const dj_ui_callbacks_t callbacks = {
        .on_tab = ui_djui_on_tab,
        .on_brightness = ui_settings_djui_on_brightness,
        .on_wireless = ui_settings_djui_on_wireless,
        .on_field = ui_settings_djui_on_field,
        .on_link = ui_settings_djui_on_link,
        .on_record = ui_settings_djui_on_record,
        .on_wake = ui_djui_on_wake,
        .on_hotcue = ui_djui_on_hotcue,
        .on_target = ui_djui_on_target,
        .on_lib_select = ui_library_djui_on_select,
        .on_lib_load = ui_library_djui_on_load,
        .on_lib_sort = ui_djui_on_lib_sort,
        .on_lib_page = ui_library_djui_on_page,
        .on_lib_source = ui_library_djui_on_source,
        .on_lib_playlists = ui_library_djui_on_playlists,
        .on_play = ui_djui_on_play,
        .on_cue = ui_djui_on_cue,
        .on_master_tempo = ui_djui_on_master_tempo,
        .on_sync = ui_djui_on_sync,
        .on_seek = ui_djui_on_seek,
        .on_fx_beat = ui_djui_on_fx_beat,
        .on_fx_toggle = ui_djui_on_fx_toggle,
        .on_fx_select = ui_djui_on_fx_select,
        .on_fx_channel = ui_djui_on_fx_channel,
        .on_fx_level = ui_djui_on_fx_level,
    };
    dj_ui_set_callbacks(&callbacks);

    ui_library_load_initial_track();
    ESP_LOGI(TAG, "dj_ui presentation active");
    return ESP_OK;
}

static uint64_t ui_monotonic_time_us(void);
static uint32_t ui_pitch_speed_permille(const deck_state_t *state);

/* v262: the simulator has no output chain, its positions are already audible. */
static uint32_t ui_output_latency_us(void)
{
#ifndef WIN32
    return audio_engine_output_latency_us();
#else
    return 0;
#endif
}

/* Phase 5: the bridge's zoom wave cache keeps the ANLZ meta and waveform
 * pointers across frames, so each deck holds its own reference past
 * ui_release_frame_context (same retain-then-release as the legacy overview). */
static anlz_snapshot_t *s_djui_anlz[DECK_CORE_DECK_COUNT];
static ui_position_interpolator_t s_djui_interp[DECK_CORE_DECK_COUNT];
static ui_audible_position_t s_djui_audible[DECK_CORE_DECK_COUNT];
static anlz_cue_t s_djui_cues[DECK_CORE_DECK_COUNT][ANLZ_MAX_CUES];
static anlz_memory_cue_t s_djui_memory[DECK_CORE_DECK_COUNT][ANLZ_MAX_MEMORY_CUES];

static void ui_djui_on_play(uint8_t deck) { ui_overview_action_play_pause(ui_deck_index(deck)); }
static void ui_djui_on_cue(uint8_t deck) { ui_overview_action_cue(ui_deck_index(deck)); }
static void ui_djui_on_master_tempo(uint8_t deck) { ui_overview_action_toggle_master_tempo(ui_deck_index(deck)); }
/* v304: SYNC press + release through the deck queue, as from the controller,
 * so LINK SYNC and local BEAT SYNC take the same deck_core path. */
static void ui_djui_on_sync(uint8_t deck)
{
#ifndef WIN32
    deck = ui_deck_index(deck);
    ESP_LOGW(TAG, "touch D%u SYNC", (unsigned)deck + 1u);
    ctrl_event_t ev = {
        .type  = CTRL_EV_BUTTON,
        .id    = ui_deck_control_id(deck, CTRL_ID_DECK1_SYNC, CTRL_ID_DECK2_SYNC),
        .deck  = deck,
        .value = 1,
        .seq   = 0
    };
    deck_core_queue_event(&ev);
    ev.value = 0;
    deck_core_queue_event(&ev);
#else
    (void)deck;
#endif
}
static void ui_djui_on_seek(uint8_t deck, uint32_t pos_ms, dj_wave_t wave)
{
    ui_touch_seek(ui_deck_index(deck), pos_ms,
                  wave == DJ_WAVE_MINI ? "TOUCH_MINI" : "TOUCH_ZOOM");
}

static const anlz_metadata_t *ui_djui_hold_anlz(uint8_t deck, anlz_snapshot_t *snapshot)
{
    if (s_djui_anlz[deck] != snapshot) {
        anlz_snapshot_t *old = s_djui_anlz[deck];
        s_djui_anlz[deck] = anlz_snapshot_retain(snapshot);
        anlz_snapshot_release(old);
    }
    return anlz_snapshot_metadata(s_djui_anlz[deck]);
}

static void ui_djui_overview_deck(const ui_frame_context_t *ctx, uint8_t deck, ui_djui_deck_view_t *view)
{
    const deck_state_t *state = &ctx->deck_state[deck];
    const anlz_metadata_t *meta = ui_djui_hold_anlz(deck, ctx->deck_anlz[deck]);
    uint16_t deck_bpm = ctx->deck_bpm[deck];

    /* Same position the legacy zoom scrolls with: audio engine speed (pitch x
     * jog bend), frozen while the scratch position is authoritative. */
    uint32_t speed_permille = ctx->mixer_snapshot.scratch_position_authoritative[deck]
                            ? 0u
                            : ctx->mixer_snapshot.effective_speed_permille[deck] != 0
                            ? ctx->mixer_snapshot.effective_speed_permille[deck]
                            : ui_pitch_speed_permille(state);
    /* v262: anchor on the audible position (the mixed one, one output
     * latency ago), then extrapolate. */
    uint64_t now_us = ui_monotonic_time_us();
    uint32_t audible_ms = ui_audible_position_update(&s_djui_audible[deck], state->position_ms, now_us,
                                                     ui_output_latency_us());
    uint32_t position_ms = ui_position_interpolator_update(&s_djui_interp[deck], audible_ms,
                                                           view->duration_ms, state->playing,
                                                           speed_permille, now_us);
    view->position_ms = position_ms;
    view->tempo_pct = (float)state->pitch_centipercent / 100.0f;
    view->master_tempo = state->master_tempo;
    view->sync = !state->sync_enabled ? DJ_SYNC_OFF
               : state->sync_net == DECK_NET_SYNC_OFF ? DJ_SYNC_LOCAL
               : state->sync_net == DECK_NET_SYNC_LOCKED ? DJ_SYNC_LINK_LOCKED
               : DJ_SYNC_LINK_WAIT;
    view->cue_point_set = view->duration_ms > 0 && state->cue_point_ms <= view->duration_ms;
    view->cue_point_ms = state->cue_point_ms;
    view->vu_peak = ctx->mixer_snapshot.deck_peak_display[deck];

    uint32_t base_bpm_x100 = meta && meta->beat_count > 0 && meta->beats[0].bpm_x100 > 0
                           ? meta->beats[0].bpm_x100
                           : (uint32_t)deck_bpm * 100u;
    if (base_bpm_x100 == 0) base_bpm_x100 = 12000u;
    view->bpm_x100 = base_bpm_x100;   /* dj_ui applies tempo_pct, as the legacy label */

    if (view->duration_ms > 0) {
        ui_beat_indicator_state_t beat =
            ui_beat_indicator_calculate(position_ms, meta ? meta->beats : NULL,
                                        meta ? meta->beat_count : 0, deck_bpm);
        view->beat_valid = beat.valid;
        view->beat_phase = beat.phase;
        view->beat_downbeat = beat.downbeat;
    }

    deck_core_loop_display_t loop = deck_core_get_loop_display(deck);
    view->loop_active = loop.active;
    view->loop_start_ms = loop.start_ms;
    view->loop_end_ms = loop.end_ms;
    view->loop_armed = loop.armed;
    view->loop_armed_ms = loop.start_ms;

    view->meta = meta;
    view->wave = ctx->overview_wave_source[deck].kind == UI_OVERVIEW_WAVEFORM_SOURCE_LOADED_MEDIA
               ? ui_waveform_source_select_for_overview_redraw(meta,
                                                               ctx->overview_wave_source[deck].waveform_low,
                                                               ctx->overview_wave_source[deck].has_waveform)
               : ui_waveform_source_select_for_overview_redraw(meta, NULL, false);
    view->window_ms = ui_overview_zoom_window_ms(meta && meta->beat_count > 0 ? meta->beats[0].bpm_x100 : 0,
                                                 deck_bpm);
    view->center_ms = position_ms;

    deck_core_hot_cues_t store;
    bool has_store = deck_core_get_hot_cues(deck, &store);
    view->cue_count = ui_hot_cue_view_merge(has_store ? &store : NULL, meta, s_djui_cues[deck]);
    view->cues = s_djui_cues[deck];

    /* v309: the deck's memory cues (with the MEMORY / DELETE edits); the
     * analysis list until the deck actor has read them for this load. */
    deck_core_memory_cues_t memory;
    uint8_t memory_count = 0;
    if (deck_core_get_memory_cues(deck, &memory)) {
        for (uint8_t i = 0; i < memory.count && i < ANLZ_MAX_MEMORY_CUES; i++) {
            s_djui_memory[deck][memory_count++] = (anlz_memory_cue_t){
                .start_ms = memory.cues[i].pos_ms,
                .end_ms = memory.cues[i].end_ms,
            };
        }
    } else if (meta) {
        memory_count = meta->memory_cue_count < ANLZ_MAX_MEMORY_CUES ? meta->memory_cue_count
                                                                     : ANLZ_MAX_MEMORY_CUES;
        memcpy(s_djui_memory[deck], meta->memory_cues, memory_count * sizeof(meta->memory_cues[0]));
    }
    view->memory = s_djui_memory[deck];
    view->memory_count = memory_count;
}

static void ui_djui_update(const ui_frame_context_t *ctx)
{
    ui_djui_frame_t frame = {
        .now_ms = ctx->now_ms,
        .overview_visible = s_active_tab == (int)DJ_TAB_OVERVIEW,
    };
    for (uint8_t deck = 0; deck < DJ_DECKS && deck < DECK_CORE_DECK_COUNT; deck++) {
        const ui_deck_track_info_t *info = ctx->deck_info[deck];
        ui_djui_deck_view_t *view = &frame.deck[deck];
        view->loaded = info && info->valid;
        view->title = info ? info->title : NULL;
        view->artist = info ? info->artist : NULL;
        view->key = info ? info->key : NULL;
        view->bpm = ctx->deck_bpm[deck];
        view->duration_ms = ctx->deck_duration_ms[deck];
        view->wave_span_ms = ctx->deck_wave_span_ms[deck];
        view->playing = ctx->deck_state[deck].playing;
        view->waveform_low = ctx->overview_wave_source[deck].has_waveform
                                 ? ctx->overview_wave_source[deck].waveform_low
                                 : NULL;
        ui_djui_overview_deck(ctx, deck, view);
        view->art_key = view->loaded ? ui_library_deck_artwork_key(deck) : 0u;
        view->art = view->art_key ? ui_artwork_get(view->art_key, UI_ARTWORK_DECK) : NULL;
        view->load_active = ui_library_deck_load_progress(deck, &view->load_percent, &view->load_db);
    }
    ui_step_lap(UI_STEP_DECK_VIEWS);
    char fx_name[12];
    ui_djui_fx_view(&ctx->beat_fx_state, &frame.fx, fx_name, sizeof fx_name);
    ui_djui_bridge_update(&frame);
    ui_step_lap(UI_STEP_BRIDGE);
    ui_djui_hotcues_update(ctx);
    ui_settings_djui_update(ctx);
    ui_step_lap(UI_STEP_PANELS);
}

esp_err_t ui_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL DJ UI layout (800x480 landscape)...");
    ui_deck_anlz_store_init(&s_deck_anlz_store);
    ui_controls_state_init(&s_controls);

#ifndef WIN32
    // On firmware, bring up LVGL on top of the BSP panel before building widgets.
    // (On the PC simulator the HAL has already initialised LVGL + a display.)
    esp_err_t be_rc = ui_lvgl_backend_init(UI_HOR_RES, UI_VER_RES);
    if (be_rc != ESP_OK) {
        return be_rc;
    }
#endif

    esp_err_t djui_rc = ui_djui_init();
    if (djui_rc != ESP_OK) {
        return djui_rc;
    }

#ifndef WIN32
    ui_idle_init(&s_idle, UI_IDLE_DEFAULT_TIMEOUT_MS,
                 (uint32_t)(esp_timer_get_time() / 1000));
#endif

    // Keep the simulator's historical 16 ms timer. Firmware updates once per
    // physical panel refresh so waveform work is phase-locked to DSI scanout.
#ifdef WIN32
    lv_timer_create(ui_timer_cb, UI_UPDATE_PERIOD_MS, NULL);
#else
    esp_err_t frame_cb_rc = ui_lvgl_backend_set_frame_callback(ui_frame_cb, NULL);
    if (frame_cb_rc != ESP_OK) {
        return frame_cb_rc;
    }
    esp_err_t perf_cb_rc = ui_lvgl_backend_set_perf_report_callback(ui_perf_report);
    if (perf_cb_rc != ESP_OK) {
        return perf_cb_rc;
    }
#endif

    splash_screen_show(ui_splash_screen_finished_cb);

#ifndef WIN32
    // Start the LVGL handler task last, once all widgets exist.
    esp_err_t start_rc = ui_lvgl_backend_start();
    if (start_rc != ESP_OK) {
        return start_rc;
    }
#endif

    ESP_LOGI(TAG, "LVGL DJ UI layout successfully initialized.");
    return ESP_OK;
}

static uint64_t ui_monotonic_time_us(void)
{
#ifndef WIN32
    return (uint64_t)esp_timer_get_time();
#else
    return (uint64_t)lv_tick_get() * 1000u;
#endif
}

static uint32_t ui_pitch_speed_permille(const deck_state_t *state)
{
    if (!state) {
        return 1000u;
    }

    float pitch_pct;
#ifndef WIN32
    pitch_pct = deck_core_pitch_percent(state);
#else
    pitch_pct = ((8192.0f - (float)state->pitch) / 8192.0f) * 10.0f;
#endif
    int speed = 1000 + (int)(pitch_pct * 10.0f + (pitch_pct >= 0.0f ? 0.5f : -0.5f));
    if (speed < 1) {
        speed = 1;
    }
    return (uint32_t)speed;
}

static void ui_build_frame_context(ui_frame_context_t *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->now_us = ui_monotonic_time_us();
    ctx->now_ms = lv_tick_get();
    ctx->active_tab = s_active_tab;

    /* v285: the decoder holds the engine mutex across a seek or a frame
     * decode (63 ms ctx steps on v284); the frame takes the last position
     * instead of waiting for it. */
    ctx->deck_state[CTRL_DECK_1] = deck_core_get_deck_state_nowait(CTRL_DECK_1);
    ctx->deck_state[CTRL_DECK_2] = deck_core_get_deck_state_nowait(CTRL_DECK_2);
    ctx->active_deck = ui_controls_active_deck(&s_controls);
    ctx->active_state = ctx->deck_state[ui_deck_index(ctx->active_deck)];
    ctx->beat_fx_state = deck_core_get_beat_fx_state();

    for (uint8_t deck = 0; deck < DECK_CORE_DECK_COUNT; deck++) {
        ctx->deck_duration_ms[deck] = ui_deck_duration_ms(deck);
        ctx->deck_wave_span_ms[deck] = ui_deck_wave_span_ms(deck);
        ctx->deck_bpm[deck] = ui_deck_bpm(deck);
        ctx->deck_anlz[deck] = ui_deck_anlz_acquire(deck);
        ctx->deck_meta[deck] =
            anlz_snapshot_metadata(ctx->deck_anlz[deck]);
        ctx->deck_info[deck] = &s_deck_track_info[deck];
        ctx->deck_speed_permille[deck] =
            ui_pitch_speed_permille(&ctx->deck_state[deck]);

        const uint8_t *loaded_waveform_low = NULL;
        bool loaded_has_waveform = false;
        if (ui_library_get_loaded_waveform(deck, &loaded_waveform_low, &loaded_has_waveform)) {
            ctx->overview_wave_source[deck] = (ui_overview_waveform_source_info_t){
                .kind = UI_OVERVIEW_WAVEFORM_SOURCE_LOADED_MEDIA,
                .waveform_low = loaded_waveform_low,
                .has_waveform = loaded_has_waveform,
            };
        } else
        {
            ctx->overview_wave_source[deck] = (ui_overview_waveform_source_info_t){
                .kind = UI_OVERVIEW_WAVEFORM_SOURCE_METADATA,
                .waveform_low = NULL,
                .has_waveform = false,
            };
        }
    }

    ctx->active_duration_ms = ctx->deck_duration_ms[ui_deck_index(ctx->active_deck)];
    ctx->active_base_bpm = ctx->deck_bpm[ui_deck_index(ctx->active_deck)];
    ctx->active_meta = ctx->deck_meta[ui_deck_index(ctx->active_deck)];
    if (ctx->active_duration_ms > 0) {
        ctx->active_beat_state =
            ui_beat_indicator_calculate(ctx->active_state.position_ms,
                                        ctx->active_meta ? ctx->active_meta->beats : NULL,
                                        ctx->active_meta ? ctx->active_meta->beat_count : 0,
                                        ctx->active_base_bpm);
        ctx->active_beat_state_valid = ctx->active_beat_state.valid;
    }

    static uint32_t s_overview_slow_bucket = UINT32_MAX;
    uint32_t overview_slow_bucket = ctx->now_ms / 1000u;
    ctx->overview_slow_update = overview_slow_bucket != s_overview_slow_bucket;
    if (ctx->overview_slow_update) {
        s_overview_slow_bucket = overview_slow_bucket;
    }

#ifndef WIN32
    audio_engine_deck_status_t audio_status = {0};
    if (audio_engine_deck_get_status_nowait(ui_deck_index(ctx->active_deck),
                                            &audio_status) == ESP_OK) {
        ctx->ae_loading = (audio_status.state == AE_LOADING);
        ctx->ae_load_pct = audio_status.load_progress;
    } else {
        ctx->ae_loading = false;
        ctx->ae_load_pct = 100;
    }
    audio_engine_get_mixer_snapshot(&ctx->mixer_snapshot);
#else
    ctx->ae_loading = false;
    ctx->ae_load_pct = 100;
#endif
}

static void ui_release_frame_context(ui_frame_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    for (uint8_t deck = 0; deck < DECK_CORE_DECK_COUNT; ++deck) {
        anlz_snapshot_release(ctx->deck_anlz[deck]);
        ctx->deck_anlz[deck] = NULL;
        ctx->deck_meta[deck] = NULL;
    }
    ctx->active_meta = NULL;
}

#ifndef WIN32
static uint32_t ui_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ui_idle_service(const ui_frame_context_t *ctx)
{
    uint32_t now = ui_now_ms();
    if (s_idle_activity_flag) {
        s_idle_activity_flag = false;
        ui_idle_notice_activity(&s_idle, now);
    }

    bool playing = false;
    for (uint8_t d = 0; d < DECK_CORE_DECK_COUNT; d++) {
        if (ctx->deck_state[d].playing) { playing = true; break; }
    }
    /* The recorder is compiled out by default; the inhibit stays in the pure
     * helper so re-enabling it needs no rediscovery here. */
    bool recording = false;

    switch (ui_idle_tick(&s_idle, now, playing, recording)) {
    case UI_IDLE_ACTION_SHOW:
        ui_djui_bridge_set_screensaver(true);
        s_idle_shown_pub = true;
        break;
    case UI_IDLE_ACTION_HIDE:
        ui_djui_bridge_set_screensaver(false);
        s_idle_shown_pub = false;
        break;
    default:
        break;
    }
}
#endif

void ui_update(void) {
#ifndef WIN32
    uint64_t update_start_us = 0;
    if (ui_diagnostics_enabled()) {
        update_start_us = (uint64_t)esp_timer_get_time();
        static uint64_t last_update_start_us = 0;
        if (last_update_start_us != 0) {
            ui_overview_perf_report_t interval_report;
            if (ui_overview_perf_record(&s_ui_update_interval_perf,
                                        (uint32_t)(update_start_us - last_update_start_us),
                                        &interval_report)) {
                ui_perf_log_us("ui_update interval", &interval_report);
            }
        }
        last_update_start_us = update_start_us;
    }
#endif

    /* Controller browse/load events remain compact commands until this point.
     * This is the LVGL task, so the resulting screen and library work has one
     * owner and never runs on the deck-control task. */
    ui_step_start();
    deck_core_process_ui_commands();
    ui_step_lap(UI_STEP_COMMANDS);

    ui_frame_context_t ctx;
    ui_build_frame_context(&ctx);
#ifndef WIN32
    ui_idle_service(&ctx);
#endif
    ui_step_lap(UI_STEP_CONTEXT);
    ui_library_update(&ctx);
    ui_step_lap(UI_STEP_LIBRARY);
    /* A completed load/USB clear can publish a new immutable ANLZ snapshot
     * during ui_library_update(). Refresh the frame so overview/status never
     * re-publish the pre-update handle for one extra tick. */
    ui_release_frame_context(&ctx);
    ui_build_frame_context(&ctx);
    ui_step_lap(UI_STEP_CONTEXT);
    ui_djui_update(&ctx);
    ui_release_frame_context(&ctx);

#ifndef WIN32
    if (ui_diagnostics_enabled()) {
        uint64_t update_end_us = (uint64_t)esp_timer_get_time();
        ui_overview_perf_report_t duration_report;
        if (ui_overview_perf_record(&s_ui_update_duration_perf,
                                    (uint32_t)(update_end_us - update_start_us),
                                    &duration_report)) {
            ui_perf_log_us("ui_update duration", &duration_report);
        }
    }
#endif
}

bool ui_is_overview_active(void)
{
    return s_active_tab == UI_TAB_OVERVIEW;
}

esp_err_t ui_show_library(void)
{
    ui_djui_show_tab(DJ_TAB_LIBRARY);
    return ESP_OK;
}

esp_err_t ui_toggle_library_view(void)
{
    ui_djui_show_tab(s_active_tab == UI_TAB_LIBRARY ? DJ_TAB_OVERVIEW : DJ_TAB_LIBRARY);
    return ESP_OK;
}

void ui_get_deck_track_info(uint8_t deck, char *out_title, size_t title_max, char *out_artist, size_t artist_max, uint16_t *out_bpm, uint32_t *out_duration_ms)
{
    uint8_t idx = ui_deck_index(deck);
    if (out_title && title_max > 0) {
        ui_copy_str(out_title,
                    title_max,
                    s_deck_track_info[idx].title[0] ? s_deck_track_info[idx].title : "No Track");
    }
    if (out_artist && artist_max > 0) {
        ui_copy_str(out_artist,
                    artist_max,
                    s_deck_track_info[idx].artist[0] ? s_deck_track_info[idx].artist : "Unknown Artist");
    }
    if (out_bpm) {
        *out_bpm = s_deck_track_info[idx].bpm;
    }
    if (out_duration_ms) {
        *out_duration_ms = s_deck_track_info[idx].duration_ms;
    }
}
