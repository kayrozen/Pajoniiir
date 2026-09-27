#include "ui_library.h"
#include "ui_diagnostics.h"
#include "ui_event_counter.h"
#include "ui_load_gate.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#include "sdkconfig.h"
#endif

static void ui_library_copy_str(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    dest[0] = '\0';
    if (!src) {
        return;
    }
    size_t i = 0;
    while (i + 1u < dest_size && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void ui_library_truncate_str(char *dest, size_t dest_size, const char *src, size_t max_len)
{
    if (!dest || dest_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    if (max_len == 0 || dest_size < 4) {
        dest[0] = '\0';
        return;
    }

    size_t limit = max_len;
    if (limit >= dest_size) {
        limit = dest_size - 1;
    }

    size_t len = strlen(src);
    if (len <= limit) {
        ui_library_copy_str(dest, dest_size, src);
        return;
    }

    size_t copy_len = limit > 3 ? limit - 3 : 0;
    if (copy_len >= dest_size) {
        copy_len = dest_size - 1;
    }
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    strncat(dest, "...", dest_size - strlen(dest) - 1);
}

void ui_library_format_row_text(ui_library_row_text_t *out,
                                const char *title,
                                const char *artist,
                                const char *key,
                                uint16_t bpm,
                                uint32_t duration_ms)
{
    if (!out) {
        return;
    }

    ui_library_truncate_str(out->title, sizeof(out->title), title, 26);
    ui_library_truncate_str(out->artist, sizeof(out->artist), artist, 18);
    strncpy(out->key, key ? key : "", sizeof(out->key) - 1);
    out->key[sizeof(out->key) - 1] = '\0';

    uint32_t secs = duration_ms / 1000u;
    snprintf(out->bpm, sizeof(out->bpm), "%u", (unsigned)bpm);
    snprintf(out->duration, sizeof(out->duration), "%u:%02u",
             (unsigned)(secs / 60u),
             (unsigned)(secs % 60u));
}

ui_library_update_plan_t ui_library_plan_update(int active_tab,
                                                bool needs_refresh,
                                                bool usb_removed_pending)
{
    return (ui_library_update_plan_t){
        .apply_usb_removed = usb_removed_pending,
        .poll_track_load_result = true,
        .refresh_library = needs_refresh,
        .focus_library_table = active_tab == 1,
    };
}

ui_library_page_t ui_library_page_for_selection(int total_tracks,
                                                 int selected_index)
{
    ui_library_page_t page = {0};
    if (total_tracks <= 0) {
        return page;
    }

    if (selected_index < 0) {
        selected_index = 0;
    } else if (selected_index >= total_tracks) {
        selected_index = total_tracks - 1;
    }

    page.page_count = (total_tracks + UI_LIBRARY_PAGE_ROWS - 1) /
                      UI_LIBRARY_PAGE_ROWS;
    page.page_index = selected_index / UI_LIBRARY_PAGE_ROWS;
    page.first_index = page.page_index * UI_LIBRARY_PAGE_ROWS;
    page.row_count = total_tracks - page.first_index;
    if (page.row_count > UI_LIBRARY_PAGE_ROWS) {
        page.row_count = UI_LIBRARY_PAGE_ROWS;
    }
    page.selected_row = selected_index - page.first_index;
    return page;
}

int ui_library_page_absolute_index(const ui_library_page_t *page,
                                   int visible_row)
{
    if (!page || visible_row < 0 || visible_row >= page->row_count) {
        return -1;
    }
    return page->first_index + visible_row;
}

int ui_library_page_selection_after_delta(int total_tracks,
                                          int selected_index,
                                          int page_delta)
{
    ui_library_page_t page = ui_library_page_for_selection(total_tracks,
                                                            selected_index);
    if (page.page_count == 0 || page_delta == 0) {
        return page.page_count == 0 ? 0 : page.first_index + page.selected_row;
    }

    int target_page = page.page_index + page_delta;
    if (target_page < 0) {
        target_page = 0;
    } else if (target_page >= page.page_count) {
        target_page = page.page_count - 1;
    }

    int target = target_page * UI_LIBRARY_PAGE_ROWS + page.selected_row;
    if (target >= total_tracks) {
        target = total_tracks - 1;
    }
    return target;
}

#ifndef UI_LIBRARY_HOST_TEST

#include "library.h"
#include "esp_log.h"
#include "ui_lvgl_backend.h"
#include "ui_theme.h"

#ifndef WIN32
#include "audio_engine.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "media_catalog.h"
#include "media_io_gate.h"
#include "dj_link.h"
#include "dj_link_session.h"
#include "ui_artwork.h"
#include "ui_djui_bridge.h"
#include "ui_djui_text.h"

/* v292: EXT_RAM_BSS_ATTR moves a static to PSRAM when
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set; empty on host builds. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define UI_TRACK_LOAD_STACK (16 * 1024)
#define UI_DJ_LINK_COUNT_MS 500u
#endif

static const char *TAG = "ui_library";

/* v257: the dj_ui build logs every LOAD step at WARN (the default level), so
 * a serial trace shows where a tap stopped. Legacy keeps its INFO logs. */
#define UI_LIB_TRACE(...) ESP_LOGW(TAG, __VA_ARGS__)

static ui_library_config_t s_library_config;
static int s_active_tab = 0;
static int s_selected_track_idx = 0;
static ui_event_counter_t s_library_refresh_events;
static uint32_t s_library_refresh_applied;
static ui_load_gate_t s_track_load_gate;
static uint8_t s_library_load_request_deck = CTRL_DECK_1;

static uint32_t s_deck_loaded_track_key[DECK_CORE_DECK_COUNT] = {0, 0};
static bool s_deck_loaded_track_valid[DECK_CORE_DECK_COUNT] = {false, false};
#ifdef WIN32
static uint8_t s_deck_loaded_waveform_low[DECK_CORE_DECK_COUNT][400];
static bool s_deck_loaded_has_waveform[DECK_CORE_DECK_COUNT] = {false, false};
static uint32_t s_deck_loaded_duration_ms[DECK_CORE_DECK_COUNT] = {0, 0};
static uint16_t s_deck_loaded_bpm[DECK_CORE_DECK_COUNT] = {0, 0};
#endif

static const anlz_metadata_t *ui_library_clone_loaded_anlz(anlz_metadata_t *snapshot)
{
    if (!snapshot) {
        return NULL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
#ifdef WIN32
    esp_err_t rc = library_clone_current_anlz(snapshot);
#else
    esp_err_t rc = media_catalog_clone_loaded_anlz(snapshot);
#endif
    return rc == ESP_OK ? snapshot : NULL;
}

#ifndef WIN32
static portMUX_TYPE s_track_load_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static media_loaded_track_t s_loaded_media[DECK_CORE_DECK_COUNT];
static bool s_loaded_media_valid[DECK_CORE_DECK_COUNT];
static QueueHandle_t s_track_load_result_q = NULL;
static ui_event_counter_t s_usb_removed_events;
static uint32_t s_usb_removed_applied;
/* v247: gate id of a load started by a DJ Link 0x19, and who sent it. */
static uint32_t s_remote_load_ui_id;
static uint8_t s_remote_load_from;
static uint32_t s_dj_link_count_ms;
/* v248: Library source. 0 = our USB (media_catalog); otherwise the player
 * whose USB dj_link browses over its dbserver. Peer rows are read from the
 * dj_link cache, never over the network from here. */
static uint8_t s_library_peer;
static int s_local_selected_idx;
static dj_link_browse_status_t s_peer_status;  /* last polled copy */
static uint32_t s_peer_select_generation;      /* status generation at select */
static uint32_t s_peer_want_generation;
static int s_peer_want_first = -1;
static uint32_t s_peer_repaint_ms;
#define UI_PEER_REPAINT_MS 150u
/* v249: LOAD on a peer row downloads the track into the SD cache (dj_link
 * task, NFS), then loads that local file through the usual worker. One
 * download at a time; SOURCE turns into CANCEL meanwhile. */
typedef struct {
    uint32_t id;                  /* dj_link fetch id, 0 = none */
    uint8_t  deck;
    uint8_t  peer;
    uint32_t rekordbox_id;
    dj_link_fetch_state_t state;
    uint8_t  percent;
    media_catalog_track_t item;   /* title / artist / bpm / duration for the deck */
} ui_peer_fetch_t;
static ui_peer_fetch_t s_peer_fetch;
static uint32_t s_peer_load_ui_id;  /* gate id of the load that follows it */
#define UI_PEER_CACHE_PREFIX "/sd/djlcache/"

typedef struct {
    int index;
    uint8_t deck;
    uint32_t generation;
    uint32_t track_key;
    uint32_t load_id;
    uint32_t audio_session_generation;
    bool deck_reset;
    media_catalog_track_t item;
    media_loaded_track_t loaded;
    esp_err_t rc;
    bool peer;
    char status[40];
} ui_track_load_result_t;

_Static_assert(sizeof(ui_track_load_result_t) <= UI_TRACK_LOAD_STACK / 2u,
               "track-load result must leave at least half the worker stack free");

typedef struct {
    int index;
    uint8_t deck;
    uint32_t generation;
    uint32_t track_key;
    uint32_t load_id;
    bool peer;                        /* v249: a DJ Link download in the SD cache */
    media_catalog_track_t peer_item;
    char peer_path[DJ_LINK_FETCH_PATH_MAX];
} ui_track_load_request_t;

#endif

#ifndef WIN32
static void ui_track_load_set_status(ui_track_load_result_t *result,
                                     const char *status,
                                     const char *fallback)
{
    const char *text = (status && status[0]) ? status : fallback;
    snprintf(result->status, sizeof(result->status), "%.*s",
             (int)sizeof(result->status) - 1,
             text ? text : "");
}
#endif

static bool ui_library_try_begin_track_load(void)
{
#ifndef WIN32
    bool accepted = false;
    portENTER_CRITICAL(&s_track_load_lock);
    accepted = ui_load_gate_try_begin(&s_track_load_gate, NULL);
    portEXIT_CRITICAL(&s_track_load_lock);
    return accepted;
#else
    return ui_load_gate_try_begin(&s_track_load_gate, NULL);
#endif
}

static uint32_t ui_library_active_track_load_id(void)
{
#ifndef WIN32
    uint32_t load_id;
    portENTER_CRITICAL(&s_track_load_lock);
    load_id = s_track_load_gate.active_id;
    portEXIT_CRITICAL(&s_track_load_lock);
    return load_id;
#else
    return s_track_load_gate.active_id;
#endif
}

static bool ui_library_track_load_is_current(uint32_t load_id)
{
#ifndef WIN32
    bool current;
    portENTER_CRITICAL(&s_track_load_lock);
    current = ui_load_gate_is_current(&s_track_load_gate, load_id);
    portEXIT_CRITICAL(&s_track_load_lock);
    return current;
#else
    return ui_load_gate_is_current(&s_track_load_gate, load_id);
#endif
}

static bool ui_library_track_load_busy(void)
{
#ifndef WIN32
    bool busy;
    portENTER_CRITICAL(&s_track_load_lock);
    busy = s_track_load_gate.busy;
    portEXIT_CRITICAL(&s_track_load_lock);
    return busy;
#else
    return s_track_load_gate.busy;
#endif
}

static void ui_library_finish_track_load_id(uint32_t load_id)
{
#ifndef WIN32
    portENTER_CRITICAL(&s_track_load_lock);
    (void)ui_load_gate_finish(&s_track_load_gate, load_id);
    portEXIT_CRITICAL(&s_track_load_lock);
#else
    (void)ui_load_gate_finish(&s_track_load_gate, load_id);
#endif
}

static void ui_library_finish_track_load(void)
{
    ui_library_finish_track_load_id(ui_library_active_track_load_id());
}

static void ui_library_invalidate_track_load(void)
{
#ifndef WIN32
    portENTER_CRITICAL(&s_track_load_lock);
    ui_load_gate_invalidate(&s_track_load_gate);
    portEXIT_CRITICAL(&s_track_load_lock);
#else
    ui_load_gate_invalidate(&s_track_load_gate);
#endif
}

/* JC1060 playlists: the local source browses every track, the playlist list
 * (export.pdb, see library_playlist_*), or one playlist's tracks in rekordbox
 * order. A peer source is always ALL. Playlist rows hold catalog track keys,
 * which survive a sort; a catalog rebuild or USB removal returns to ALL. */
typedef enum {
    UI_LIB_MODE_ALL = 0,
    UI_LIB_MODE_PLAYLISTS,
    UI_LIB_MODE_PLAYLIST_TRACKS,
} ui_library_mode_t;

#define UI_LIBRARY_PLAYLIST_MAX_TRACKS 2048

static ui_library_mode_t s_lib_mode = UI_LIB_MODE_ALL;
static uint32_t *s_pl_keys;             /* PSRAM, allocated on the first open */
static int s_pl_key_count;
static char s_pl_name[LIBRARY_PLAYLIST_TEXT_MAX];
static int s_all_selected_idx;          /* ALL row kept while in playlists */
static int s_pl_list_selected_idx;      /* list row kept while inside one */

static uint8_t ui_library_deck_index(uint8_t deck)
{
    return deck < DECK_CORE_DECK_COUNT ? deck : DECK_CORE_COMPAT_DECK;
}

static int ui_library_media_count(void)
{
#ifndef WIN32
    return media_catalog_count();
#else
    return library_count();
#endif
}

#ifndef WIN32
/* The polled status describes the current peer selection (not a previous
 * one still in flight to the dj_link task). */
static bool ui_library_peer_status_current(void)
{
    return s_library_peer != 0u && s_peer_status.peer == s_library_peer &&
           s_peer_status.generation != s_peer_select_generation;
}
#endif

static bool ui_library_peer_view(void)
{
#ifndef WIN32
    return s_library_peer != 0u;
#else
    return false;
#endif
}

/* Rows in the table's current source. */
static int ui_library_view_count(void)
{
#ifndef WIN32
    if (ui_library_peer_view()) {
        return ui_library_peer_status_current() ? (int)s_peer_status.count : 0;
    }
#endif
    if (s_lib_mode == UI_LIB_MODE_PLAYLISTS) {
        return library_playlist_count();
    }
    if (s_lib_mode == UI_LIB_MODE_PLAYLIST_TRACKS) {
        return s_pl_key_count;
    }
    return ui_library_media_count();
}

/* Catalog row behind a local view index, or -1 (playlist list rows, or a
 * playlist entry the catalog no longer holds). */
static int ui_library_catalog_row(int view_idx)
{
    if (s_lib_mode == UI_LIB_MODE_PLAYLISTS) {
        return -1;
    }
    if (s_lib_mode == UI_LIB_MODE_PLAYLIST_TRACKS) {
        if (!s_pl_keys || view_idx < 0 || view_idx >= s_pl_key_count) {
            return -1;
        }
#ifndef WIN32
        return media_catalog_find_index_by_key(s_pl_keys[view_idx]);
#else
        return library_find_row_by_key(s_pl_keys[view_idx]);
#endif
    }
    return view_idx;
}

static void ui_library_status_hold(const char *text, lv_color_t color, uint32_t hold_ms)
{
    if (s_library_config.actions.status_hold) {
        s_library_config.actions.status_hold(text, color, hold_ms);
    }
}

static ui_library_page_t ui_library_current_page(void)
{
    return ui_library_page_for_selection(ui_library_view_count(),
                                         s_selected_track_idx);
}

/* The visible page only changes when the selection moves or the catalog is
 * republished, but the LVGL draw path needs it per cell. Keep the last computed
 * page so the draw callback does not take the library lock (via media_catalog_count)
 * once per draw task. Every mutation of s_selected_track_idx or of the row set
 * goes through ui_library_populate_rows()/ui_library_select_visible_cell(), which
 * refresh this. */
static ui_library_page_t s_library_page_cache;
static bool s_library_page_cache_valid;
/* v256: dj_ui has no table; ui_library_populate_rows() marks the page for
 * ui_library_djui_publish() instead. */
static bool s_djui_rows_dirty = true;

static ui_library_page_t ui_library_refresh_page_cache(void)
{
    s_library_page_cache = ui_library_current_page();
    s_library_page_cache_valid = true;
    return s_library_page_cache;
}

static ui_library_page_t ui_library_page_cached(void)
{
    if (!s_library_page_cache_valid) {
        return ui_library_refresh_page_cache();
    }
    return s_library_page_cache;
}

static void ui_library_invalidate_page_cache(void)
{
    s_library_page_cache_valid = false;
}

#ifndef WIN32
static bool ui_library_peer_fetch_active(void)
{
    return s_peer_fetch.id != 0u;
}

#endif

#ifndef WIN32
/* Peer row: KEY carries a badge instead (the dbserver list has no key):
 * NET = loadable over NFS, META = metadata only, DB / NN% = this row is
 * downloading. BPM / TIME show "..." until the metadata request for the
 * visible page has come back. */
static void ui_library_peer_badge(const dj_link_peer_track_t *t, char *badge, size_t len)
{
    if (ui_library_peer_fetch_active() && s_peer_fetch.peer == s_library_peer &&
        s_peer_fetch.rekordbox_id == t->rekordbox_id) {
        if (s_peer_fetch.state == DJ_LINK_FETCH_AUDIO) {
            snprintf(badge, len, "%u%%", (unsigned)s_peer_fetch.percent);
        } else {
            snprintf(badge, len, "DB");
        }
    } else {
        snprintf(badge, len, "%s", t->audio == DJ_LINK_PEER_AUDIO_NFS ? "NET" : "META");
    }
}

#endif

static void ui_library_select_visible_cell(void)
{
    (void)ui_library_refresh_page_cache();
    s_djui_rows_dirty = true;
}

static void ui_library_populate_rows(void)
{
    ui_library_refresh_page_cache();
    s_djui_rows_dirty = true;
}

static void ui_library_playlist_rows_changed(void)
{
    ui_library_invalidate_page_cache();
    ui_library_populate_rows();
}

/* LVGL lock held. Opens playlist `index` of the list view. */
static void ui_library_open_playlist(int index)
{
    library_playlist_info_t info;
    if (library_playlist_get(index, &info) != ESP_OK) {
        return;
    }
    if (!s_pl_keys) {
#ifndef WIN32
        s_pl_keys = heap_caps_malloc(UI_LIBRARY_PLAYLIST_MAX_TRACKS * sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_pl_keys = malloc(UI_LIBRARY_PLAYLIST_MAX_TRACKS * sizeof(uint32_t));
#endif
        if (!s_pl_keys) {
            ui_library_status_hold("NO MEM", COL_RED, 2000);
            return;
        }
    }
    int n = library_playlist_track_keys(index, s_pl_keys, UI_LIBRARY_PLAYLIST_MAX_TRACKS);
    s_pl_key_count = n > 0 ? n : 0;
    ui_library_copy_str(s_pl_name, sizeof(s_pl_name), info.name);
    s_pl_list_selected_idx = index;
    s_lib_mode = UI_LIB_MODE_PLAYLIST_TRACKS;
    s_selected_track_idx = 0;
    ui_library_playlist_rows_changed();
    UI_LIB_TRACE("playlist %d '%.20s': %d tracks, %u not on USB", index, info.name,
                 s_pl_key_count, (unsigned)info.missing);
    if ((int)info.track_count > s_pl_key_count && s_pl_key_count > 0) {
        ui_library_status_hold("FIRST 2048 TRACKS", COL_AMBER, 1500);
    } else if (info.missing > 0u) {
        char status[48];
        snprintf(status, sizeof(status), "%u NOT ON USB", (unsigned)info.missing);
        ui_library_status_hold(status, COL_AMBER, 1500);
    } else if (s_pl_key_count == 0) {
        ui_library_status_hold("EMPTY PLAYLIST", COL_AMBER, 1200);
    }
}

/* Back to all local tracks (catalog rebuilt, USB removed, peer source).
 * Returns whether a playlist view was left. LVGL lock held. */
static bool ui_library_leave_playlists(void)
{
    if (s_lib_mode == UI_LIB_MODE_ALL) {
        return false;
    }
    s_lib_mode = UI_LIB_MODE_ALL;
    s_pl_key_count = 0;
    s_pl_name[0] = '\0';
    int n = ui_library_media_count();
    s_selected_track_idx = s_all_selected_idx < n ? s_all_selected_idx : 0;
    return true;
}

/* PLAYLISTS button: ALL -> playlist list -> ALL; inside a playlist it is
 * BACK to the list. LVGL lock held. */
static void ui_library_playlists_button(void)
{
    if (ui_library_peer_view()) {
        ui_library_status_hold("PLAYLISTS: LOCAL ONLY", COL_AMBER, 1200);
        return;
    }
    switch (s_lib_mode) {
    case UI_LIB_MODE_ALL:
        if (library_playlist_count() <= 0) {
            ui_library_status_hold("NO PLAYLISTS", COL_AMBER, 1200);
            return;
        }
        s_all_selected_idx = s_selected_track_idx;
        s_lib_mode = UI_LIB_MODE_PLAYLISTS;
        s_selected_track_idx = s_pl_list_selected_idx < library_playlist_count()
            ? s_pl_list_selected_idx : 0;
        break;
    case UI_LIB_MODE_PLAYLISTS:
        (void)ui_library_leave_playlists();
        break;
    case UI_LIB_MODE_PLAYLIST_TRACKS:
    default:
        s_lib_mode = UI_LIB_MODE_PLAYLISTS;
        s_pl_key_count = 0;
        s_pl_name[0] = '\0';
        s_selected_track_idx = s_pl_list_selected_idx < library_playlist_count()
            ? s_pl_list_selected_idx : 0;
        break;
    }
    UI_LIB_TRACE("library mode %d, index %d", (int)s_lib_mode, s_selected_track_idx);
    ui_library_playlist_rows_changed();
}

static void ui_library_apply_loaded_track(uint8_t deck,
                                          const char *title,
                                          const char *artist,
                                          const char *key,
                                          uint16_t bpm,
                                          uint32_t duration_ms,
                                          const uint8_t waveform_low[400],
                                          bool has_waveform,
                                          const anlz_metadata_t *meta)
{
    deck = ui_library_deck_index(deck);
#ifdef WIN32
    s_deck_loaded_has_waveform[deck] = has_waveform;
    s_deck_loaded_duration_ms[deck] = duration_ms;
    s_deck_loaded_bpm[deck] = bpm;
    if (waveform_low) {
        memcpy(s_deck_loaded_waveform_low[deck], waveform_low, 400);
    }
#endif
    if (s_library_config.actions.set_deck_track_info) {
        s_library_config.actions.set_deck_track_info(deck, title, artist, key, bpm, duration_ms);
    }
    if (s_library_config.actions.set_deck_anlz) {
        s_library_config.actions.set_deck_anlz(deck, meta);
    }
}

static void ui_library_apply_empty_track(uint8_t deck)
{
    deck = ui_library_deck_index(deck);
#ifdef WIN32
    s_deck_loaded_has_waveform[deck] = false;
#endif
#ifndef WIN32
    s_loaded_media_valid[deck] = false;
    memset(&s_loaded_media[deck], 0, sizeof(s_loaded_media[deck]));
#endif
    s_deck_loaded_track_valid[deck] = false;
    s_deck_loaded_track_key[deck] = 0u;
    if (s_library_config.actions.clear_deck_track_info) {
        s_library_config.actions.clear_deck_track_info(deck);
    }
    if (s_library_config.actions.set_deck_anlz) {
        s_library_config.actions.set_deck_anlz(deck, NULL);
    }
}

#ifndef WIN32
/* Tear down the audio session for a deck whose load result is not going to be
 * published. Safe on a deck whose load already failed: audio_engine_stop_for_deck
 * does not key teardown on `loaded`, so it simply joins whatever tasks the
 * failed session parked and returns the deck to the empty state the UI shows. */
static void ui_library_release_deck_audio(uint8_t deck)
{
    esp_err_t rc = audio_engine_deck_stop(deck);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "deck %u audio release failed: %s",
                 (unsigned)deck + 1u, esp_err_to_name(rc));
    }
}

static void ui_library_release_deck_audio_session(uint8_t deck,
                                                  uint32_t session_generation)
{
    if (session_generation == 0u) return;
    esp_err_t rc = audio_engine_deck_stop_session(deck, session_generation);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "deck %u audio session %u release failed: %s",
                 (unsigned)deck + 1u, (unsigned)session_generation,
                 esp_err_to_name(rc));
    }
}

static void ui_track_load_worker(void *arg)
{
    ui_track_load_request_t req = *(ui_track_load_request_t *)arg;
    free(arg);

    /* The single-flight load gate guarantees one worker. Keep completion data
     * on its fixed 16 KiB task stack so every exit can enqueue a result even
     * when both internal RAM and PSRAM allocators are exhausted. The stack is
     * in PSRAM (see ui_submit_track_load). */
    ui_track_load_result_t result_storage = {0};
    ui_track_load_result_t *result = &result_storage;

    result->index = req.index;
    result->deck = req.deck;
    result->generation = req.generation;
    result->track_key = req.track_key;
    result->load_id = req.load_id;
    result->peer = req.peer;
    if (req.peer) {
        /* v249: the file is complete in the SD cache. There is no catalog row,
         * ANLZ, waveform or PVBR for it; the engine sizes it itself. */
        result->item = req.peer_item;
        result->loaded.track_key = req.track_key;
        result->loaded.bpm = req.peer_item.bpm;
        result->loaded.duration_ms = req.peer_item.duration_ms;
        snprintf(result->loaded.audio_path, sizeof(result->loaded.audio_path), "%s",
                 req.peer_path);
        result->rc = ESP_OK;
    } else {
        result->rc = media_catalog_load_by_identity(req.track_key,
                                                     req.generation,
                                                     &result->item,
                                                     &result->loaded);
    }
    if (result->rc == ESP_ERR_INVALID_STATE) {
        ui_track_load_set_status(result, "LIBRARY CHANGED", "LIBRARY CHANGED");
    } else if (result->rc != ESP_OK) {
        ui_track_load_set_status(result, "LOAD ERR", "LOAD ERR");
    } else if (!ui_library_track_load_is_current(req.load_id)) {
        result->rc = ESP_ERR_INVALID_STATE;
        ui_track_load_set_status(result, "LOAD CANCELLED", "LOAD CANCELLED");
    } else {
        if (req.deck == CTRL_DECK_1) {
            (void)audio_engine_deck_clear_loop(req.deck);
        }
        deck_core_reset_deck(req.deck);
        result->deck_reset = true;
        esp_err_t clear_rc =
            deck_core_clear_loaded_track(req.deck, req.generation);
        result->rc = clear_rc;
        if (clear_rc == ESP_OK) {
            result->rc = audio_engine_deck_load_session(
                req.deck,
                result->loaded.audio_path,
                result->loaded.has_pvbr ? result->loaded.pvbr : NULL,
                result->loaded.duration_ms,
                &result->audio_session_generation);
            if (result->rc == ESP_OK &&
                (media_catalog_generation() != req.generation ||
                 !ui_library_track_load_is_current(req.load_id))) {
                /* USB removal/catalog replacement can race the actual audio
                 * load after identity resolution. Retire that just-created
                 * session in the worker instead of waiting for an LVGL tick;
                 * the app-side stop_all covers the opposite ordering. */
                ui_library_release_deck_audio_session(
                    req.deck, result->audio_session_generation);
                result->rc = ESP_ERR_INVALID_STATE;
            }
        }
        if (result->rc != ESP_OK) {
            if (result->rc == ESP_ERR_INVALID_STATE) {
                ui_track_load_set_status(result,
                                         "LIBRARY CHANGED",
                                         "LIBRARY CHANGED");
            } else {
                audio_engine_deck_status_t deck_status = {0};
                const char *audio_err = NULL;
                if (audio_engine_deck_get_status(req.deck, &deck_status) ==
                    ESP_OK) {
                    audio_err = deck_status.last_error_text;
                }
                ui_track_load_set_status(result, audio_err, "AUDIO ERR");
            }
        } else {
            ui_track_load_set_status(result, "TRACK LOADED", "TRACK LOADED");
        }
    }

    if (s_track_load_result_q) {
        /* Multiple invalidated workers may finish after a reconnect. Preserve
         * every completion so a stale result cannot overwrite the active one. */
        (void)xQueueSend(s_track_load_result_q, result, portMAX_DELAY);
    }
    if (ui_diagnostics_enabled()) {
        ESP_LOGI(TAG, "ui_load stack high water=%u words",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    vTaskDeleteWithCaps(NULL);
}

/* v257: 8 results of ~3 KiB each. xQueueCreate takes the ~25 KiB from
 * internal RAM, where the dj_ui build has no such block left: every LOAD
 * ended in "NO QUEUE". The queue is only used from task context (load worker,
 * LVGL task), so PSRAM is safe there. Legacy keeps its internal queue. */
static QueueHandle_t ui_library_create_result_queue(void)
{
    return xQueueCreateWithCaps(8, sizeof(ui_track_load_result_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* peer_item / peer_path: a v249 DJ Link download (index is then -1). */
static esp_err_t ui_submit_track_load(int index, uint32_t track_key, uint32_t generation, uint8_t deck,
                                      const media_catalog_track_t *peer_item,
                                      const char *peer_path)
{
    UI_LIB_TRACE("load start: deck=%d index=%d key=0x%08x (internal free=%u, largest=%u)",
                 (int)deck, (int)index, (unsigned)track_key,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!s_track_load_result_q) {
        s_track_load_result_q = ui_library_create_result_queue();
    }
    if (!s_track_load_result_q) {
        ESP_LOGE(TAG, "load start: result queue (8 x %u B) not created (internal largest=%u)",
                 (unsigned)sizeof(ui_track_load_result_t),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ui_library_status_hold("NO QUEUE", COL_RED, 2500);
        ui_library_finish_track_load();
        return ESP_ERR_NO_MEM;
    }

    xQueueReset(s_track_load_result_q);

    ui_track_load_request_t *req = malloc(sizeof(*req));
    if (!req) {
        ESP_LOGE(TAG, "load start: malloc failed (internal free=%u, largest=%u, total free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        ui_library_status_hold("NO MEM", COL_RED, 2500);
        ui_library_finish_track_load();
        return ESP_ERR_NO_MEM;
    }
    req->index = index;
    req->deck = deck;
    req->generation = generation;
    req->track_key = track_key;
    req->load_id = ui_library_active_track_load_id();
    req->peer = peer_item && peer_path;
    if (req->peer) {
        req->peer_item = *peer_item;
        snprintf(req->peer_path, sizeof(req->peer_path), "%s", peer_path);
    }

    /* v207: the stack lives in PSRAM like ae_decode's. The per-frame mix moved
     * to IRAM (v206) and left no 16 KiB internal block. Safe here: with
     * SPIRAM_FETCH_INSTRUCTIONS + SPIRAM_RODATA, flash writes (NVS) never
     * disable the cache, and SD/FATFS I/O already runs from ae_decode's PSRAM
     * stack. */
    if (xTaskCreateWithCaps(ui_track_load_worker, "ui_load", UI_TRACK_LOAD_STACK,
                            req, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "load start: task create failed (internal free=%u, largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        free(req);
        ui_library_status_hold("NO TASK", COL_RED, 2500);
        ui_library_finish_track_load();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void ui_apply_usb_removed(void)
{
    /* Cancel the worker without releasing its single-flight slot. The worker
     * must publish/retire its exact audio session before a reconnect can start
     * another LOAD, preventing two workers from reordering deck-core writes. */
    ui_library_invalidate_track_load();
    if (ui_library_leave_playlists()) {
        ui_library_playlist_rows_changed();
    }
    ui_library_invalidate_page_cache();
    bool removed_loaded = false;
    for (uint8_t deck = 0; deck < DECK_CORE_DECK_COUNT; deck++) {
        if (s_loaded_media_valid[deck]) {
            s_loaded_media_valid[deck] = false;
            s_deck_loaded_track_valid[deck] = false;
            s_deck_loaded_track_key[deck] = 0;
            if (s_library_config.actions.clear_deck_track_info) {
                s_library_config.actions.clear_deck_track_info(deck);
            }
            if (s_library_config.actions.set_deck_anlz) {
                s_library_config.actions.set_deck_anlz(deck, NULL);
            }
            removed_loaded = true;
        }
    }
    if (removed_loaded) {
        ui_library_status_hold("USB REMOVED", COL_AMBER, 2500);
    }
}

static void ui_poll_track_load_result(void)
{
    if (!s_track_load_result_q) return;

    ui_track_load_result_t result;
    while (xQueueReceive(s_track_load_result_q, &result, 0) == pdTRUE) {
        if (!ui_library_track_load_is_current(result.load_id)) {
            ESP_LOGD(TAG, "discard stale load result id=%u active=%u",
                     (unsigned)result.load_id,
                     (unsigned)ui_library_active_track_load_id());
            ui_library_release_deck_audio_session(
                result.deck, result.audio_session_generation);
            ui_library_finish_track_load_id(result.load_id);
            continue;
        }
        bool stale = result.generation != media_catalog_generation();
        if (stale) {
            if (result.deck_reset) {
                /* The worker may have completed audio_engine_deck_load() before
                 * the catalog changed under it. Clearing only the UI would leave
                 * the engine holding a track the operator can still start from a
                 * deck the screen shows as empty — and, after a USB removal, one
                 * whose media is gone. Retire the audio session first, then the
                 * UI, so both sides agree the deck is empty. */
                if (result.audio_session_generation != 0u) {
                    ui_library_release_deck_audio_session(
                        result.deck, result.audio_session_generation);
                } else {
                    ui_library_release_deck_audio(result.deck);
                }
                ui_library_apply_empty_track(result.deck);
            }
            ui_library_status_hold("LIBRARY CHANGED", COL_AMBER, 2500);
            ui_library_finish_track_load_id(result.load_id);
            continue;
        }

        if (result.rc != ESP_OK) {
            const char *display = result.status[0] ? result.status : "LOAD ERR";
            ESP_LOGW(TAG, "track load worker failed key=0x%08x index=%d: %s",
                     (unsigned)result.track_key, result.index, esp_err_to_name(result.rc));
            if (result.deck_reset) {
                ui_library_release_deck_audio(result.deck);
                ui_library_apply_empty_track(result.deck);
            }
            ui_library_status_hold(display, COL_TEXT_DIM, 3500);
            ui_library_finish_track_load_id(result.load_id);
            continue;
        }

        if (!result.peer) {
            library_set_selected_track_index(result.index);
        }
        uint8_t deck = ui_library_deck_index(result.deck);
        const uint16_t bpm = result.loaded.bpm ? result.loaded.bpm : result.item.bpm;
        anlz_metadata_t meta_snapshot;
        memset(&meta_snapshot, 0, sizeof(meta_snapshot));
        /* The catalog's loaded ANLZ belongs to a local track, never a peer's. */
        const anlz_metadata_t *meta =
            result.peer ? NULL : ui_library_clone_loaded_anlz(&meta_snapshot);
        esp_err_t publish_rc = deck_core_publish_loaded_track(
            deck,
            result.generation,
            result.loaded.track_key,
            bpm,
            result.loaded.duration_ms,
            meta);
        if (publish_rc != ESP_OK) {
            anlz_free(&meta_snapshot);
            ui_library_release_deck_audio(deck);
            ui_library_apply_empty_track(deck);
            ui_library_status_hold("LIBRARY CHANGED", COL_AMBER, 2500);
            ui_library_finish_track_load_id(result.load_id);
            continue;
        }

        s_loaded_media[deck] = result.loaded;
        s_loaded_media_valid[deck] = true;
        s_deck_loaded_track_key[deck] = result.loaded.track_key;
        s_deck_loaded_track_valid[deck] = true;
        if (ui_diagnostics_enabled()) {
            ESP_LOGI(TAG,
                     "load result: deck=%u key=0x%08X generation=%u",
                     (unsigned)deck,
                     (unsigned)result.loaded.track_key,
                     (unsigned)result.generation);
        }
        ui_library_apply_loaded_track(deck,
                                      result.item.title,
                                      result.item.artist,
                                      result.item.key,
                                      bpm,
                                      result.loaded.duration_ms,
                                      result.loaded.waveform_low,
                                      result.loaded.has_waveform != 0,
                                      meta);
        anlz_free(&meta_snapshot);

        ESP_LOGI(TAG, "Audio: loaded deck %u: %s (autoplay off)",
                 (unsigned)result.deck + 1u, result.loaded.audio_path);
        char remote_text[32];
        const char *loaded_text = result.deck == CTRL_DECK_1 ? "D1 LOADED" : "D2 LOADED";
        if (s_remote_load_ui_id != 0u && result.load_id == s_remote_load_ui_id) {
            snprintf(remote_text, sizeof(remote_text), "D%u LOADED FROM #%u",
                     (unsigned)result.deck + 1u, (unsigned)s_remote_load_from);
            loaded_text = remote_text;
            s_remote_load_ui_id = 0u;
        } else if (s_peer_load_ui_id != 0u && result.load_id == s_peer_load_ui_id) {
            snprintf(remote_text, sizeof(remote_text), "D%u LOADED (DJ LINK)",
                     (unsigned)result.deck + 1u);
            loaded_text = remote_text;
            s_peer_load_ui_id = 0u;
        }
        ui_library_status_hold(loaded_text, COL_GREEN, 2000);
        ui_library_finish_track_load_id(result.load_id);
    }
}

#endif

#ifdef WIN32
static esp_err_t ui_library_publish_simulated_track(
    uint8_t deck,
    const library_track_t *track,
    const anlz_metadata_t *meta)
{
    if (!track) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t generation = library_generation();
    deck_core_reset_deck(deck);
    esp_err_t rc = deck_core_clear_loaded_track(deck, generation);
    if (rc != ESP_OK) {
        return rc;
    }
    return deck_core_publish_loaded_track(deck,
                                          generation,
                                          track->track_id,
                                          track->bpm,
                                          track->duration_ms,
                                          meta);
}
#endif

#ifndef WIN32
/* v249: LOAD on a peer row starts the NFS download into the SD cache; the
 * deck is loaded only once the file is complete (ui_library_poll_peer_fetch).
 * Refusals happen before the load gate, so no deck or audio state is
 * touched. The engine reads a track's file only while the local USB is
 * mounted (media_io_gate), so a cached peer file needs it too. */
static void ui_library_peer_load(uint8_t deck)
{
    dj_link_peer_track_t t;
    bool have = ui_library_peer_status_current() &&
                dj_link_browse_get_track(s_peer_status.generation,
                                         (uint32_t)s_selected_track_idx, &t);
    char reason[96];
    dj_link_peer_load_t verdict = dj_link_peer_load_check(have ? &t : NULL, reason,
                                                          sizeof(reason));
    const char *refusal = NULL;
    if (verdict != DJ_LINK_PEER_LOAD_OK) {
        refusal = verdict == DJ_LINK_PEER_LOAD_NO_TRACK ? "NO TRACK" : "METADATA ONLY - NO AUDIO";
    } else if (ui_library_peer_fetch_active()) {
        refusal = "DOWNLOAD BUSY";
    } else if (!media_io_gate_is_available()) {
        refusal = "LOCAL USB NEEDED";
    } else if (ui_library_track_load_busy() || media_catalog_load_in_progress()) {
        refusal = "LOAD BUSY";
    }
    if (refusal) {
        ESP_LOGW(TAG, "peer load refused: deck %u <- player #%u rekordbox id %u \"%s\": %s",
                 (unsigned)deck + 1u, (unsigned)s_library_peer,
                 have ? (unsigned)t.rekordbox_id : 0u, have ? t.title : "",
                 verdict != DJ_LINK_PEER_LOAD_OK ? reason : refusal);
        ui_library_status_hold(refusal, COL_AMBER, 2500);
        return;
    }
    /* Cached peer files the decks hold must survive the cache pruning. */
    uint32_t keep[DECK_CORE_DECK_COUNT] = {0};
    for (uint8_t d = 0; d < DECK_CORE_DECK_COUNT; d++) {
        if (s_loaded_media_valid[d] &&
            strncmp(s_loaded_media[d].audio_path, UI_PEER_CACHE_PREFIX,
                    sizeof(UI_PEER_CACHE_PREFIX) - 1u) == 0) {
            keep[d] = s_loaded_media[d].track_key;
        }
    }
    uint32_t id = dj_link_fetch_start(s_library_peer, t.rekordbox_id, keep);
    if (id == 0u) {
        ui_library_status_hold("DJ LINK BUSY", COL_AMBER, 2000);
        return;
    }
    memset(&s_peer_fetch, 0, sizeof(s_peer_fetch));
    s_peer_fetch.id = id;
    s_peer_fetch.deck = deck;
    s_peer_fetch.peer = s_library_peer;
    s_peer_fetch.rekordbox_id = t.rekordbox_id;
    s_peer_fetch.state = DJ_LINK_FETCH_PDB;
    snprintf(s_peer_fetch.item.title, sizeof(s_peer_fetch.item.title), "%s",
             t.title[0] ? t.title : "Unknown Title");
    snprintf(s_peer_fetch.item.artist, sizeof(s_peer_fetch.item.artist), "%s", t.artist);
    s_peer_fetch.item.rekordbox_track_id = t.rekordbox_id;
    s_peer_fetch.item.bpm = (uint16_t)((t.bpm100 + 50u) / 100u);
    s_peer_fetch.item.duration_ms = (uint32_t)t.duration_s * 1000u;
    ESP_LOGW(TAG, "peer load: deck %u <- player #%u rekordbox id %u \"%s\" - downloading",
             (unsigned)deck + 1u, (unsigned)s_library_peer, (unsigned)t.rekordbox_id,
             s_peer_fetch.item.title);
    ui_library_status_hold("DOWNLOADING", COL_ACCENT, 1500);
    ui_library_populate_rows();
}
#endif

static void ui_library_load_selected_deck(uint8_t deck)
{
#ifndef WIN32
    if (ui_library_peer_view()) {
        ui_library_peer_load(deck);
        return;
    }
#endif
    if (s_lib_mode == UI_LIB_MODE_PLAYLISTS) {
        /* LOAD on a playlist row opens it; nothing reaches a deck. */
        ui_library_open_playlist(s_selected_track_idx);
        return;
    }
    const int catalog_idx = ui_library_catalog_row(s_selected_track_idx);
    if (catalog_idx < 0) {
        ui_library_status_hold("NOT ON USB", COL_AMBER, 1200);
        return;
    }
    if (!ui_library_try_begin_track_load()) {
        UI_LIB_TRACE("load D%u: refused, load gate busy", (unsigned)deck + 1u);
        ui_library_status_hold("LOAD BUSY", COL_AMBER, 1200);
        return;
    }

#ifdef WIN32
    library_track_t *track = library_get_ptr(catalog_idx);
    if (!track) {
        ui_library_finish_track_load();
        return;
    }

    library_set_selected_track_index(catalog_idx);
    library_load_anlz(track);
    anlz_metadata_t meta_snapshot;
    const anlz_metadata_t *meta = ui_library_clone_loaded_anlz(&meta_snapshot);
    if (ui_library_publish_simulated_track(deck, track, meta) != ESP_OK) {
        anlz_free(&meta_snapshot);
        ui_library_apply_empty_track(deck);
        ui_library_finish_track_load();
        return;
    }
    s_deck_loaded_track_key[deck] = track->track_id;
    s_deck_loaded_track_valid[deck] = true;
    ui_library_apply_loaded_track(deck,
                                  track->title,
                                  track->artist,
                                  track->key,
                                  track->bpm,
                                  track->duration_ms,
                                  track->waveform_low,
                                  track->has_waveform != 0,
                                  meta);
    anlz_free(&meta_snapshot);

    ESP_LOGI(TAG, "Loaded track to deck %u: %s by %s (waveform=%d)",
             (unsigned)deck + 1u, track->title, track->artist, track->has_waveform);
#else
    const uint32_t generation = media_catalog_generation();
    media_catalog_track_t item;
    if (media_catalog_get(catalog_idx, &item) != ESP_OK ||
        media_catalog_generation() != generation) {
        ESP_LOGW(TAG, "Catalog changed while resolving index %d", catalog_idx);
        ui_library_finish_track_load();
        return;
    }

    ui_library_status_hold("LOADING", COL_ACCENT, 1500);
    (void)ui_submit_track_load(catalog_idx, item.track_key, generation, deck,
                               NULL, NULL);
    return;
#endif
    ui_library_status_hold("TRACK LOADED", COL_GREEN, 2000);
    ui_library_finish_track_load();
}

static void library_load_event_cb(lv_event_t *e)
{
    uint8_t deck = s_library_load_request_deck;
    if (e) {
        lv_obj_t *btn = lv_event_get_target(e);
        deck = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
    }
    ui_library_load_selected_deck(deck);
}

static void ui_library_preserve_selection_by_key(uint32_t target_key)
{
    if (target_key == 0) {
        return;
    }
    /* Runs on the LVGL task after every sort. Resolving through the identity
     * index keeps it a single locked pass instead of one full-record copy per
     * row, and firmware and simulator now agree on what "same track" means
     * (library_track_key, i.e. path hash when the PDB has no track_id). */
#ifndef WIN32
    int row = media_catalog_find_index_by_key(target_key);
#else
    int row = library_find_row_by_key(target_key);
#endif
    if (row >= 0) {
        s_selected_track_idx = row;
        ui_library_invalidate_page_cache();
    }
}

static uint32_t ui_library_selected_key(void)
{
#ifndef WIN32
    uint32_t key = 0;
    return (media_catalog_row_key(s_selected_track_idx, &key) == ESP_OK) ? key : 0;
#else
    library_track_t *sel_track = library_get_ptr(s_selected_track_idx);
    return sel_track ? sel_track->track_id : 0;
#endif
}

/* Peer lists come in the player's order; sorting is local only. */
static bool ui_library_sort_refused(void)
{
    if (!ui_library_peer_view() && s_lib_mode != UI_LIB_MODE_ALL) {
        /* Playlists keep the rekordbox order. */
        ui_library_status_hold("SORT: ALL TRACKS ONLY", COL_AMBER, 1200);
        return true;
    }
    if (!ui_library_peer_view()) {
        return false;
    }
    ui_library_status_hold("SORT: LOCAL ONLY", COL_AMBER, 1200);
    return true;
}

static void ui_library_page_delta(int page_delta)
{
    int count = ui_library_view_count();
    int new_idx = ui_library_page_selection_after_delta(count,
                                                         s_selected_track_idx,
                                                         page_delta);
    if (count > 0 && new_idx != s_selected_track_idx) {
        s_selected_track_idx = new_idx;
        ui_library_populate_rows();
    }
}

#ifndef WIN32
/* Switch the table between our USB (0) and a DJ Link player's USB. Local
 * selection is kept across the round trip. */
static void ui_library_set_source(uint8_t peer)
{
    if (peer == s_library_peer) {
        return;
    }
    if (s_library_peer == 0u) {
        (void)ui_library_leave_playlists();
        s_local_selected_idx = s_selected_track_idx;
    }
    dj_link_browse_get_status(&s_peer_status);
    s_peer_select_generation = s_peer_status.generation;
    s_peer_want_first = -1;
    s_library_peer = peer;
    dj_link_browse_select(peer);
    if (peer != 0u) {
        s_selected_track_idx = 0;
    } else {
        int n = ui_library_media_count();
        s_selected_track_idx = s_local_selected_idx < n ? s_local_selected_idx : 0;
    }
    ui_library_invalidate_page_cache();
    ui_library_populate_rows();
}

/* SOURCE: LOCAL -> each DJ Link player, by number -> LOCAL. */
static void library_source_event_cb(lv_event_t *e)
{
    (void)e;
    if (ui_library_peer_fetch_active()) {
        /* v249: SOURCE reads CANCEL while a peer track downloads. */
        dj_link_fetch_cancel(s_peer_fetch.id);
        ui_library_status_hold("CANCELLING", COL_AMBER, 1200);
        return;
    }
    dj_link_summary_t summary;
    dj_link_get_summary(&summary);
    const dj_link_player_t *next = NULL;
    if (summary.state != DJ_LINK_STATE_OFF) {
        for (uint8_t i = 0; i < summary.player_count; i++) {
            if (summary.players[i].number > s_library_peer) {
                next = &summary.players[i];
                break;
            }
        }
    }
    if (!next && s_library_peer == 0u) {
        ui_library_status_hold(summary.state == DJ_LINK_STATE_OFF ? "DJ LINK OFF"
                                                                  : "NO DJ LINK PLAYERS",
                               COL_AMBER, 1500);
        return;
    }
    ui_library_set_source(next ? next->number : 0u);
    char status[40];
    if (next) {
        snprintf(status, sizeof(status), "USB %s #%u", next->name, (unsigned)next->number);
        ESP_LOGW(TAG, "Library source: USB of player #%u %s (LOAD downloads over NFS)",
                 (unsigned)next->number, next->name);
    } else {
        snprintf(status, sizeof(status), "LOCAL USB");
        ESP_LOGW(TAG, "Library source: local USB");
    }
    ui_library_status_hold(status, COL_ACCENT, 1200);
}

static void ui_library_peer_fetch_clear(void)
{
    memset(&s_peer_fetch, 0, sizeof(s_peer_fetch));
    ui_library_populate_rows();
}

/* The download is complete: load the cached file like a local track. */
static void ui_library_load_peer_file(const dj_link_fetch_status_t *st)
{
    uint8_t deck = s_peer_fetch.deck;
    const char *refusal = NULL;
    if (!media_io_gate_is_available()) {
        refusal = "LOCAL USB NEEDED";
    } else if (media_catalog_load_in_progress() || !ui_library_try_begin_track_load()) {
        refusal = "LOAD BUSY";
    }
    if (refusal) {
        /* The file stays cached: LOAD again finds it without downloading. */
        ESP_LOGW(TAG, "peer load: %s ready but %s", st->path, refusal);
        ui_library_status_hold(refusal, COL_AMBER, 2500);
        return;
    }
    media_catalog_track_t item = s_peer_fetch.item;
    item.track_key = st->track_key;
    ESP_LOGW(TAG, "peer load: deck %u <- %s%s", (unsigned)deck + 1u, st->path,
             st->cache_hit ? " (cached)" : "");
    if (ui_submit_track_load(-1, st->track_key, media_catalog_generation(), deck,
                             &item, st->path) == ESP_OK) {
        s_peer_load_ui_id = ui_library_active_track_load_id();
        ui_library_status_hold("LOADING", COL_ACCENT, 1500);
    }
}

/* ui_update(): follow the download started by a peer LOAD. */
static void ui_library_poll_peer_fetch(void)
{
    if (!ui_library_peer_fetch_active()) {
        return;
    }
    dj_link_fetch_status_t st;
    dj_link_fetch_get_status(&st);
    if (st.id != s_peer_fetch.id) {
        ui_library_peer_fetch_clear();
        return;
    }
    if (st.state == DJ_LINK_FETCH_PDB || st.state == DJ_LINK_FETCH_AUDIO) {
        uint32_t now_ms = lv_tick_get();
        if ((st.state != s_peer_fetch.state || st.percent != s_peer_fetch.percent) &&
            (uint32_t)(now_ms - s_peer_repaint_ms) >= UI_PEER_REPAINT_MS) {
            s_peer_fetch.state = st.state;
            s_peer_fetch.percent = st.percent;
            s_peer_repaint_ms = now_ms;
            ui_library_populate_rows();
        }
        return;
    }
    if (st.state == DJ_LINK_FETCH_DONE) {
        ui_library_load_peer_file(&st);
    } else if (st.state == DJ_LINK_FETCH_CANCELLED) {
        ui_library_status_hold("DOWNLOAD CANCELLED", COL_AMBER, 2000);
    } else {
        char text[48];
        snprintf(text, sizeof(text), "%s", st.error[0] ? st.error : "DOWNLOAD FAILED");
        ESP_LOGW(TAG, "peer load: download failed: %s", text);
        ui_library_status_hold(text, COL_RED, 3500);
    }
    ui_library_peer_fetch_clear();
}

/* ui_update(): follow the dj_link cache. Rows are re-read only when the
 * list, its metadata or the selection generation moved. */
static void ui_library_poll_peer_browse(void)
{
    if (s_library_peer == 0u) {
        return;
    }
    dj_link_browse_status_t st;
    dj_link_browse_get_status(&st);
    bool changed = st.generation != s_peer_status.generation ||
                   st.state != s_peer_status.state || st.count != s_peer_status.count ||
                   st.total != s_peer_status.total ||
                   st.peer != s_peer_status.peer ||
                   strcmp(st.error, s_peer_status.error) != 0;
    /* Metadata replies trickle in row by row: batch their repaint. */
    uint32_t now_ms = lv_tick_get();
    if (!changed && st.detail_seq != s_peer_status.detail_seq &&
        (uint32_t)(now_ms - s_peer_repaint_ms) >= UI_PEER_REPAINT_MS) {
        changed = true;
    }
    if (!changed) {
        st.detail_seq = s_peer_status.detail_seq;
    }
    s_peer_status = st;
    if (changed) {
        s_peer_repaint_ms = now_ms;
        ui_library_populate_rows();
    }
    if (ui_library_peer_status_current() && st.state == DJ_LINK_BROWSE_LISTED) {
        ui_library_page_t page = ui_library_page_cached();
        if (page.first_index != s_peer_want_first ||
            st.generation != s_peer_want_generation) {
            s_peer_want_first = page.first_index;
            s_peer_want_generation = st.generation;
            dj_link_browse_want_details(st.generation, (uint32_t)page.first_index,
                                        (uint32_t)page.row_count);
        }
    }
}
#endif

void ui_library_init(const ui_library_config_t *config)
{
    memset(&s_library_config, 0, sizeof(s_library_config));
    ui_load_gate_reset(&s_track_load_gate);
    ui_event_counter_reset(&s_library_refresh_events);
    s_library_refresh_applied = 0u;
#ifndef WIN32
    ui_event_counter_reset(&s_usb_removed_events);
    s_usb_removed_applied = 0u;
#endif
    if (config) {
        s_library_config = *config;
    }
    if (!s_track_load_result_q) {
        s_track_load_result_q = ui_library_create_result_queue();
        if (!s_track_load_result_q) {
            ESP_LOGE(TAG, "track-load result queue (8 x %u B) not created",
                     (unsigned)sizeof(ui_track_load_result_t));
        }
    }
}

void ui_library_load_initial_track(void)
{
    library_set_selected_track_index(0);
#ifdef WIN32
    library_track_t *track0 = library_get_ptr(0);
    if (track0) {
        library_load_anlz(track0);
        anlz_metadata_t meta_snapshot;
        const anlz_metadata_t *meta = ui_library_clone_loaded_anlz(&meta_snapshot);
        if (ui_library_publish_simulated_track(
                CTRL_DECK_1, track0, meta) == ESP_OK) {
            s_deck_loaded_track_key[CTRL_DECK_1] = track0->track_id;
            s_deck_loaded_track_valid[CTRL_DECK_1] = true;
            ui_library_apply_loaded_track(CTRL_DECK_1,
                                          track0->title,
                                          track0->artist,
                                          track0->key,
                                          track0->bpm,
                                          track0->duration_ms,
                                          track0->waveform_low,
                                          track0->has_waveform != 0,
                                          meta);
        } else {
            ui_library_apply_empty_track(CTRL_DECK_1);
        }
        anlz_free(&meta_snapshot);
    }
    library_track_t *track1 = library_get_ptr(1);
    if (track1) {
        library_load_anlz(track1);
        anlz_metadata_t meta_snapshot;
        const anlz_metadata_t *meta = ui_library_clone_loaded_anlz(&meta_snapshot);
        if (ui_library_publish_simulated_track(
                CTRL_DECK_2, track1, meta) == ESP_OK) {
            s_deck_loaded_track_key[CTRL_DECK_2] = track1->track_id;
            s_deck_loaded_track_valid[CTRL_DECK_2] = true;
            ui_library_apply_loaded_track(CTRL_DECK_2,
                                          track1->title,
                                          track1->artist,
                                          track1->key,
                                          track1->bpm,
                                          track1->duration_ms,
                                          track1->waveform_low,
                                          track1->has_waveform != 0,
                                          meta);
        } else {
            ui_library_apply_empty_track(CTRL_DECK_2);
        }
        anlz_free(&meta_snapshot);
    }
#else
    ui_library_apply_empty_track(CTRL_DECK_1);
    ui_library_apply_empty_track(CTRL_DECK_2);
#endif
}

void ui_trigger_library_refresh(void)
{
    (void)ui_event_counter_request(&s_library_refresh_events);
}

void ui_notify_usb_removed(void)
{
#ifndef WIN32
    (void)ui_event_counter_request(&s_usb_removed_events);
#endif
}

void ui_refresh_library(void)
{
    int n = ui_library_media_count();

#ifndef WIN32
    if (ui_diagnostics_enabled()) {
        ESP_LOGI(TAG, "ui_refresh_library start. Free SRAM: %d B, SPIRAM: %d B",
                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
#endif

    ui_lvgl_lock();
    /* The playlist set was rebuilt with the catalog. */
    (void)ui_library_leave_playlists();
    s_all_selected_idx = 0;
    s_pl_list_selected_idx = 0;
    if (ui_library_peer_view()) {
        /* Our catalog changed under a peer view: the peer rows stay. */
        s_local_selected_idx = 0;
    } else {
        s_selected_track_idx = 0;
        ui_library_populate_rows();
    }
    ui_lvgl_unlock();

#ifndef WIN32
    if (ui_diagnostics_enabled()) {
        ESP_LOGI(TAG, "ui_refresh_library end. Free SRAM: %d B, SPIRAM: %d B",
                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
#endif

    ESP_LOGI(TAG, "library table refreshed: %d tracks", n);
}

bool ui_is_library_active(void)
{
    return s_active_tab == 1;
}

esp_err_t ui_library_select_delta(int delta)
{
    if (delta == 0) {
        return ESP_OK;
    }
    ui_lvgl_lock();
    int n = ui_library_view_count();
    if (n <= 0) {
        ui_lvgl_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    int new_idx = s_selected_track_idx + delta;
    if (new_idx < 0) new_idx = 0;
    if (new_idx >= n) new_idx = n - 1;
    if (new_idx == s_selected_track_idx) {
        ui_library_select_visible_cell();
        ui_lvgl_unlock();
        return ESP_OK;
    }

    ui_library_page_t old_page = ui_library_current_page();
    s_selected_track_idx = new_idx;
    ui_library_page_t new_page = ui_library_current_page();
    if (new_page.page_index != old_page.page_index) {
        ui_library_populate_rows();
    } else {
        ui_library_select_visible_cell();
    }
    ui_lvgl_unlock();
    return ESP_OK;
}

esp_err_t ui_library_load_selected(void)
{
    return ui_library_load_selected_for_deck(CTRL_DECK_1);
}

esp_err_t ui_library_load_selected_for_deck(uint8_t deck)
{
    /* dj_ui presentation (migration phase 1) builds no library table; the
     * load pipeline below is widget-independent, so controller LOAD still
     * loads the selected catalog row there. */
    int count = ui_library_view_count();
    UI_LIB_TRACE("load selected D%u: index %d of %d%s%s", (unsigned)deck + 1u,
                 s_selected_track_idx, count, ui_library_peer_view() ? " (peer)" : "",
                 ui_library_track_load_busy() ? ", load busy" : "");
    if (count <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_lib_mode != UI_LIB_MODE_PLAYLISTS && ui_library_track_load_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    ui_lvgl_lock();
    uint8_t old_deck = s_library_load_request_deck;
    s_library_load_request_deck = deck;
    library_load_event_cb(NULL);
    s_library_load_request_deck = old_deck;
    ui_lvgl_unlock();
    return ESP_OK;
}

#ifndef WIN32
/* v247: a DJ Link player asked us to load one of our own USB tracks (0x19,
 * validated by dj_link). Runs in ui_update(), through the same single-flight
 * load path as the LOAD buttons; never touches a playing deck. */
static void ui_library_poll_dj_link(const ui_frame_context_t *ctx)
{
    uint32_t now_ms = lv_tick_get();
    if ((uint32_t)(now_ms - s_dj_link_count_ms) >= UI_DJ_LINK_COUNT_MS) {
        s_dj_link_count_ms = now_ms;
        int count = ui_library_media_count();
        dj_link_set_local_track_count(count > 0 ? (uint32_t)count : 0u);
        if (s_library_peer != 0u) {
            dj_link_summary_t summary;
            dj_link_get_summary(&summary);
            bool present = false;
            for (uint8_t i = 0; summary.state != DJ_LINK_STATE_OFF &&
                                i < summary.player_count; i++) {
                present = present || summary.players[i].number == s_library_peer;
            }
            if (!present) {
                ESP_LOGW(TAG, "%s - Library back to the local USB",
                         summary.state == DJ_LINK_STATE_OFF ? "DJ Link off"
                                                            : "player left DJ Link");
                ui_library_set_source(0u);
                ui_library_status_hold("LOCAL USB", COL_AMBER, 1500);
            }
        }
    }
    ui_library_poll_peer_browse();
    ui_library_poll_peer_fetch();

    dj_link_load_request_t req;
    if (!ctx || !dj_link_take_load_request(&req)) {
        return;
    }
    const char *refusal = NULL;
    esp_err_t rc = ESP_FAIL;
    int deck = dj_link_pick_target_deck(ctx->deck_state[CTRL_DECK_1].playing,
                                        ctx->deck_state[CTRL_DECK_2].playing,
                                        s_deck_loaded_track_valid[CTRL_DECK_1],
                                        s_deck_loaded_track_valid[CTRL_DECK_2]);
    if (deck < 0) {
        refusal = "both decks playing";
    } else if (ui_library_track_load_busy() || media_catalog_load_in_progress()) {
        refusal = "load busy";
    } else {
        /* library_track_key() is the rekordbox id when the PDB has one;
         * re-check the id so a path-hash key can never alias it. */
        const uint32_t generation = media_catalog_generation();
        int index = media_catalog_find_index_by_key(req.rekordbox_id);
        media_catalog_track_t item;
        if (index < 0 || media_catalog_get(index, &item) != ESP_OK ||
            item.rekordbox_track_id != req.rekordbox_id ||
            media_catalog_generation() != generation) {
            refusal = "track not in our library";
        } else {
            rc = ui_library_load_track_identity_for_deck(item.track_key, generation,
                                                         (uint8_t)deck);
            if (rc != ESP_OK) {
                refusal = esp_err_to_name(rc);
            }
        }
    }
    dj_link_finish_load_request(req.id, rc == ESP_OK);

    char status[32];
    if (rc == ESP_OK) {
        s_remote_load_ui_id = ui_library_active_track_load_id();
        s_remote_load_from = req.from_number;
        ESP_LOGW(TAG, "DJ Link load from #%u: rekordbox id %u -> deck %d",
                 (unsigned)req.from_number, (unsigned)req.rekordbox_id, deck + 1);
        snprintf(status, sizeof(status), "LOADING FROM #%u", (unsigned)req.from_number);
        ui_library_status_hold(status, COL_ACCENT, 1500);
    } else {
        ESP_LOGW(TAG, "DJ Link load from #%u: rekordbox id %u refused: %s",
                 (unsigned)req.from_number, (unsigned)req.rekordbox_id, refusal);
        snprintf(status, sizeof(status), "REFUSED LOAD #%u", (unsigned)req.from_number);
        ui_library_status_hold(status, COL_AMBER, 2000);
    }
}
#endif

/* v256: this model drives the dj_ui Library page.
 * The page text is rebuilt only when ui_library_populate_rows() marked it,
 * i.e. exactly when the legacy table would have been refilled; everything
 * else is plain state read per frame. Buffers live in PSRAM, allocated once. */
typedef struct {
    ui_library_row_text_t text[UI_LIBRARY_PAGE_ROWS];
    char badge[UI_LIBRARY_PAGE_ROWS][8];
    uint32_t key[UI_LIBRARY_PAGE_ROWS];      /* catalog key, 0 = peer row */
    dj_track_t rows[UI_LIBRARY_PAGE_ROWS];
    const uint16_t *art[UI_LIBRARY_PAGE_ROWS]; /* thumbnail pushed per row, NULL = none yet */
    int count;                               /* rows pushed at the last rebuild */
    int total;                               /* view count at the last rebuild */
} ui_library_djui_page_t;

static ui_library_djui_page_t *s_djui_page;
/* Catalog order the dj_ui page shows. A catalog reload restarts in load
 * order, so the sort only holds for the generation it was applied to. */
static int s_djui_sort_field = -1;           /* media_catalog_sort field, -1 = load order */
static bool s_djui_sort_desc;
static uint32_t s_djui_sort_generation;

static void ui_library_djui_fill_row(int visible_row, int track_index)
{
    ui_library_djui_page_t *p = s_djui_page;
    ui_library_row_text_t *text = &p->text[visible_row];
    dj_track_t *row = &p->rows[visible_row];
    memset(row, 0, sizeof(*row));
    p->key[visible_row] = 0;
    p->badge[visible_row][0] = '\0';
    ui_library_format_row_text(text, "", "", "", 0, 0);

    /* Title and artist go in whole (dj_ui ellipsizes to the column width),
     * fitted to what its fonts can draw. */
    if (ui_library_peer_view()) {
        dj_link_peer_track_t t;
        if (ui_library_peer_status_current() &&
            dj_link_browse_get_track(s_peer_status.generation, (uint32_t)track_index, &t)) {
            ui_library_peer_badge(&t, p->badge[visible_row], sizeof(p->badge[visible_row]));
            if (ui_library_peer_fetch_active() && s_peer_fetch.peer == s_library_peer &&
                s_peer_fetch.rekordbox_id == t.rekordbox_id) {
                row->badge_tone = DJ_TONE_OK;
            } else if (t.audio != DJ_LINK_PEER_AUDIO_NFS) {
                row->badge_tone = DJ_TONE_MUTED;
            }
            ui_djui_text_fit(text->title, sizeof(text->title), t.title[0] ? t.title : "Unknown Title");
            ui_djui_text_fit(text->artist, sizeof(text->artist), t.artist);
            /* Same placeholders as the legacy peer table. */
            if (!t.has_detail) {
                snprintf(text->bpm, sizeof(text->bpm), "...");
                snprintf(text->duration, sizeof(text->duration), "...");
            } else {
                row->bpm = (float)t.bpm100 / 100.0f;
                row->len_ms = (uint32_t)t.duration_s * 1000u;
                if (t.bpm100 == 0u) snprintf(text->bpm, sizeof(text->bpm), "--");
                if (t.duration_s == 0u) snprintf(text->duration, sizeof(text->duration), "--");
            }
            row->bpm_text = t.has_detail && t.bpm100 != 0u ? NULL : text->bpm;
            row->time_text = t.has_detail && t.duration_s != 0u ? NULL : text->duration;
        }
    } else if (s_lib_mode == UI_LIB_MODE_PLAYLISTS) {
        /* Playlist list: name, its folder, track count in the TIME column. */
        library_playlist_info_t info;
        if (library_playlist_get(track_index, &info) == ESP_OK) {
            ui_djui_text_fit(text->title, sizeof(text->title), info.name);
            ui_djui_text_fit(text->artist, sizeof(text->artist), info.folder);
            text->key[0] = '\0';
            text->bpm[0] = '\0';
            snprintf(text->duration, sizeof(text->duration), "%u TR",
                     (unsigned)info.track_count);
            row->bpm_text = text->bpm;
            row->time_text = text->duration;
        }
    } else {
        media_catalog_row_t r;
        track_index = ui_library_catalog_row(track_index);
        if (track_index >= 0 && media_catalog_get_row(track_index, &r) == ESP_OK) {
            ui_library_format_row_text(text, "", "", r.key, r.bpm, r.duration_ms);
            ui_djui_text_fit(text->title, sizeof(text->title), r.title);
            ui_djui_text_fit(text->artist, sizeof(text->artist), r.artist);
            ui_djui_text_fit(text->key, sizeof(text->key), text->key);
            row->bpm = (float)r.bpm;
            row->len_ms = r.duration_ms;
            p->key[visible_row] = r.track_key;
        }
    }
    row->title = text->title;
    row->artist = text->artist;
    row->key = text->key;
    row->badge = p->badge[visible_row][0] ? p->badge[visible_row] : NULL;
}

static void ui_library_djui_source_text(char *out, size_t len)
{
    if (!ui_library_peer_view()) {
        if (s_lib_mode == UI_LIB_MODE_PLAYLISTS) {
            snprintf(out, len, "PLAYLISTS");
        } else if (s_lib_mode == UI_LIB_MODE_PLAYLIST_TRACKS) {
            char name[21];
            ui_djui_text_fit(name, sizeof(name), s_pl_name);
            snprintf(out, len, "PLAYLIST %s", name);
        } else {
            snprintf(out, len, "LOCAL USB");
        }
        return;
    }
    const dj_link_browse_status_t *st = &s_peer_status;
    /* Fitted before the cut, so a player name never ends mid-sequence. */
    char name[21];
    ui_djui_text_fit(name, sizeof(name), st->peer_name[0] ? st->peer_name : "PLAYER");
    if (!ui_library_peer_status_current()) {
        snprintf(out, len, "USB #%u  CONNECTING...", (unsigned)s_library_peer);
        return;
    }
    switch (st->state) {
    case DJ_LINK_BROWSE_WAITING:
        snprintf(out, len, "USB %s #%u  WAITING FOR DJ LINK", name, (unsigned)st->peer);
        break;
    case DJ_LINK_BROWSE_LOADING:
        snprintf(out, len, "USB %s #%u  LOADING %u/%u", name, (unsigned)st->peer,
                 (unsigned)st->count, (unsigned)st->total);
        break;
    case DJ_LINK_BROWSE_LISTED:
        if (!ui_library_peer_fetch_active()) {
            snprintf(out, len, "USB %s #%u  LOAD = DOWNLOAD", name, (unsigned)st->peer);
        } else {
            snprintf(out, len, "USB %s #%u  %s %u%%", name, (unsigned)st->peer,
                     s_peer_fetch.state == DJ_LINK_FETCH_AUDIO ? "DOWNLOAD" : "READING DB",
                     (unsigned)s_peer_fetch.percent);
        }
        break;
    case DJ_LINK_BROWSE_FAILED:
        snprintf(out, len, "USB %s #%u  ERROR: %.16s", name, (unsigned)st->peer, st->error);
        break;
    default:
        snprintf(out, len, "USB #%u", (unsigned)st->peer);
        break;
    }
}

/* ui_library_update(), LVGL task. */
static void ui_library_djui_publish(const ui_frame_context_t *ctx)
{
    if (!s_djui_page) {
        s_djui_page = heap_caps_calloc(1, sizeof(*s_djui_page),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_djui_page) {
            ESP_LOGE(TAG, "dj_ui library page: no PSRAM");
            return;
        }
    }
    ui_djui_library_view_t v = {
        .selected = -1,
        .loaded = {-1, -1},
        .progress = -1,
    };
    /* Thumbnails: USB reads wait while a track load owns the stick. */
    ui_artwork_set_paused(ui_library_track_load_busy());
    bool art_changed = ui_artwork_poll();
    ui_library_page_t page;
    if (s_djui_rows_dirty) {
        s_djui_rows_dirty = false;
        page = ui_library_refresh_page_cache();
        s_djui_page->total = ui_library_view_count();
        int count = page.row_count < UI_LIBRARY_PAGE_ROWS ? page.row_count : UI_LIBRARY_PAGE_ROWS;
        ui_artwork_begin_page();
        for (int r = 0; r < count; r++) {
            ui_library_djui_fill_row(r, ui_library_page_absolute_index(&page, r));
            uint32_t key = s_djui_page->key[r];
            s_djui_page->art[r] = key ? ui_artwork_get(key, UI_ARTWORK_ROW) : NULL;
            s_djui_page->rows[r].art = s_djui_page->art[r];
        }
        s_djui_page->count = count;
        ui_djui_bridge_library_set_rows(s_djui_page->rows, (uint8_t)count);
    } else {
        page = ui_library_page_cached();
        /* Lazy load: rows pushed before their thumbnail was decoded. */
        for (int r = 0; art_changed && r < s_djui_page->count; r++) {
            uint32_t key = s_djui_page->key[r];
            const uint16_t *art = key ? ui_artwork_get(key, UI_ARTWORK_ROW) : NULL;
            if (art == s_djui_page->art[r]) continue;
            s_djui_page->art[r] = art;
            ui_djui_bridge_library_set_row_art((uint8_t)r, art);
        }
    }

    char source[UI_DJUI_LIB_TEXT_LEN];
    ui_library_djui_source_text(source, sizeof(source));
    v.source = source;
    v.total = (uint16_t)(s_djui_page->total > 0 ? s_djui_page->total : 0);
    v.unit = s_lib_mode == UI_LIB_MODE_PLAYLISTS ? "PLAYLISTS" : "TRACKS";
    v.pages = (uint16_t)page.page_count;
    v.page = page.page_count > 0 ? (uint16_t)(page.page_index + 1) : 0u;
    if (page.row_count > 0) {
        v.selected = (int8_t)page.selected_row;
    }
    /* Peer ids are another player's and never match what our decks loaded. */
    for (uint8_t d = 0; d < DJ_DECKS && d < DECK_CORE_DECK_COUNT && !ui_library_peer_view(); d++) {
        if (!s_deck_loaded_track_valid[d] || s_deck_loaded_track_key[d] == 0u) continue;
        for (int r = 0; r < page.row_count && r < UI_LIBRARY_PAGE_ROWS; r++) {
            if (s_djui_page->key[r] == s_deck_loaded_track_key[d]) {
                v.loaded[d] = (int8_t)r;
                break;
            }
        }
    }

    /* Same rule as the legacy ACTIVE / READY indicator. */
    bool d1 = ctx && ctx->deck_state[CTRL_DECK_1].playing;
    bool d2 = ctx && ctx->deck_state[CTRL_DECK_2].playing;
    if (d1 && d2) {
        v.status_deck = DJ_DECKS;
        v.deck_status = "ACTIVE";
    } else if (d1 || d2) {
        v.status_deck = d1 ? CTRL_DECK_1 : CTRL_DECK_2;
        v.deck_status = "ACTIVE";
    } else {
        v.status_deck = ctx && ctx->active_deck == CTRL_DECK_2 ? CTRL_DECK_2 : CTRL_DECK_1;
        v.deck_status = "READY";
    }

    /* Peer lists keep the player's order: no column is lit there. */
    if (s_djui_sort_field >= 0 && s_djui_sort_generation != media_catalog_generation()) {
        UI_LIB_TRACE("dj_ui sort cleared: catalog reloaded in load order");
        s_djui_sort_field = -1;
    }
    if (!ui_library_peer_view() && s_lib_mode == UI_LIB_MODE_ALL && s_djui_sort_field >= 0) {
        v.sort = (dj_sort_t)(s_djui_sort_field + 1);
        v.sort_desc = s_djui_sort_desc;
    }

    v.load_enabled = !ui_library_track_load_busy();
    static int8_t s_traced_enabled = -1;
    static int8_t s_traced_loaded[DJ_DECKS] = {-2, -2};
    if (s_traced_enabled != (int8_t)v.load_enabled) {
        s_traced_enabled = (int8_t)v.load_enabled;
        UI_LIB_TRACE("dj_ui LOAD buttons %s", v.load_enabled ? "enabled" : "disabled (load busy)");
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        if (s_traced_loaded[d] == v.loaded[d]) continue;
        s_traced_loaded[d] = v.loaded[d];
        UI_LIB_TRACE("dj_ui D%u loaded row %d (key 0x%08x%s)", (unsigned)d + 1u, (int)v.loaded[d],
                     (unsigned)s_deck_loaded_track_key[d],
                     s_deck_loaded_track_valid[d] ? "" : ", none");
    }
    if (ui_library_peer_fetch_active()) {
        v.progress = (int16_t)s_peer_fetch.percent;
        v.source_label = "CANCEL";
    } else {
        v.source_label = "SOURCE";
    }
    v.playlists_label = s_lib_mode == UI_LIB_MODE_PLAYLIST_TRACKS ? "BACK"
                      : s_lib_mode == UI_LIB_MODE_PLAYLISTS       ? "ALL TRACKS"
                                                                  : "PLAYLISTS";
    ui_djui_bridge_library_update(&v);
}

static void ui_library_djui_select_row(uint8_t row)
{
    ui_library_page_t page = ui_library_page_cached();
    if ((int)row >= page.row_count) {
        return;
    }
    s_selected_track_idx = ui_library_page_absolute_index(&page, (int)row);
    ui_library_select_visible_cell();
    UI_LIB_TRACE("dj_ui select row %u -> index %d", (unsigned)row, s_selected_track_idx);
}

/* A tap on a playlist row opens it (LOAD D1/D2 on it does the same, through
 * ui_library_load_selected_for_deck). */
void ui_library_djui_on_select(uint8_t row)
{
    ui_library_djui_select_row(row);
    if (s_lib_mode == UI_LIB_MODE_PLAYLISTS && !ui_library_peer_view()) {
        ui_library_open_playlist(s_selected_track_idx);
    }
}

/* LOAD D1 / D2 on the page: the controller LOAD button's own path
 * (BTN_LOAD -> DECK_UI_CMD_LOAD_SELECTED -> ui_library_load_selected_for_deck)
 * with the tapped row selected first. */
void ui_library_djui_on_load(uint8_t deck, uint8_t row)
{
    ui_library_djui_select_row(row);
    ctrl_event_t ev = {
        .type  = CTRL_EV_BUTTON,
        .id    = deck == CTRL_DECK_2 ? CTRL_ID_LOAD_DECK2 : CTRL_ID_LOAD_DECK1,
        .deck  = deck == CTRL_DECK_2 ? CTRL_DECK_2 : CTRL_DECK_1,
        .value = 1,
        .seq   = 0
    };
    esp_err_t rc = deck_core_queue_event(&ev);
    UI_LIB_TRACE("dj_ui LOAD D%u row %u -> index %d, LOAD button event %s",
                 (unsigned)ev.deck + 1u, (unsigned)row, s_selected_track_idx,
                 esp_err_to_name(rc));
}

/* dj_ui sort ids ARTIST..KEY are media_catalog fields 0..3. A new column
 * starts ascending, a second tap on the lit column reverses it. dj_ui lights
 * the column only from the published state, so a refusal (peer list, load
 * busy) leaves the buttons as they were. Same guards and selection keeping
 * as the legacy sort buttons, whose per-column toggles stay untouched. */
void ui_library_djui_on_sort(uint8_t sort)
{
    if (sort < DJ_SORT_ARTIST || sort > DJ_SORT_KEY || ui_library_sort_refused()) {
        return;
    }
    if (ui_library_track_load_busy() || media_catalog_load_in_progress()) {
        ui_library_status_hold("LOAD BUSY", COL_AMBER, 1200);
        return;
    }
    int field = (int)sort - 1;
    bool desc = field == s_djui_sort_field ? !s_djui_sort_desc : false;
    uint32_t target_key = ui_library_selected_key();
    media_catalog_sort(field, desc);
    s_djui_sort_field = field;
    s_djui_sort_desc = desc;
    s_djui_sort_generation = media_catalog_generation();
    ui_refresh_library();
    ui_library_preserve_selection_by_key(target_key);
    ui_library_populate_rows();
    UI_LIB_TRACE("dj_ui sort field %d %s, selection index %d", field, desc ? "desc" : "asc",
                 s_selected_track_idx);
}

void ui_library_djui_on_page(int8_t dir)
{
    ui_library_page_delta(dir);
}

void ui_library_djui_on_source(void)
{
    library_source_event_cb(NULL);
}

void ui_library_djui_on_playlists(void)
{
    ui_library_playlists_button();
}

void ui_library_update(const ui_frame_context_t *ctx)
{
#ifndef WIN32
    ui_library_poll_dj_link(ctx);
#endif
    int active_tab = ctx ? ctx->active_tab : 0;
    s_active_tab = active_tab;
    const uint32_t refresh_requested =
        ui_event_counter_sample(&s_library_refresh_events);
#ifndef WIN32
    const uint32_t usb_removed_requested =
        ui_event_counter_sample(&s_usb_removed_events);
#endif
    ui_library_update_plan_t plan =
        ui_library_plan_update(
            active_tab,
            ui_event_counter_pending(refresh_requested,
                                     s_library_refresh_applied),
#ifndef WIN32
            ui_event_counter_pending(usb_removed_requested,
                                     s_usb_removed_applied)
#else
            false
#endif
        );

#ifndef WIN32
    if (plan.apply_usb_removed) {
        ui_apply_usb_removed();
        s_usb_removed_applied = usb_removed_requested;
    }
    if (plan.poll_track_load_result) {
        ui_poll_track_load_result();
    }
#endif

    if (plan.refresh_library) {
        ui_refresh_library();
        s_library_refresh_applied = refresh_requested;
    }

    ui_library_djui_publish(ctx);
}

uint32_t ui_library_deck_artwork_key(uint8_t deck)
{
    uint8_t idx = ui_library_deck_index(deck);
    if (!s_deck_loaded_track_valid[idx] || !s_loaded_media_valid[idx]) return 0u;
    /* A DJ Link download is another player's track: no local artwork. */
    if (strncmp(s_loaded_media[idx].audio_path, UI_PEER_CACHE_PREFIX,
                sizeof(UI_PEER_CACHE_PREFIX) - 1u) == 0) {
        return 0u;
    }
    return s_deck_loaded_track_key[idx];
}

uint32_t ui_library_deck_duration_ms(uint8_t deck, uint32_t fallback_duration_ms)
{
    uint8_t idx = ui_library_deck_index(deck);
#ifndef WIN32
    if (s_loaded_media_valid[idx]) return s_loaded_media[idx].duration_ms;
#else
    if (s_deck_loaded_track_valid[idx] && s_deck_loaded_duration_ms[idx] > 0) return s_deck_loaded_duration_ms[idx];
#endif
    return fallback_duration_ms;
}

uint16_t ui_library_deck_bpm(uint8_t deck, uint16_t fallback_bpm)
{
    uint8_t idx = ui_library_deck_index(deck);
#ifndef WIN32
    if (s_loaded_media_valid[idx] && s_loaded_media[idx].bpm > 0) return s_loaded_media[idx].bpm;
#else
    if (s_deck_loaded_track_valid[idx] && s_deck_loaded_bpm[idx] > 0) return s_deck_loaded_bpm[idx];
#endif
    return fallback_bpm;
}

bool ui_library_get_loaded_waveform(uint8_t deck,
                                    const uint8_t **waveform_low,
                                    bool *has_waveform)
{
    uint8_t idx = ui_library_deck_index(deck);
    if (waveform_low) {
        *waveform_low = NULL;
    }
    if (has_waveform) {
        *has_waveform = false;
    }
#ifndef WIN32
    if (!s_loaded_media_valid[idx]) {
        return false;
    }
    if (waveform_low) {
        *waveform_low = s_loaded_media[idx].waveform_low;
    }
    if (has_waveform) {
        *has_waveform = s_loaded_media[idx].has_waveform != 0;
    }
    return true;
#else
    if (idx >= DECK_CORE_DECK_COUNT || !s_deck_loaded_track_valid[idx] || !s_deck_loaded_has_waveform[idx]) {
        return false;
    }
    if (waveform_low) {
        *waveform_low = s_deck_loaded_waveform_low[idx];
    }
    if (has_waveform) {
        *has_waveform = s_deck_loaded_has_waveform[idx];
    }
    return true;
#endif
}

esp_err_t ui_library_load_track_identity_for_deck(uint32_t track_key,
                                                   uint32_t generation,
                                                   uint8_t deck)
{
#ifndef WIN32
    if (track_key == 0u || deck >= DECK_CORE_DECK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (media_catalog_generation() != generation) {
        return ESP_ERR_INVALID_STATE;
    }

    int resolved_index = media_catalog_find_index_by_key(track_key);
    if (media_catalog_generation() != generation) {
        return ESP_ERR_INVALID_STATE;
    }
    if (resolved_index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!ui_library_try_begin_track_load()) {
        return ESP_ERR_INVALID_STATE;
    }

    ui_lvgl_lock();
    esp_err_t rc = ui_submit_track_load(resolved_index, track_key, generation, deck,
                                        NULL, NULL);
    ui_lvgl_unlock();
    return rc;
#else
    (void)generation;
    int index = library_find_row_by_key(track_key);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return ui_library_load_track_index_for_deck(index, deck);
#endif
}

#endif
