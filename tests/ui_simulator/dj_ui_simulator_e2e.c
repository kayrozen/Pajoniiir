/* JC1060 (1024x600) headless E2E gate for the dj_ui presentation layer.
 *
 * Builds dj_ui.c + the deterministic demo data source against the pinned LVGL
 * commit, drives navigation through the real LVGL click callbacks and writes
 * full-framebuffer PPM captures whose SHA-256 is checked by
 * run_ui_simulator_e2e_jc1060.sh. Time only advances through lv_tick_inc(), so
 * every run renders the same pixels. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "lvgl_private.h"   /* lv_display_t inv_areas: UI_SIM_DEBUG bench only */
#include "dj_ui.h"
#include "dj_ui_demo.h"
#include "ui_djui_bridge.h"
#include "ui_djui_text.h"
#include "ui_artwork_thumb.h"
#include "artwork_fixture.h"

#define DISPLAY_WIDTH  1024
#define DISPLAY_HEIGHT 600
#define TICK_STEP_MS 16u

static uint32_t s_framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static lv_display_t *s_display;
static int s_failures;
static const char *s_output_dir;

/* v287 direct strips: flushes over a strip, as the backend's flush_cb masks */
static bool s_sim_direct;
static uint32_t s_sim_repainted, s_sim_over_px;

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)pixels;
    for (uint8_t d = 0; d < DJ_DECKS && s_sim_direct; d++) {
        lv_area_t z;
        lv_area_t o;
        if (ui_djui_bridge_direct_area(d, &z) && lv_area_intersect(&o, area, &z)) {
            s_sim_repainted |= 1u << d;
            s_sim_over_px += (uint32_t)lv_area_get_size(&o);
        }
    }
    lv_display_flush_ready(display);
}

/* The backend task calls the bridge after every lv_timer_handler(). */
static void sim_post_refresh(void)
{
    if (!s_sim_direct) return;
    uint32_t repainted = s_sim_repainted;
    s_sim_repainted = 0;
    ui_djui_bridge_post_refresh(repainted);
}

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    s_failures++;
}

static void pump(uint32_t duration_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < duration_ms) {
        lv_tick_inc(TICK_STEP_MS);
        (void)lv_timer_handler();
        sim_post_refresh();
        elapsed += TICK_STEP_MS;
    }
    lv_refr_now(s_display);
    sim_post_refresh();
}

/* button_only: the label's direct parent must be clickable, so a box caption
 * ("RECORD") never shadows the button with the same text. */
static lv_obj_t *find_label_ex(lv_obj_t *root, const char *text, bool visible_only, bool button_only)
{
    if (!root || !text) {
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_label_class) && (!visible_only || lv_obj_is_visible(root)) &&
        (!button_only || lv_obj_has_flag(lv_obj_get_parent(root), LV_OBJ_FLAG_CLICKABLE))) {
        const char *value = lv_label_get_text(root);
        if (value && strcmp(value, text) == 0) {
            return root;
        }
    }
    uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_label_ex(lv_obj_get_child(root, (int32_t)i), text, visible_only, button_only);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static lv_obj_t *find_visible_label(const char *text)
{
    lv_obj_t *found = find_label_ex(lv_screen_active(), text, true, false);
    if (!found) {
        found = find_label_ex(lv_layer_top(), text, true, false);
    }
    return found;
}

static void dump_visible_labels(lv_obj_t *root, int depth)
{
    if (!root || depth > 10) {
        return;
    }
    if (lv_obj_check_type(root, &lv_label_class) && lv_obj_is_visible(root)) {
        const char *value = lv_label_get_text(root);
        fprintf(stderr, "  [label depth=%d] '%s'\n", depth, value ? value : "(null)");
    }
    uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; i++) {
        dump_visible_labels(lv_obj_get_child(root, (int32_t)i), depth + 1);
    }
}

static bool expect_label(const char *text)
{
    if (find_visible_label(text)) {
        return true;
    }
    fprintf(stderr, "Missing visible label: '%s'\n", text);
    if (getenv("UI_SIM_DEBUG")) {
        dump_visible_labels(lv_screen_active(), 0);
        dump_visible_labels(lv_layer_top(), 0);
    }
    s_failures++;
    return false;
}

static bool click_obj(lv_obj_t *obj, const char *what)
{
    lv_obj_t *target = obj;
    while (target && !lv_obj_has_flag(target, LV_OBJ_FLAG_CLICKABLE)) {
        target = lv_obj_get_parent(target);
    }
    if (!target) {
        fprintf(stderr, "No clickable ancestor: %s\n", what);
        s_failures++;
        return false;
    }
    if (lv_obj_send_event(target, LV_EVENT_CLICKED, NULL) != LV_RESULT_OK) {
        fprintf(stderr, "Click event failed for: %s\n", what);
        s_failures++;
        return false;
    }
    pump(64);
    return true;
}

static bool click_label(const char *text)
{
    lv_obj_t *label = find_visible_label(text);
    if (!label) {
        return expect_label(text);
    }
    return click_obj(label, text);
}

static bool click_button(const char *text)
{
    lv_obj_t *label = find_label_ex(lv_screen_active(), text, true, true);
    if (!label) {
        fprintf(stderr, "Missing visible button: '%s'\n", text);
        s_failures++;
        return false;
    }
    return click_obj(label, text);
}

static void expect_hidden_label(const char *text)
{
    if (find_visible_label(text)) {
        fprintf(stderr, "Label should not be visible: '%s'\n", text);
        s_failures++;
    }
}

/* ---- Scripted pointer: real LVGL press/release, needed where a handler reads
 * lv_indev_active() (waveform seek) or hit-testing matters (top-layer overlay). */
static lv_point_t s_ptr;
static bool s_ptr_down;

static void pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_ptr;
    data->state = s_ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void tap(int32_t x, int32_t y)
{
    s_ptr.x = x;
    s_ptr.y = y;
    s_ptr_down = true;
    pump(64);
    s_ptr_down = false;
    pump(64);
}

static void expect_near(const char *what, int64_t actual, int64_t expected, int64_t tol)
{
    if (actual < expected - tol || actual > expected + tol) {
        fprintf(stderr, "FAIL: %s = %lld, expected %lld +/- %lld\n", what, (long long)actual,
                (long long)expected, (long long)tol);
        s_failures++;
    }
}

static uint64_t framebuffer_hash(void)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const uint8_t *bytes = (const uint8_t *)s_framebuffer;
    for (size_t i = 0; i < sizeof(s_framebuffer); i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t capture(const char *name)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.ppm", s_output_dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        fail("screenshot path is too long");
        return 0;
    }

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    sim_post_refresh();

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Cannot open screenshot: %s\n", path);
        s_failures++;
        return 0;
    }
    fprintf(file, "P6\n%d %d\n255\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (size_t i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        uint32_t pixel = s_framebuffer[i];
        uint8_t rgb[3] = {
            (uint8_t)((pixel >> 16) & 0xffu),
            (uint8_t)((pixel >> 8) & 0xffu),
            (uint8_t)(pixel & 0xffu),
        };
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
    printf("CAPTURE %s\n", path);
    return framebuffer_hash();
}

/* UI_SIM_DEBUG only: host cost of re-rendering one zoom surface per source.
 * A ratio between sources, not a P4 number (HIL: backend "lvgl render" log). */
static double zoom_refr_us(uint8_t deck, int frames)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < frames; i++) {
        dj_ui_wave_invalidate(deck, DJ_WAVE_ZOOM);
        lv_refr_now(s_display);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    return ((double)(b.tv_sec - a.tv_sec) * 1e6 + (double)(b.tv_nsec - a.tv_nsec) / 1e3) / frames;
}

static void wave_source_timing(void)
{
    if (!getenv("UI_SIM_DEBUG")) return;
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
    pump(32);
    double peaks = zoom_refr_us(0, 200);
    double image = zoom_refr_us(1, 200);
    dj_ui_wave_set_source(1, DJ_WAVE_ZOOM, DJ_WAVE_SRC_EXTERNAL);
    double external = zoom_refr_us(1, 200);
    dj_ui_wave_set_source(1, DJ_WAVE_ZOOM, DJ_WAVE_SRC_IMAGE);
    fprintf(stderr, "[UI_SIM_DEBUG] zoom surface refresh (host): PEAKS %.1f us, IMAGE %.1f us, "
            "EXTERNAL %.1f us\n", peaks, image, external);
}

/* Firmware bridge (the JC1060 UI since v293) on a fresh dj_ui tree: the
 * set_* path answers to plain frame data. Not captured. */
static dj_field_t s_bridge_field = DJ_F_COUNT;
static void bridge_on_field(dj_field_t f) { s_bridge_field = f; }

/* Phase 2: the firmware's Settings data, as ui_settings_djui_update() fills it. */
static void bridge_settings_scenario(void)
{
    static const dj_ui_callbacks_t cb = { .on_field = bridge_on_field };
    dj_ui_set_callbacks(&cb);
    dj_ui_show_tab(DJ_TAB_SETTINGS);

    ui_djui_settings_view_t v = {
        .brightness_pct = 5,                  /* below the floor: shown as 10 */
        .wireless_on = true,
        .master_trim = "MASTER: -3 dB",
        .main_out_usb = false,
        .ui_blackout_play = true,
        .controller_name = "DDJ-FLX4",
        .controller_connected = true,
        .cue_mode = "CUE: SPLIT MONO",
        .jog_cdj = true,
        .tempo_range_pct = 16,
        .sd_state = UI_DJUI_SD_MOUNTED,
        .sd_free_bytes = 12ull * 1024 * 1024 * 1024 + 512ull * 1024 * 1024,
        .sd_total_bytes = 29ull * 1024 * 1024 * 1024,
        .sd_log_valid = true, .sd_log_available = true, .sd_log_kb = 95,
        .fw_version = "251", .fw_partition = "ota_1",
        .reset_reason = "Task watchdog", .reset_bad = true,
        .link_enabled = true,
        .link_status = "DJ LINK: ON P5 - CDJ-3000 #2 126.0 BPM",
        .link_tone = DJ_TONE_OK,
        .link_peer_count = 2,
        .link_peers = {
            { .number = 2, .name = "CDJ-3000", .bpm = 126.0f, .master = true, .playing = true },
            { .number = 3, .name = "XDJ-1000MK2", .bpm = 0.0f },
        },
        .link_master_valid = true,
        .link_master = { .player = 2, .bpm = 126.0f, .confirmed = true },
        .rec_state = UI_DJUI_REC_ACTIVE, .rec_secs = 83, .rec_mb = 12,
    };
    if (ui_djui_bridge_brightness_clamp(5) != 10 || ui_djui_bridge_brightness_clamp(140) != 100) {
        fail("bridge: brightness clamp");
    }
    ui_djui_bridge_settings_update(&v);
    pump(64);
    expect_label("10%");
    expect_label("P4 REMOTE: ON");
    expect_label("MASTER: -3 dB");
    expect_label("MAIN: PCM5102A RCA");
    expect_label("TEST: UI OFF on PLAY");
    expect_label("CUE: DDJ-FLX4");
    expect_label("MIXER: DDJ-FLX4");
    expect_label("CUE: SPLIT MONO");
    expect_label("JOG: CDJ");
    expect_label("TEMPO: +/-16%");
    expect_label("Controller (USB1): Connected");
    expect_label("Mounted: 12.5 GB free / 29.0 GB");
    expect_label("SD Log: OK  95KB  drop 0");
    expect_label("P4: 251 [ota_1]");
    expect_label("Last reset: Task watchdog");
    expect_label("DJ LINK: ON P5 - CDJ-3000 #2 126.0 BPM");
    expect_label("CDJ-3000 #2   126.0 BPM   MASTER   PLAY");
    expect_label("XDJ-1000MK2 #3   --- BPM");
    expect_label("M#2 126.0");
    expect_label("REC 01:23  12 MB");
    expect_label("STOP REC");
    expect_label("LOCAL: disabled");
    capture("settings_bridge");

    /* The cue-mode chip is tappable in dj_ui mode (legacy CUE button). */
    s_bridge_field = DJ_F_COUNT;
    click_label("CUE: SPLIT MONO");
    if (s_bridge_field != DJ_F_MIX_CUE) fail("bridge: CUE chip did not report DJ_F_MIX_CUE");
    /* v267: the jog-mode chip is tappable too (legacy JOG button). */
    s_bridge_field = DJ_F_COUNT;
    click_label("JOG: CDJ");
    if (s_bridge_field != DJ_F_MIX_JOG) fail("bridge: JOG chip did not report DJ_F_MIX_JOG");
    /* v275: the tempo range chip is tappable and follows the view. */
    s_bridge_field = DJ_F_COUNT;
    click_label("TEMPO: +/-16%");
    if (s_bridge_field != DJ_F_MIX_TEMPO) fail("bridge: TEMPO chip did not report DJ_F_MIX_TEMPO");
    v.tempo_range_pct = 6;
    ui_djui_bridge_settings_update(&v);
    pump(64);
    expect_label("TEMPO: +/-6%");
    v.tempo_range_pct = 16;
    ui_djui_bridge_settings_update(&v);
    pump(64);

    /* Test blackout frame while a deck plays, gone when it stops. */
    v.blackout_now = true;
    ui_djui_bridge_settings_update(&v);
    pump(64);
    expect_label("UI OFF - PLAYING");
    v.blackout_now = false;

    /* Hotplug, SD pulled, DJ Link and recorder off: every field follows. */
    v.controller_name = "";
    v.controller_connected = false;
    v.sd_state = UI_DJUI_SD_OFFLINE;
    v.sd_log_dropped = 3;
    v.link_enabled = false;
    v.link_status = "DJ LINK: OFF";
    v.link_tone = DJ_TONE_MUTED;
    v.link_peer_count = 0;
    v.link_master_valid = false;
    v.rec_state = UI_DJUI_REC_IDLE;
    ui_djui_bridge_settings_update(&v);
    pump(64);
    expect_hidden_label("UI OFF - PLAYING");
    expect_label("CUE: USB");
    expect_label("MIXER: USB");
    expect_label("Controller (USB1): Disconnected");
    expect_label("Offline (/sd unavailable)");
    expect_label("SD Log: OK  95KB  drop 3");
    expect_label("DJ LINK: OFF");
    expect_hidden_label("CDJ-3000 #2   126.0 BPM   MASTER   PLAY");
    expect_label("LINK OFF");
    expect_label("REC --:--");
    expect_label("RECORD");
}

/* Phase 3: merged hot cue slots (store > ANLZ > empty), as ui.c fills them. */
static int s_bridge_hc_deck = -1, s_bridge_hc_index = -1, s_bridge_target = -1;
static void bridge_on_hotcue(uint8_t deck, uint8_t index)
{
    s_bridge_hc_deck = deck;
    s_bridge_hc_index = index;
}
static void bridge_on_target(uint8_t deck) { s_bridge_target = deck; }

static void bridge_hotcues_scenario(void)
{
    static const dj_ui_callbacks_t cb = { .on_hotcue = bridge_on_hotcue, .on_target = bridge_on_target };
    dj_ui_set_callbacks(&cb);
    dj_ui_show_tab(DJ_TAB_HOTCUES);

    ui_djui_hotcues_view_t v = { .target = 0, .anlz = { true, false } };
    v.slot[0][0] = (ui_djui_hotcue_t){ .set = true, .pos_ms = 15000 };
    v.slot[0][2] = (ui_djui_hotcue_t){ .set = true, .loop = true, .pos_ms = 62000, .end_ms = 66000 };
    v.slot[0][5] = (ui_djui_hotcue_t){ .set = true, .pos_ms = 3723000 };
    v.slot[1][1] = (ui_djui_hotcue_t){ .set = true, .pos_ms = 90500 };
    ui_djui_bridge_hotcues_update(&v);
    pump(64);
    expect_label("CUE A");
    expect_label("00:00:15");
    expect_label("LOOP C");
    expect_label("00:01:02");
    expect_label("01:02:03");
    expect_label("CUES 3/8");
    expect_label("LOOPS 1");
    expect_label("ANLZ DATA");
    expect_label("TARGET D1");
    capture("hotcues_bridge_d1");

    /* A card reports the target deck and slot; the bridge returns what was shown. */
    click_label("LOOP C");
    ui_djui_hotcue_t c;
    if (s_bridge_hc_deck != 0 || s_bridge_hc_index != 2) fail("bridge: LOOP C card did not report D1/2");
    if (!ui_djui_bridge_hotcue_get(0, 2, &c) || !c.set || !c.loop || c.pos_ms != 62000 || c.end_ms != 66000) {
        fail("bridge: hotcue_get D1 LOOP C");
    }
    if (!ui_djui_bridge_hotcue_get(0, 1, &c) || c.set) fail("bridge: hotcue_get D1 B should be empty");
    if (ui_djui_bridge_hotcue_get(2, 0, &c) || ui_djui_bridge_hotcue_get(0, 8, &c)) {
        fail("bridge: hotcue_get out of range");
    }

    /* TARGET D2 tap reports on_target; the firmware answers with a new view. */
    click_button("D2");
    if (s_bridge_target != 1) fail("bridge: TARGET D2 did not report on_target(1)");
    v.target = 1;
    ui_djui_bridge_hotcues_update(&v);
    pump(64);
    expect_label("CUE B");
    expect_label("00:01:30");
    expect_label("CUE C");            /* D1 loop label does not leak into D2 */
    expect_label("CUES 1/8");
    expect_label("LOOPS 0");
    expect_label("NO ANLZ");
    expect_label("TARGET D2");
    capture("hotcues_bridge_d2");

    /* Store cleared on D2 (eject / SHIFT+CUE path): the card empties. */
    v.slot[1][1].set = false;
    ui_djui_bridge_hotcues_update(&v);
    pump(64);
    expect_hidden_label("00:01:30");
    expect_label("CUES 0/8");
    if (!ui_djui_bridge_hotcue_get(1, 1, &c) || c.set) fail("bridge: cleared D2 B still set");
}

/* v256: the Library page as ui_library.c publishes it (rows only when the
 * page changed, the rest diffed) and its callbacks back into the model. */
static int s_lib_select = -1, s_lib_load_deck = -1, s_lib_load_row = -1;
static int s_lib_sort = -1, s_lib_page = 0, s_lib_source = 0;
static void lib_on_select(uint8_t row) { s_lib_select = row; }
static void lib_on_load(uint8_t deck, uint8_t row) { s_lib_load_deck = deck; s_lib_load_row = row; }
static void lib_on_sort(dj_sort_t sort) { s_lib_sort = (int)sort; }
static void lib_on_page(int8_t dir) { s_lib_page = dir; }
static void lib_on_source(void) { s_lib_source++; }
static int s_lib_playlists = 0;
static void lib_on_playlists(void) { s_lib_playlists++; }

/* PREV / NEXT and LOAD are dimmed, not hidden, when they would do nothing. */
static void expect_button_opa(const char *text, lv_opa_t opa)
{
    lv_obj_t *label = find_visible_label(text);
    if (!label) {
        fprintf(stderr, "Button label not found: '%s'\n", text);
        s_failures++;
        return;
    }
    lv_opa_t got = lv_obj_get_style_opa(lv_obj_get_parent(label), LV_PART_MAIN);
    if (got != opa) {
        fprintf(stderr, "Button '%s' opacity %u, expected %u\n", text, (unsigned)got, (unsigned)opa);
        s_failures++;
    }
}

static void bridge_library_scenario(void)
{
    static const dj_ui_callbacks_t cb = {
        .on_lib_select = lib_on_select, .on_lib_load = lib_on_load, .on_lib_sort = lib_on_sort,
        .on_lib_page = lib_on_page, .on_lib_source = lib_on_source,
    };
    dj_ui_set_callbacks(&cb);
    dj_ui_show_tab(DJ_TAB_LIBRARY);

    /* Phase 4: FR tags are fitted the way ui_library builds the page. */
    char fr_title[64], fr_artist[64];
    ui_djui_text_fit(fr_title, sizeof fr_title, "\xC3\x89t\xC3\xA9 Paris \xE2\x80\x93 Gar\xC3\xA7on \xC2\xBB");
    ui_djui_text_fit(fr_artist, sizeof fr_artist, "C\xC3\xA9line & Z\xC3\xB6" "e");
    const dj_track_t rows[4] = {
        { .title = "Bridge Track A", .artist = "Artist A", .key = "8A", .bpm = 124, .len_ms = 245000 },
        { .title = "Bridge Track B", .artist = "Artist B", .key = "11B", .bpm = 128, .len_ms = 372000 },
        { .title = "Bridge Track C", .artist = "Artist C", .key = "2A", .bpm = 174, .len_ms = 301000 },
        { .title = fr_title, .artist = fr_artist, .key = "5A", .bpm = 122, .len_ms = 199000 },
    };
    ui_djui_bridge_library_set_rows(rows, 4);
    ui_djui_library_view_t v = {
        .source = "LOCAL USB", .total = 12, .page = 2, .pages = 2,
        .selected = 1, .loaded = { 0, -1 },
        .status_deck = 1, .deck_status = "READY",
        .load_enabled = true, .progress = -1, .source_label = "SOURCE",
        .sort = DJ_SORT_BPM,
    };
    ui_djui_bridge_library_update(&v);
    pump(64);
    expect_label("Bridge Track A");
    expect_label("Bridge Track C");
    expect_label("Ete Paris - Garcon \"");
    expect_label("Celine & Zoe");
    expect_label("11B");
    expect_label("174");
    expect_label("6:12");
    expect_label("LOCAL USB     12 TRACKS     PAGE 2/2");
    expect_label("DECK 2");
    expect_label("READY");
    expect_label(LV_SYMBOL_UP);
    expect_button_opa("PREV", LV_OPA_COVER);
    expect_button_opa("NEXT", LV_OPA_40);
    capture("library_bridge");

    /* Taps report page rows; the model answers with the next view. */
    click_label("Bridge Track C");
    if (s_lib_select != 2) fail("bridge: library row tap did not report row 2");
    click_label("LOAD DECK 2");
    if (s_lib_load_deck != 1 || s_lib_load_row != 2) fail("bridge: LOAD DECK 2 did not report D2/row 2");
    /* Sort: dj_ui reports the tapped column and lights only what the model
     * publishes; the second tap on the lit column comes back reversed. */
    s_lib_sort = -1;
    click_label("SORT ARTIST");
    if (s_lib_sort != DJ_SORT_ARTIST) fail("bridge: SORT ARTIST not reported");
    pump(16);
    expect_label(LV_SYMBOL_UP);           /* model refused: BPM stays lit */
    click_label("SORT BPM");
    if (s_lib_sort != DJ_SORT_BPM) fail("bridge: SORT BPM not reported");
    click_label("SORT BPM");
    if (s_lib_sort != DJ_SORT_BPM) fail("bridge: second SORT BPM tap should report BPM again");
    v.sort_desc = true;
    ui_djui_bridge_library_update(&v);
    pump(64);
    expect_label(LV_SYMBOL_DOWN);
    expect_hidden_label(LV_SYMBOL_UP);
    capture("library_bridge_sort_desc");
    click_label("PREV");
    if (s_lib_page != -1) fail("bridge: PREV not reported");
    click_label("NEXT");
    if (s_lib_page != 1) fail("bridge: NEXT not reported");
    click_label("SOURCE");
    if (s_lib_source != 1) fail("bridge: SOURCE not reported");

    /* Load gate busy: LOAD is swallowed; status holds land in the library too. */
    v.load_enabled = false;
    v.selected = 2;
    ui_djui_bridge_library_update(&v);
    s_lib_load_deck = -1;
    click_label("LOAD DECK 1");
    if (s_lib_load_deck != -1) fail("bridge: LOAD reported while the gate is busy");
    uint32_t now = lv_tick_get();
    ui_djui_bridge_status_hold("LOAD BUSY", DJ_TONE_WARN, 500, now);
    pump(64);
    expect_label("LOAD BUSY");
    ui_djui_frame_t f = { .now_ms = now + 600 };
    ui_djui_bridge_update(&f);
    pump(64);
    expect_hidden_label("LOAD BUSY");
    /* Holds may carry a DJ Link player name: fitted too. */
    ui_djui_bridge_status_hold("USB CDJ-3000 Z\xC3\xBCrich #2", DJ_TONE_INFO, 500, now);
    pump(64);
    expect_label("USB CDJ-3000 Zurich #2");

    /* Peer page shrinking under the selection, download running. */
    const dj_track_t peer[3] = {
        { .title = "Peer Track", .artist = "Peer Artist", .key = "", .bpm = 128, .len_ms = 240000,
          .badge = "42%", .badge_tone = DJ_TONE_OK },
        { .title = "Peer Meta", .artist = "Peer Artist", .key = "", .badge = "META",
          .badge_tone = DJ_TONE_MUTED, .bpm_text = "...", .time_text = "..." },
        { .title = "Peer No Tempo", .artist = "Peer Artist", .key = "", .len_ms = 180000,
          .badge = "NET", .bpm_text = "--" },
    };
    ui_djui_bridge_library_set_rows(peer, 3);
    v = (ui_djui_library_view_t){
        .source = "USB CDJ-3000 #2  DOWNLOAD 42%", .total = 3, .page = 1, .pages = 1,
        .selected = 0, .loaded = { -1, -1 },
        .status_deck = DJ_DECKS, .deck_status = "ACTIVE",
        .load_enabled = true, .progress = 42, .source_label = "CANCEL",
    };
    ui_djui_bridge_library_update(&v);
    pump(64);
    expect_label("Peer Track");
    expect_label("42%");
    expect_label("CANCEL");
    expect_label("DECK 1+2");
    expect_label("ACTIVE");
    expect_label("USB CDJ-3000 #2  DOWNLOAD 42%     3 TRACKS     PAGE 1/1");
    expect_hidden_label("Bridge Track B");
    expect_label("META");
    expect_label("...");
    expect_label("--");
    expect_label("3:00");
    /* Peer lists keep the player's order: no sort column is lit. */
    expect_hidden_label(LV_SYMBOL_DOWN);
    expect_hidden_label(LV_SYMBOL_UP);
    expect_button_opa("PREV", LV_OPA_40);
    expect_button_opa("NEXT", LV_OPA_40);
    capture("library_bridge_peer");
    click_label("LOAD DECK 1");
    if (s_lib_load_deck != 0 || s_lib_load_row != 0) fail("bridge: peer LOAD DECK 1 not reported");
}

/* Playlists (jc1060): runs last so the overview captures keep their clock. */
static void bridge_library_playlists_scenario(void)
{
    static const dj_ui_callbacks_t cb = {
        .on_lib_playlists = lib_on_playlists,
    };
    dj_ui_set_callbacks(&cb);
    dj_ui_show_tab(DJ_TAB_LIBRARY);
    /* Playlist list: count unit, track count in TIME, button reads ALL TRACKS;
     * inside a playlist it reads BACK. */
    const dj_track_t lists[2] = {
        { .title = "Favorites", .artist = "", .key = "", .bpm_text = "", .time_text = "12 TR" },
        { .title = "Warmup", .artist = "Sets", .key = "", .bpm_text = "", .time_text = "3 TR" },
    };
    ui_djui_bridge_library_set_rows(lists, 2);
    ui_djui_library_view_t v = {
        .source = "PLAYLISTS", .unit = "PLAYLISTS", .total = 2, .page = 1, .pages = 1,
        .selected = 0, .loaded = { -1, -1 },
        .status_deck = 0, .deck_status = "READY",
        .load_enabled = true, .progress = -1, .source_label = "SOURCE",
        .playlists_label = "ALL TRACKS",
    };
    ui_djui_bridge_library_update(&v);
    pump(64);
    expect_label("PLAYLISTS     2 PLAYLISTS     PAGE 1/1");
    expect_label("Favorites");
    expect_label("12 TR");
    expect_label("ALL TRACKS");
    expect_hidden_label("Peer Track");
    click_label("ALL TRACKS");
    if (s_lib_playlists != 1) fail("bridge: PLAYLISTS button not reported");
    v.source = "PLAYLIST Warmup";
    v.unit = NULL;
    v.playlists_label = "BACK";
    ui_djui_bridge_library_update(&v);
    pump(64);
    expect_label("PLAYLIST Warmup     2 TRACKS     PAGE 1/1");
    expect_label("BACK");
    click_label("BACK");
    if (s_lib_playlists != 2) fail("bridge: BACK not reported");
}

/* ---- Overview (phase 5): our ANLZ wave cache rendered into the dj_ui zoom
 * as a ring IMAGE, dj_ui markers on top, frame budget governor ---- */
#define OV_DURATION_MS 300000u
#define OV_HIGH_PER_S  150u

static anlz_beat_t s_ov_beats[640];
static uint8_t s_ov_high[OV_DURATION_MS / 1000u * OV_HIGH_PER_S];
static anlz_metadata_t s_ov_meta[DJ_DECKS];

static void overview_fixture(void)
{
    /* 128 BPM grid, bar-phased */
    for (uint32_t i = 0; i < 640; i++) {
        s_ov_beats[i] = (anlz_beat_t){ .beat_phase = (uint16_t)(i % 4), .bpm_x100 = 12800,
                                       .time_ms = 250 + i * 60000u / 128u };
    }
    for (uint32_t i = 0; i < sizeof s_ov_high; i++) {
        uint8_t amp = (uint8_t)(6 + (i * 7 + (i / 37) * 3) % 26);
        s_ov_high[i] = (uint8_t)(amp | (((i / 300) % 8) << 5));
    }
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        anlz_metadata_t *m = &s_ov_meta[d];
        memset(m, 0, sizeof *m);
        m->beats = s_ov_beats;
        m->beat_count = 640;
        m->bpm = 128;
        for (uint32_t i = 0; i < ANLZ_WAVEFORM_LOW_LEN; i++) m->waveform_low[i] = (uint8_t)((i * 11) % 32);
        m->has_waveform_low = true;
    }
    s_ov_meta[0].waveform_high = s_ov_high;     /* D1 HIGH, D2 LOW detail */
    s_ov_meta[0].waveform_high_len = sizeof s_ov_high;
    s_ov_meta[0].cues[0] = (anlz_cue_t){ ANLZ_CUE_SINGLE, 0, 61000, 0 };
    s_ov_meta[0].cues[1] = (anlz_cue_t){ ANLZ_CUE_LOOP, 1, 63000, 64875 };
    s_ov_meta[0].cue_count = 2;
    s_ov_meta[1].cues[0] = (anlz_cue_t){ ANLZ_CUE_SINGLE, 2, 31000, 0 };
    s_ov_meta[1].cue_count = 1;
}

static void overview_deck(ui_djui_deck_view_t *v, uint8_t d, uint32_t pos_ms)
{
    const anlz_metadata_t *m = &s_ov_meta[d];
    *v = (ui_djui_deck_view_t){
        .loaded = true, .title = d ? "Overview Two" : "Overview One", .artist = "Bridge Artist",
        .bpm = 128, .bpm_x100 = d ? 12800 : 12850, .tempo_pct = d ? 0.0f : 2.0f,
        .key = d ? NULL : "8A", .master_tempo = d == 0,
        .duration_ms = OV_DURATION_MS, .position_ms = pos_ms, .playing = true,
        .waveform_low = m->waveform_low,
        .cue_point_set = true, .cue_point_ms = d ? 29000 : 60000,
        .beat_valid = true, .beat_phase = (uint8_t)((pos_ms / 469) % 4),
        .beat_downbeat = (pos_ms / 469) % 4 == 0, .vu_peak = d ? 9000 : 26000,
        .wave = ui_waveform_source_select(m, NULL, false), .meta = m,
        .center_ms = pos_ms, .window_ms = 8000,
        .cues = m->cues, .cue_count = m->cue_count,
    };
    if (d == 0) {                     /* active loop, burned into the strip */
        v->loop_active = true;
        v->loop_start_ms = 62000;
        v->loop_end_ms = 63875;
    } else {                          /* loop-in pressed: dj_ui trail */
        v->loop_armed = true;
        v->loop_armed_ms = pos_ms - 1500;
    }
}

static void overview_frame(ui_djui_frame_t *f, uint32_t p0, uint32_t p1)
{
    f->now_ms = lv_tick_get();
    f->overview_visible = true;
    overview_deck(&f->deck[0], 0, p0);
    overview_deck(&f->deck[1], 1, p1);
    f->fx = (ui_djui_fx_view_t){ .name = "ECHO", .channel = 1, .beat_index = 2, .time_ms = 469,
                                 .level_pct = 50, .on = true };
}

/* v287: the sim's PPA copies RGB565 to XRGB8888 as LVGL's RGB565 image blend
 * converts, so a direct strip matches the LVGL-drawn one. */
static uint32_t s_sim_blits;
static bool s_sim_blit_fail;

static bool sim_direct_blit(int32_t x, int32_t y, const uint16_t *src, int32_t src_w, int32_t h,
                            int32_t src_x, int32_t w)
{
    if (s_sim_blit_fail) return false;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT ||
        src_x < 0 || src_x + w > src_w) {
        fail("direct: blit outside the framebuffer or the ring");
        return false;
    }
    s_sim_blits++;
    for (int32_t r = 0; r < h; r++) {
        const uint16_t *in = src + (size_t)r * (size_t)src_w + (size_t)src_x;
        uint32_t *out = s_framebuffer + (size_t)(y + r) * DISPLAY_WIDTH + (size_t)x;
        for (int32_t i = 0; i < w; i++) {
            uint32_t c = in[i];
            out[i] = 0xff000000u | ((((c >> 11) * 2106u) >> 8) << 16) |
                     (((((c >> 5) & 0x3fu) * 1037u) >> 8) << 8) | (((c & 0x1fu) * 2106u) >> 8);
        }
    }
    return true;
}

/* Both zoom areas, RGB only. */
static void zoom_pixels(uint32_t *out, size_t cap)
{
    size_t n = 0;
    for (uint8_t d = 0; d < DJ_DECKS; d++) {
        lv_area_t z;
        if (!ui_djui_bridge_direct_area(d, &z)) continue;
        for (int32_t y = z.y1; y <= z.y2; y++) {
            for (int32_t x = z.x1; x <= z.x2 && n < cap; x++) {
                out[n++] = s_framebuffer[(size_t)y * DISPLAY_WIDTH + (size_t)x] & 0xffffffu;
            }
        }
    }
}

/* Direct strips against the LVGL-drawn reference: the markers are blended in
 * RGB565 instead of XRGB8888, a few LSBs apart at most. */
static void expect_zoom_like(const char *what, const uint32_t *ref, uint32_t *cur, size_t n)
{
    zoom_pixels(cur, n);
    unsigned diff_max = 0, diff_px = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned m = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            int a = (int)((ref[i] >> sh) & 0xffu), b = (int)((cur[i] >> sh) & 0xffu);
            unsigned dd = (unsigned)(a > b ? a - b : b - a);
            if (dd > m) m = dd;
        }
        if (m) diff_px++;
        if (m > diff_max) diff_max = m;
    }
    if (diff_max > 12) {          /* one RGB565 step plus rounding */
        fprintf(stderr, "FAIL: direct: %s: %u px differ, max %u\n", what, diff_px, diff_max);
        s_failures++;
    }
    if (getenv("UI_SIM_DEBUG")) {
        fprintf(stderr, "[UI_SIM_DEBUG] direct %s: %u of %zu zoom px differ, max %u\n", what, diff_px, n, diff_max);
    }
}

static void bridge_direct_scenario(ui_djui_frame_t *f, uint32_t *p0, uint32_t *p1)
{
    ui_djui_bridge_perf_t perf;
    ui_djui_bridge_get_perf(&perf);
    size_t n = (size_t)perf.zoom_w * perf.zoom_h * DJ_DECKS;
    uint32_t *ref = malloc(n * sizeof *ref), *cur = malloc(n * sizeof *cur);
    if (!ref || !cur || n == 0) {
        fail("direct: no zoom strips");
        free(ref);
        free(cur);
        return;
    }

    /* LVGL-drawn reference, then the same state blitted */
    ui_djui_bridge_update(f);
    pump(64);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    zoom_pixels(ref, n);
    ui_djui_bridge_reset_perf();
    s_sim_blits = 0;
    s_sim_direct = true;
    ui_djui_bridge_set_direct_blit(sim_direct_blit);
    pump(64);
    ui_djui_bridge_get_perf(&perf);
    if (perf.direct_blits != DJ_DECKS || s_sim_blits < DJ_DECKS) fail("direct: switch-over did not blit both strips once");
    expect_zoom_like("switch-over", ref, cur, n);
    /* a full LVGL repaint draws over both: blitted again */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    sim_post_refresh();
    ui_djui_bridge_get_perf(&perf);
    if (perf.direct_repaints < DJ_DECKS || perf.direct_blits != 2u * DJ_DECKS) fail("direct: full repaint not re-blitted");
    expect_zoom_like("full repaint", ref, cur, n);

    /* Scrolling: every changed strip goes out; LVGL only touches the strips
     * where a label overhangs them (the zoom time label, 2 rows). */
    ui_djui_bridge_reset_perf();
    s_sim_over_px = 0;
    const int frames = 60;
    for (int i = 0; i < frames; i++) {
        *p0 += 17;
        *p1 += 16;
        overview_frame(f, *p0, *p1);
        ui_djui_bridge_update(f);
        pump(16);
    }
    ui_djui_bridge_get_perf(&perf);
    if (perf.direct_fails != 0) fail("direct: blit failed while scrolling");
    if (perf.direct_blits < (uint32_t)frames) fail("direct: strips did not follow the playhead");
    if (s_sim_over_px > (uint32_t)frames * perf.zoom_w) fail("direct: LVGL still paints the strips while scrolling");
    if (getenv("UI_SIM_DEBUG")) {
        fprintf(stderr, "[UI_SIM_DEBUG] direct scroll %d frames: %u blits, %u lvgl repaints, "
                        "lvgl px over the strips %u (%.1f per frame)\n", frames, (unsigned)perf.direct_blits,
                (unsigned)perf.direct_repaints, (unsigned)s_sim_over_px, (double)s_sim_over_px / frames);
    }
    /* marker-for-marker against LVGL at the scrolled state */
    s_sim_direct = false;
    ui_djui_bridge_set_direct_blit(NULL);
    pump(64);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    zoom_pixels(ref, n);
    s_sim_direct = true;
    ui_djui_bridge_set_direct_blit(sim_direct_blit);
    pump(64);
    expect_zoom_like("scrolled", ref, cur, n);

    /* Covered (Library tab, screensaver): nothing goes over them. */
    dj_ui_show_tab(DJ_TAB_LIBRARY);
    pump(64);
    s_sim_blits = 0;
    f->overview_visible = false;
    for (int i = 0; i < 4; i++) {
        *p0 += 17;
        *p1 += 16;
        overview_frame(f, *p0, *p1);
        f->overview_visible = false;
        ui_djui_bridge_update(f);
        pump(16);
    }
    if (s_sim_blits != 0) fail("direct: strip blitted over the Library tab");
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
    overview_frame(f, *p0, *p1);
    ui_djui_bridge_update(f);
    pump(64);
    if (s_sim_blits == 0) fail("direct: strips not back after the Library tab");
    ui_djui_bridge_set_screensaver(true);
    pump(64);
    s_sim_blits = 0;
    *p0 += 500;
    overview_frame(f, *p0, *p1);
    ui_djui_bridge_update(f);
    lv_obj_invalidate(lv_screen_active());
    pump(64);
    if (s_sim_blits != 0) fail("direct: strip blitted over the screensaver");
    ui_djui_bridge_set_screensaver(false);
    ui_djui_bridge_update(f);
    pump(64);
    if (s_sim_blits == 0) fail("direct: strips not back after the screensaver");

    /* A failed blit hands the strips back to LVGL for good. */
    ui_djui_bridge_reset_perf();
    s_sim_blit_fail = true;
    *p0 += 17;
    overview_frame(f, *p0, *p1);
    ui_djui_bridge_update(f);
    pump(64);
    s_sim_blit_fail = false;
    s_sim_blits = 0;
    *p0 += 17;
    overview_frame(f, *p0, *p1);
    ui_djui_bridge_update(f);
    pump(64);
    ui_djui_bridge_get_perf(&perf);
    if (perf.direct_fails != 1 || s_sim_blits != 0) fail("direct: failed blit did not fall back to LVGL");
    zoom_pixels(ref, n);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    expect_zoom_like("fallback", ref, cur, n);

    s_sim_direct = false;
    ui_djui_bridge_set_direct_blit(NULL);
    free(ref);
    free(cur);
}

static void bridge_overview_scenario(void)
{
    overview_fixture();
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
    ui_djui_bridge_reset_perf();

    ui_djui_frame_t f = {0};
    uint32_t p0 = 61500, p1 = 30000;
    overview_frame(&f, p0, p1);
    ui_djui_bridge_update(&f);
    pump(64);
    expect_label("Overview One");
    expect_label("Overview Two");
    expect_label("BPM / 8A");
    expect_label("131.1");            /* grid 128.50 BPM at +2.0 % */
    expect_label("128.0");

    ui_djui_bridge_perf_t perf;
    ui_djui_bridge_get_perf(&perf);
    uint32_t strip_px = perf.zoom_w * perf.zoom_h;
    if (strip_px == 0) fail("bridge: zoom strip size unknown");
    /* v285: one strip rebuild per frame; the other deck follows next frame. */
    if (perf.strip_redraws != 1 || perf.strip_full != 1 || perf.strip_full_deferred != 1) {
        fail("bridge: first Overview frame did not rebuild exactly one zoom strip");
    }
    if (perf.strip_px_last != strip_px) fail("bridge: strip px accounting");
    ui_djui_bridge_update(&f);
    pump(64);
    ui_djui_bridge_get_perf(&perf);
    if (perf.strip_redraws != 2 || perf.strip_full != 2 || perf.strip_full_deferred != 1) {
        fail("bridge: second Overview frame did not rebuild the deferred zoom strip");
    }
    if (perf.strip_px_last > UI_DJUI_FRAME_BUDGET_PX) fail("bridge: strip px over the frame budget");

    /* ring IMAGE: the zoom centre column is the cache's rendered time */
    int32_t x = -1;
    lv_area_t z;
    if (!dj_ui_wave_get_area(0, DJ_WAVE_ZOOM, &z) || !dj_ui_wave_ms_to_x(0, DJ_WAVE_ZOOM, p0, &x)) {
        fail("bridge: zoom playhead outside surface");
    } else {
        expect_near("bridge D1 zoom playhead x", x, lv_area_get_width(&z) / 2, 1);
    }
    capture("overview_bridge");

    /* v275: a tap on the footer time shows the elapsed time, per deck, and
     * survives the per-frame updates; a second tap goes back. */
    expect_label("03:58.5");
    click_label("REMAIN");
    ui_djui_bridge_update(&f);
    pump(64);
    expect_label("ELAPSED");
    expect_label("04:30.0");          /* deck 2 still counts down */
    expect_hidden_label("03:58.5");
    capture("overview_bridge_elapsed");
    click_label("ELAPSED");
    pump(64);
    expect_label("03:58.5");
    expect_hidden_label("ELAPSED");

    /* Both decks scroll for one second at 60 Hz: steady cost stays in budget. */
    ui_djui_bridge_reset_perf();
    for (int i = 0; i < 60; i++) {
        p0 += 17;
        p1 += 16;
        overview_frame(&f, p0, p1);
        ui_djui_bridge_update(&f);
        pump(16);
    }
    ui_djui_bridge_get_perf(&perf);
    if (perf.strip_deferred != 0) fail("bridge: default budget deferred a strip");
    if (perf.strip_redraws < 100) fail("bridge: zoom strips did not follow the playhead");
    if (perf.strip_px_max > UI_DJUI_FRAME_BUDGET_PX) fail("bridge: strip px over budget");
    /* whole refresh, strips + playhead/VU/beat/labels, as LVGL renders it */
    if (perf.inv_px_max > UI_DJUI_FRAME_BUDGET_PX) fail("bridge: rendered px per refresh over budget");
    if (getenv("UI_SIM_DEBUG")) {
        fprintf(stderr, "[UI_SIM_DEBUG] bridge zoom %ux%u: %u updates, %u redraws, strip px/frame max %u, "
                        "rendered px/refresh last %u max %u (budget %u)\n",
                (unsigned)perf.zoom_w, (unsigned)perf.zoom_h, (unsigned)perf.updates,
                (unsigned)perf.strip_redraws, (unsigned)perf.strip_px_max, (unsigned)perf.inv_px_last,
                (unsigned)perf.inv_px_max, (unsigned)UI_DJUI_FRAME_BUDGET_PX);
    }
    capture("overview_bridge_scroll");

    /* Over budget: one strip per frame, decks alternate, none starves. */
    ui_djui_bridge_set_frame_budget_px(strip_px);
    ui_djui_bridge_reset_perf();
    for (int i = 0; i < 10; i++) {
        p0 += 17;
        p1 += 16;
        overview_frame(&f, p0, p1);
        ui_djui_bridge_update(&f);
        pump(16);
    }
    ui_djui_bridge_get_perf(&perf);
    if (perf.strip_deferred == 0) fail("bridge: small budget did not defer");
    if (perf.strip_px_max > strip_px) fail("bridge: small budget exceeded");
    if (perf.strip_redraws != 10) fail("bridge: degraded budget must still redraw one strip per frame");
    ui_djui_bridge_set_frame_budget_px(0);

    /* Hidden (other tab) and screensaver: no strip work. */
    ui_djui_bridge_reset_perf();
    f.overview_visible = false;
    f.deck[0].center_ms += 500;
    ui_djui_bridge_update(&f);
    ui_djui_bridge_set_screensaver(true);
    f.overview_visible = true;
    ui_djui_bridge_update(&f);
    ui_djui_bridge_get_perf(&perf);
    if (perf.strip_redraws != 0) fail("bridge: strip rendered while hidden");
    ui_djui_bridge_set_screensaver(false);
    pump(64);
    ui_djui_bridge_update(&f);
    ui_djui_bridge_get_perf(&perf);
    if (perf.strip_redraws == 0) fail("bridge: strip not resumed after the screensaver");

    bridge_direct_scenario(&f, &p0, &p1);

    /* Eject: the zoom leaves ring mode, no stale strip. */
    f.deck[1].loaded = false;
    ui_djui_bridge_update(&f);
    pump(64);
    expect_hidden_label("Overview Two");
}

/* v260: the transport callbacks fire once per real tap and never from the
 * bridge's per-frame set_* calls. A pointer indev drives press / release, so
 * every event LVGL derives from a tap (PRESSED, SHORT_CLICKED, CLICKED,
 * RELEASED) reaches the buttons. */
static struct {
    unsigned play, cue, mt, seek, hotcue;
    uint8_t deck;
} s_tr;
static void tr_on_play(uint8_t deck) { s_tr.play++; s_tr.deck = deck; }
static void tr_on_cue(uint8_t deck) { s_tr.cue++; s_tr.deck = deck; }
static void tr_on_mt(uint8_t deck) { s_tr.mt++; s_tr.deck = deck; }
static void tr_on_seek(uint8_t deck, uint32_t pos_ms, dj_wave_t wave) { (void)pos_ms; (void)wave; s_tr.seek++; s_tr.deck = deck; }
static void tr_on_hotcue(uint8_t deck, uint8_t index) { (void)index; s_tr.hotcue++; s_tr.deck = deck; }

static lv_point_t s_ptr_point;
static bool s_ptr_pressed;
static void ptr_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_ptr_point;
    data->state = s_ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void tap_obj(lv_obj_t *obj, const char *what)
{
    while (obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) obj = lv_obj_get_parent(obj);
    if (!obj) {
        fprintf(stderr, "No clickable ancestor: %s\n", what);
        s_failures++;
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    s_ptr_point = (lv_point_t){ (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
    s_ptr_pressed = true;
    pump(96);
    s_ptr_pressed = false;
    pump(96);
}

static void expect_transport(const char *step, unsigned play, unsigned cue, unsigned mt, unsigned seek)
{
    if (s_tr.play != play || s_tr.cue != cue || s_tr.mt != mt || s_tr.seek != seek || s_tr.hotcue != 0) {
        fprintf(stderr, "FAIL: %s: play %u cue %u mt %u seek %u hotcue %u, expected %u %u %u %u 0\n",
                step, s_tr.play, s_tr.cue, s_tr.mt, s_tr.seek, s_tr.hotcue, play, cue, mt, seek);
        s_failures++;
    }
}

static void bridge_transport_scenario(void)
{
    static const dj_ui_callbacks_t cb = {
        .on_play = tr_on_play, .on_cue = tr_on_cue, .on_master_tempo = tr_on_mt,
        .on_seek = tr_on_seek, .on_hotcue = tr_on_hotcue,
    };
    dj_ui_set_callbacks(&cb);
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
    memset(&s_tr, 0, sizeof s_tr);

    lv_indev_t *ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, ptr_read);
    s_ptr_pressed = false;

    /* Play / pause / cue / tempo / MT changes as the firmware pushes them. */
    ui_djui_frame_t f = {0};
    uint32_t p0 = 60000, p1 = 29000;
    for (int i = 0; i < 40; i++) {
        overview_frame(&f, p0, p1);
        f.deck[0].playing = (i / 5) % 2 == 0;
        f.deck[1].playing = (i / 7) % 2 == 0;
        f.deck[0].cue_point_ms = (i / 10) % 2 ? 60000 : p0;
        f.deck[0].master_tempo = (i / 8) % 2 == 0;
        f.deck[0].tempo_pct = (float)(i % 5) - 2.0f;
        ui_djui_bridge_update(&f);
        pump(16);
        if (f.deck[0].playing) p0 += 16;
        if (f.deck[1].playing) p1 += 16;
    }
    expect_transport("bridge updates", 0, 0, 0, 0);

    lv_obj_t *play = find_label_ex(lv_screen_active(), LV_SYMBOL_PLAY, true, true);
    lv_obj_t *cue = find_label_ex(lv_screen_active(), "CUE", true, true);
    lv_obj_t *mt = find_label_ex(lv_screen_active(), "MT", true, true);
    if (!play || !cue || !mt) {
        fail("transport: D1 PLAY / CUE / MT buttons not found");
    } else {
        tap_obj(play, "PLAY");
        expect_transport("PLAY tap", 1, 0, 0, 0);
        tap_obj(cue, "CUE");
        expect_transport("CUE tap", 1, 1, 0, 0);
        tap_obj(mt, "MT");
        expect_transport("MT tap", 1, 1, 1, 0);
        if (s_tr.deck != 0) fail("transport: tap reported the wrong deck");
    }

    for (int i = 0; i < 20; i++) {
        overview_frame(&f, p0 += 16, p1 += 16);
        f.deck[0].playing = i % 2 == 0;
        ui_djui_bridge_update(&f);
        pump(16);
    }
    expect_transport("bridge updates after taps", 1, 1, 1, 0);

    lv_indev_delete(ptr);
    dj_ui_set_callbacks(&(dj_ui_callbacks_t){0});
}

/* v264: every FX panel control reports its tap, and the bridge's pushes show
 * the deck_core state (target, time, level) without fighting a held slider. */
static struct {
    unsigned select, channel, toggle, beat_taps, level_calls;
    int beat, level;
} s_fx;
static void fx_on_select(void) { s_fx.select++; }
static void fx_on_channel(void) { s_fx.channel++; }
static void fx_on_toggle(void) { s_fx.toggle++; }
static void fx_on_beat(uint8_t i) { s_fx.beat_taps++; s_fx.beat = i; }
static void fx_on_level(uint8_t pct) { s_fx.level_calls++; s_fx.level = pct; }

static void bridge_fx_scenario(void)
{
    static const dj_ui_callbacks_t cb = {
        .on_fx_select = fx_on_select, .on_fx_channel = fx_on_channel, .on_fx_toggle = fx_on_toggle,
        .on_fx_beat = fx_on_beat, .on_fx_level = fx_on_level,
    };
    dj_ui_set_callbacks(&cb);
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
    memset(&s_fx, 0, sizeof s_fx);

    lv_indev_t *ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, ptr_read);
    s_ptr_pressed = false;

    ui_djui_frame_t f = {0};
    overview_frame(&f, 60000, 29000);
    ui_djui_bridge_update(&f);
    pump(64);
    expect_label("ECHO");
    expect_label("CH 1");
    expect_label("469 ms");
    expect_label("50%");
    if (s_fx.select || s_fx.channel || s_fx.toggle || s_fx.beat_taps || s_fx.level_calls) {
        fail("fx: bridge push fired a callback");
    }

    tap_obj(find_visible_label("ECHO"), "FX name");
    tap_obj(find_visible_label("CH 1"), "FX CH");
    tap_obj(find_label_ex(lv_screen_active(), "1/2", true, true), "FX 1/2");
    tap_obj(find_label_ex(lv_screen_active(), "ON", true, true), "FX ON");
    if (s_fx.select != 1 || s_fx.channel != 1 || s_fx.toggle != 1 || s_fx.beat_taps != 1 || s_fx.beat != 1) {
        fprintf(stderr, "FAIL: fx taps: select %u channel %u toggle %u beat %u/%d\n",
                s_fx.select, s_fx.channel, s_fx.toggle, s_fx.beat_taps, s_fx.beat);
        s_failures++;
    }

    /* LEVEL: drag the knob from 50 % to 3/4, the state echo waits for the release. */
    lv_obj_t *lvl = find_visible_label("50%");
    lv_obj_t *panel = lvl ? lv_obj_get_parent(lvl) : NULL, *slider = NULL;
    for (uint32_t i = 0; panel && i < lv_obj_get_child_count(panel); i++) {
        lv_obj_t *c = lv_obj_get_child(panel, (int32_t)i);
        if (lv_obj_check_type(c, &lv_slider_class)) slider = c;
    }
    if (!slider) {
        fail("fx: LEVEL slider not found");
    } else {
        lv_area_t a;
        lv_obj_get_coords(slider, &a);
        s_ptr_point = (lv_point_t){ a.x1 + lv_area_get_width(&a) / 2, (a.y1 + a.y2) / 2 };
        s_ptr_pressed = true;
        pump(48);
        for (int step = 1; step <= 6; step++) {
            s_ptr_point.x = a.x1 + lv_area_get_width(&a) / 2 + lv_area_get_width(&a) * step / 24;
            pump(32);
        }
        if (s_fx.level_calls == 0) fail("fx: LEVEL press did not report");
        else expect_near("fx level", s_fx.level, 75, 4);
        f.fx.level_pct = 20;          /* stale state while held */
        ui_djui_bridge_update(&f);
        pump(32);
        if (lv_slider_get_value(slider) != s_fx.level) fail("fx: held LEVEL slider moved by the bridge");
        s_ptr_pressed = false;
        pump(96);
        f.fx.level_pct = (uint8_t)s_fx.level;   /* deck_core echo */
        ui_djui_bridge_update(&f);
        pump(32);
        char want[8];
        snprintf(want, sizeof want, "%d%%", s_fx.level);
        expect_label(want);
    }

    /* Target both decks, Filter has no time. */
    f.fx = (ui_djui_fx_view_t){ .name = "FILTER", .channel = 0, .beat_index = 4, .time_ms = 0,
                                .level_pct = 100, .on = false };
    ui_djui_bridge_update(&f);
    pump(64);
    expect_label("FILTER");
    expect_label("1+2");
    expect_label("- ms");
    expect_label("100%");
    expect_label("OFF");

    lv_indev_delete(ptr);
    dj_ui_set_callbacks(&(dj_ui_callbacks_t){0});
}

/* ---- Artwork: the firmware thumbnail decode (ui_artwork_thumb, TJpgDec)
 * on synthetic JPEGs, then the copies dj_ui draws in rows and deck header ---- */
static ui_artwork_thumb_work_t s_art_work;
static ui_artwork_thumb_t s_art_quad, s_art_gray, s_art_bad;

static void expect_rgb565(const char *what, uint16_t px, int r, int g, int b)
{
    int pr = (px >> 11) << 3, pg = ((px >> 5) & 0x3f) << 2, pb = (px & 0x1f) << 3;
    if (abs(pr - r) > 24 || abs(pg - g) > 24 || abs(pb - b) > 24) {
        fprintf(stderr, "FAIL: %s = rgb(%d,%d,%d), expected about rgb(%d,%d,%d)\n", what, pr, pg, pb, r, g, b);
        s_failures++;
    }
}

static void expect_fb_rgb(const char *what, int32_t x, int32_t y, int r, int g, int b)
{
    uint32_t px = s_framebuffer[y * DISPLAY_WIDTH + x];
    int pr = (px >> 16) & 0xff, pg = (px >> 8) & 0xff, pb = px & 0xff;
    if (abs(pr - r) > 24 || abs(pg - g) > 24 || abs(pb - b) > 24) {
        fprintf(stderr, "FAIL: %s = rgb(%d,%d,%d), expected about rgb(%d,%d,%d)\n", what, pr, pg, pb, r, g, b);
        s_failures++;
    }
}

/* First visible lv_image under root (a row, a deck header art box). */
static lv_obj_t *find_image(lv_obj_t *root)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *c = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(c, &lv_image_class)) return c;
        lv_obj_t *deep = find_image(c);
        if (deep) return deep;
    }
    return NULL;
}

static void artwork_decode_checks(void)
{
    if (!ui_artwork_thumb_decode(ART_FIXTURE_QUAD, sizeof ART_FIXTURE_QUAD, &s_art_work, &s_art_quad)) {
        fail("artwork: 80x80 baseline 4:2:0 JPEG did not decode");
    } else {
        const int n = UI_ARTWORK_ROW_PX, q = n / 4, m = UI_ARTWORK_DECK_PX;
        expect_rgb565("row art top-left", s_art_quad.row[q * n + q], 255, 0, 0);
        expect_rgb565("row art top-right", s_art_quad.row[q * n + n - 1 - q], 0, 255, 0);
        expect_rgb565("row art bottom-left", s_art_quad.row[(n - 1 - q) * n + q], 0, 0, 255);
        expect_rgb565("row art bottom-right", s_art_quad.row[(n - 1 - q) * n + n - 1 - q], 255, 255, 255);
        expect_rgb565("deck art top-left", s_art_quad.deck[(m / 4) * m + m / 4], 255, 0, 0);
        expect_rgb565("deck art bottom-right", s_art_quad.deck[(m - 1 - m / 4) * m + m - 1 - m / 4], 255, 255, 255);
    }
    if (!ui_artwork_thumb_decode(ART_FIXTURE_GRAY, sizeof ART_FIXTURE_GRAY, &s_art_work, &s_art_gray)) {
        fail("artwork: 240x240 grayscale JPEG did not decode");
    } else {
        expect_rgb565("gray art centre", s_art_gray.row[20 * UI_ARTWORK_ROW_PX + 20], 128, 128, 128);
        expect_rgb565("gray art corner", s_art_gray.deck[0], 128, 128, 128);
    }
    if (ui_artwork_thumb_decode(ART_FIXTURE_PROGRESSIVE, sizeof ART_FIXTURE_PROGRESSIVE, &s_art_work, &s_art_bad)) {
        fail("artwork: progressive JPEG should be refused");
    }
    if (ui_artwork_thumb_decode(ART_FIXTURE_QUAD, 200, &s_art_work, &s_art_bad)) {
        fail("artwork: truncated JPEG should be refused");
    }
    static const uint8_t not_jpeg[16] = { 0x89, 'P', 'N', 'G' };
    if (ui_artwork_thumb_decode(not_jpeg, sizeof not_jpeg, &s_art_work, &s_art_bad) ||
        ui_artwork_thumb_decode(NULL, 0, &s_art_work, &s_art_bad)) {
        fail("artwork: non-JPEG input should be refused");
    }
}

static void bridge_artwork_scenario(void)
{
    artwork_decode_checks();

    /* Library: thumbnails with the page, one landing later (lazy load). */
    dj_ui_show_tab(DJ_TAB_LIBRARY);
    const dj_track_t rows[3] = {
        { .title = "Art Track Quad", .artist = "Artist A", .key = "8A", .bpm = 124, .len_ms = 245000,
          .art = s_art_quad.row },
        { .title = "Art Track Late", .artist = "Artist B", .key = "11B", .bpm = 128, .len_ms = 372000 },
        { .title = "Art Track None", .artist = "Artist C", .key = "2A", .bpm = 174, .len_ms = 301000 },
    };
    ui_djui_bridge_library_set_rows(rows, 3);
    ui_djui_library_view_t v = {
        .source = "LOCAL USB", .total = 3, .page = 1, .pages = 1,
        .selected = -1, .loaded = { -1, -1 },
        .status_deck = 0, .deck_status = "READY",
        .load_enabled = true, .progress = -1, .source_label = "SOURCE",
    };
    ui_djui_bridge_library_update(&v);
    pump(64);
    lv_obj_t *t0 = find_visible_label("Art Track Quad"), *t1 = find_visible_label("Art Track Late");
    lv_obj_t *t2 = find_visible_label("Art Track None");
    if (!t0 || !t1 || !t2) {
        fail("artwork: library rows missing");
        return;
    }
    if (find_image(lv_obj_get_parent(t1)) || find_image(lv_obj_get_parent(t2))) {
        fail("artwork: row without artwork shows an image");
    }
    ui_djui_bridge_library_set_row_art(1, s_art_gray.row);
    pump(64);
    capture("library_bridge_art");
    lv_obj_t *img0 = find_image(lv_obj_get_parent(t0)), *img1 = find_image(lv_obj_get_parent(t1));
    if (!img0 || !img1) {
        fail("artwork: row thumbnail not shown");
    } else {
        lv_area_t a;
        lv_obj_get_coords(img0, &a);
        expect_near("row thumbnail width", lv_area_get_width(&a), UI_ARTWORK_ROW_PX, 0);
        expect_fb_rgb("row 0 thumbnail top-left", a.x1 + 8, a.y1 + 8, 255, 0, 0);
        expect_fb_rgb("row 0 thumbnail bottom-right", a.x2 - 8, a.y2 - 8, 255, 255, 255);
        lv_obj_get_coords(img1, &a);
        expect_fb_rgb("row 1 late thumbnail", a.x1 + 20, a.y1 + 20, 128, 128, 128);
    }
    /* Dropped again (e.g. the catalog changed under the page). */
    ui_djui_bridge_library_set_row_art(1, NULL);
    pump(16);
    if (find_image(lv_obj_get_parent(t1))) fail("artwork: cleared row thumbnail still shown");

    /* Deck header: the thumbnail follows (art_key, art); NULL = "ART". */
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
    ui_djui_frame_t f;
    overview_frame(&f, 90000, 45000);
    f.deck[0].title = "Art Deck One";
    f.deck[0].art_key = 0x1234;
    f.deck[0].art = s_art_quad.deck;
    ui_djui_bridge_update(&f);
    pump(64);
    lv_obj_t *dt = find_visible_label("Art Deck One");
    lv_obj_t *dimg = dt ? find_image(lv_obj_get_parent(dt)) : NULL;
    lv_area_t a;
    if (!dimg) {
        fail("artwork: deck thumbnail not shown");
    } else {
        lv_obj_get_coords(dimg, &a);
        expect_near("deck thumbnail width", lv_area_get_width(&a), UI_ARTWORK_DECK_PX, 0);
    }
    capture("overview_bridge_art");
    if (dimg) {
        expect_fb_rgb("deck thumbnail top-left", a.x1 + 6, a.y1 + 6, 255, 0, 0);
        expect_fb_rgb("deck thumbnail bottom-right", a.x2 - 6, a.y2 - 6, 255, 255, 255);
    }
    /* Another track, still decoding: placeholder until its pixels land. */
    f.deck[0].art_key = 0x5678;
    f.deck[0].art = NULL;
    ui_djui_bridge_update(&f);
    pump(16);
    if (dt && find_image(lv_obj_get_parent(dt))) fail("artwork: stale deck thumbnail after track change");
    f.deck[0].art = s_art_gray.deck;
    ui_djui_bridge_update(&f);
    pump(16);
    dimg = dt ? find_image(lv_obj_get_parent(dt)) : NULL;
    if (!dimg) {
        fail("artwork: deck thumbnail did not land");
    } else {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_display);
        lv_obj_get_coords(dimg, &a);
        expect_fb_rgb("deck gray thumbnail", a.x1 + 17, a.y1 + 17, 128, 128, 128);
    }
}


/* UI_SIM_DEBUG only: both decks playing through the bridge, per frame: host
 * bridge + refresh cost and what LVGL invalidates. Ratios, not P4 numbers. */
static uint32_t s_bench_inv_n, s_bench_inv_px;
static bool s_bench_dump;
static void bench_inval_cb(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_invalidated_area(e);
    if (!a) return;
    s_bench_inv_n++;
    s_bench_inv_px += (uint32_t)lv_area_get_size(a);
    if (s_bench_dump) {
        fprintf(stderr, "[UI_SIM_DEBUG]   inval (%d,%d %dx%d)\n", (int)a->x1, (int)a->y1,
                (int)lv_area_get_width(a), (int)lv_area_get_height(a));
    }
}

/* What LVGL renders once the areas are joined (a full screen after an
 * overflow of its invalidation buffer). */
static uint32_t s_bench_rendered_px;
static void bench_render_cb(lv_event_t *e)
{
    const lv_display_t *disp = lv_event_get_target(e);
    for (uint32_t i = 0; i < disp->inv_p; i++) {
        if (!disp->inv_area_joined[i]) s_bench_rendered_px += (uint32_t)lv_area_get_size(&disp->inv_areas[i]);
    }
}

static double bench_us(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1e6 + (double)(b->tv_nsec - a->tv_nsec) / 1e3;
}

static void bench_playing(const char *label, dj_tab_t tab, bool deck_art, int frames)
{
    dj_ui_show_tab(tab);
    ui_djui_frame_t f;
    uint32_t p0 = 70000, p1 = 40000;
    double upd = 0, refr = 0, refr_max = 0;
    uint32_t inv_n = 0, inv_px = 0;
    for (int i = -4; i < frames; i++) {
        p0 += 33; p1 += 33;           /* 30 fps playback */
        overview_frame(&f, p0, p1);
        f.overview_visible = tab == DJ_TAB_OVERVIEW;
        for (uint8_t d = 0; d < DJ_DECKS; d++) {
            f.deck[d].art_key = deck_art ? 0x100u + d : 0u;
            f.deck[d].art = deck_art ? s_art_quad.deck : NULL;
        }
        lv_tick_inc(33);
        struct timespec a, b, c;
        s_bench_inv_n = s_bench_inv_px = 0;
        s_bench_dump = i == frames - 1;
        if (s_bench_dump) fprintf(stderr, "[UI_SIM_DEBUG] %s: last frame invalidations\n", label);
        clock_gettime(CLOCK_MONOTONIC, &a);
        ui_djui_bridge_update(&f);
        if (tab == DJ_TAB_LIBRARY) {
            /* ui_library_djui_publish pushes the view every frame */
            ui_djui_library_view_t v = {
                .source = "LOCAL USB", .total = 8, .page = 1, .pages = 1,
                .selected = 2, .loaded = { 0, 3 }, .status_deck = DJ_DECKS, .deck_status = "ACTIVE",
                .load_enabled = true, .progress = -1, .source_label = "SOURCE",
            };
            ui_djui_bridge_library_update(&v);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        lv_timer_handler();
        sim_post_refresh();
        lv_refr_now(s_display);
        sim_post_refresh();
        clock_gettime(CLOCK_MONOTONIC, &c);
        s_bench_dump = false;
        if (i < 0) continue;          /* warm-up */
        upd += bench_us(&a, &b);
        double r = bench_us(&b, &c);
        refr += r;
        if (r > refr_max) refr_max = r;
        inv_n += s_bench_inv_n;
        inv_px += s_bench_inv_px;
    }
    fprintf(stderr, "[UI_SIM_DEBUG] %-28s bridge %.1f us, refr %.1f us (max %.1f), "
            "inval %.1f areas %.0f px per frame\n", label, upd / frames, refr / frames, refr_max,
            (double)inv_n / frames, (double)inv_px / frames);
}

/* UI_SIM_DEBUG only: what one Library interaction invalidates and costs. */
static void bench_library_step(const char *label, int kind, dj_track_t *rows, int frames)
{
    ui_djui_library_view_t v = {
        .source = "LOCAL USB", .total = 64, .page = 1, .pages = 8,
        .selected = 0, .loaded = { -1, -1 }, .status_deck = DJ_DECKS, .deck_status = "ACTIVE",
        .load_enabled = true, .progress = -1, .source_label = "SOURCE",
    };
    double refr = 0, refr_max = 0;
    uint32_t inv_n = 0, inv_px = 0;
    uint64_t rendered = 0;
    for (int i = -2; i < frames; i++) {
        lv_tick_inc(33);
        s_bench_inv_n = s_bench_inv_px = 0;
        s_bench_rendered_px = 0;
        s_bench_dump = i == frames - 1;
        if (s_bench_dump) fprintf(stderr, "[UI_SIM_DEBUG] %s: last frame invalidations\n", label);
        if (kind == 0) {                      /* encoder: selection moves inside the page */
            v.selected = (int8_t)((i + 8) % 8);
        } else if (kind == 1) {               /* page flip, thumbnails still decoding */
            for (int r = 0; r < 8; r++) rows[r].art = NULL;
            ui_djui_bridge_library_set_rows(rows, 8);
            v.page = (uint16_t)(1 + (i + 8) % 8);
        } else {                              /* one thumbnail lands */
            ui_djui_bridge_library_set_row_art((uint8_t)((i + 8) % 8),
                                               (i & 1) ? s_art_quad.row : s_art_gray.row);
        }
        ui_djui_bridge_library_update(&v);
        struct timespec b, c;
        clock_gettime(CLOCK_MONOTONIC, &b);
        lv_timer_handler();
        sim_post_refresh();
        lv_refr_now(s_display);
        sim_post_refresh();
        clock_gettime(CLOCK_MONOTONIC, &c);
        s_bench_dump = false;
        if (i < 0) continue;
        double r = bench_us(&b, &c);
        refr += r;
        if (r > refr_max) refr_max = r;
        inv_n += s_bench_inv_n;
        inv_px += s_bench_inv_px;
        rendered += s_bench_rendered_px;
    }
    fprintf(stderr, "[UI_SIM_DEBUG] %-28s refr %.1f us (max %.1f), inval %.1f areas %.0f px, "
            "rendered %.0f px per step\n", label, refr / frames, refr_max, (double)inv_n / frames,
            (double)inv_px / frames, (double)rendered / frames);
}

static void bench_library_nav(dj_track_t *rows)
{
    dj_ui_show_tab(DJ_TAB_LIBRARY);
    lv_display_add_event_cb(s_display, bench_render_cb, LV_EVENT_RENDER_START, NULL);
    bench_library_step("library, select move", 0, rows, 64);
    bench_library_step("library, page flip", 1, rows, 64);
    bench_library_step("library, thumbnail lands", 2, rows, 64);
    lv_display_remove_event_cb_with_user_data(s_display, bench_render_cb, NULL);
}

static void playing_render_timing(void)
{
    if (!getenv("UI_SIM_DEBUG")) return;
    lv_display_add_event_cb(s_display, bench_inval_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    bench_playing("overview, no deck art", DJ_TAB_OVERVIEW, false, 200);
    bench_playing("overview, deck art", DJ_TAB_OVERVIEW, true, 200);
    s_sim_direct = true;              /* v287: refr includes the sim blits */
    ui_djui_bridge_set_direct_blit(sim_direct_blit);
    bench_playing("overview, direct strips", DJ_TAB_OVERVIEW, true, 200);
    s_sim_direct = false;
    ui_djui_bridge_set_direct_blit(NULL);

    static dj_track_t rows[8];
    char titles[8][24];
    for (int r = 0; r < 8; r++) {
        snprintf(titles[r], sizeof titles[r], "Bench Track %d", r);
        rows[r] = (dj_track_t){ .title = titles[r], .artist = "Bench", .key = "8A", .bpm = 128,
                                .len_ms = 300000, .art = NULL };
    }
    ui_djui_bridge_library_set_rows(rows, 8);
    bench_playing("library, no row art", DJ_TAB_LIBRARY, true, 200);
    for (int r = 0; r < 8; r++) rows[r].art = s_art_quad.row;
    ui_djui_bridge_library_set_rows(rows, 8);
    bench_playing("library, row art", DJ_TAB_LIBRARY, true, 200);
    bench_library_nav(rows);
    lv_display_remove_event_cb_with_user_data(s_display, bench_inval_cb, NULL);
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
}

static void bridge_scenario(void)
{
    dj_ui_demo_stop();
    lv_obj_t *old = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    lv_obj_delete(old);
    if (!ui_djui_bridge_create(scr)) {
        fail("bridge: mini waveform buffers missing");
    }

    static uint8_t wave[UI_DJUI_WAVEFORM_LOW_LEN];
    for (int i = 0; i < UI_DJUI_WAVEFORM_LOW_LEN; i++) wave[i] = (uint8_t)((i * 7) % 32);
    ui_djui_frame_t f = { .now_ms = lv_tick_get() };
    f.deck[0] = (ui_djui_deck_view_t){
        .loaded = true, .title = "Bridge Title", .artist = "Bridge Artist", .bpm = 128,
        .tempo_pct = 2.0f, .duration_ms = 300000, .position_ms = 61500, .playing = true,
        .waveform_low = wave,
    };
    ui_djui_bridge_update(&f);
    ui_djui_bridge_status_hold("BRIDGE LOADING", DJ_TONE_INFO, 500, f.now_ms);
    pump(64);
    expect_label("Bridge Title");
    expect_label("Bridge Artist");
    expect_label("130.6");            /* 128 BPM at +2.0 % */
    expect_label("+2.0%");
    expect_label("01:01.5");
    expect_label("03:58.5");          /* remaining */
    expect_label("No Track");         /* deck 2 not loaded */
    expect_label("BRIDGE LOADING");

    f.now_ms += 600;                  /* past the status hold */
    f.deck[0].position_ms = 62000;
    wave[0] ^= 0x1f;                  /* same buffer, new content: re-render */
    ui_djui_bridge_update(&f);
    pump(64);
    expect_label("01:02.0");
    expect_hidden_label("BRIDGE LOADING");

    f.deck[0].loaded = false;         /* eject */
    ui_djui_bridge_update(&f);
    pump(64);
    expect_hidden_label("Bridge Title");

    bridge_settings_scenario();
    bridge_hotcues_scenario();
    bridge_library_scenario();
    bridge_overview_scenario();
    bridge_transport_scenario();
    bridge_fx_scenario();
    bridge_library_playlists_scenario();
    bridge_artwork_scenario();
    playing_render_timing();
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dj_ui_simulator_e2e <screenshot-output-dir>\n");
        return 2;
    }
    s_output_dir = argv[1];

    lv_init();
    s_display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!s_display) {
        fail("lv_display_create failed");
        return 1;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(s_display, s_framebuffer, NULL, sizeof(s_framebuffer),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_display, flush_cb);
    lv_display_set_default(s_display);

    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read_cb);
    lv_indev_set_display(pointer, s_display);

    dj_ui_create(lv_screen_active());
    dj_ui_demo_start();
    pump(1000);

    /* ---- Overview: D1 PEAKS surfaces, D2 IMAGE surfaces, cue/loop markers,
     * transport, beat, VU, status + DJ Link master pills ---- */
    expect_label("OVERVIEW");
    expect_label("Etherwood - TANGARA.mp3");
    expect_label("D1 PLAY");
    uint64_t overview = capture("overview");

    pump(500);
    if (capture("overview_playing") == overview) {
        fail("playback timer produced no visible change");
    }

    /* Surface geometry: the zoom playhead is the centre column; IMAGE zoom
     * gives the header band back to LVGL. */
    lv_area_t z0, z1, m0;
    if (!dj_ui_wave_get_area(0, DJ_WAVE_ZOOM, &z0) || !dj_ui_wave_get_area(1, DJ_WAVE_ZOOM, &z1) ||
        !dj_ui_wave_get_area(0, DJ_WAVE_MINI, &m0)) {
        fail("dj_ui_wave_get_area returned false");
    } else {
        int32_t x = -1;
        if (!dj_ui_wave_ms_to_x(0, DJ_WAVE_ZOOM, dj_ui_demo_position(0), &x)) {
            fail("zoom playhead outside surface");
        }
        expect_near("D1 zoom playhead x", x, lv_area_get_width(&z0) / 2, 1);
        if (lv_area_get_height(&z1) >= lv_area_get_height(&z0)) {
            fail("IMAGE zoom surface does not exclude the LVGL header band");
        }
        if (dj_ui_wave_ms_to_x(0, DJ_WAVE_ZOOM, dj_ui_demo_position(0) + 60000, &x)) {
            fail("ms_to_x accepted a time outside the zoom window");
        }
        if (dj_ui_wave_get_window_ms(0) != 8000) {
            fail("default zoom window is not 8000 ms");
        }
    }

    /* Seek: tap the middle of the D1 mini waveform -> half the track (223 s). */
    tap((m0.x1 + m0.x2) / 2, (m0.y1 + m0.y2) / 2);
    expect_near("D1 position after mini seek", dj_ui_demo_position(0), 111500, 600);

    /* CUE while playing: back to the cue point and pause (CDJ style). */
    click_button("CUE");
    if (dj_ui_demo_playing(0)) {
        fail("CUE did not pause Deck 1");
    }
    click_button("MT");
    capture("overview_cue");

    /* EXTERNAL zoom: dj_ui leaves the surface to the caller's direct blit. */
    dj_ui_wave_set_source(0, DJ_WAVE_ZOOM, DJ_WAVE_SRC_EXTERNAL);
    pump(100);
    capture("overview_external");
    dj_ui_wave_set_source(0, DJ_WAVE_ZOOM, DJ_WAVE_SRC_PEAKS);

    /* ---- Library: sort, select, load onto Deck 2 through the load gate ---- */
    if (click_label("LIBRARY")) {
        expect_label("TITLE");
        expect_label("SORT: LOCAL ONLY");
        capture("library");
    }
    click_label("SORT BPM");
    click_label("Kleu - Insane.mp3");
    click_label("LOAD DECK 2");
    expect_label("LOADING 20%");
    expect_label("D2 LOAD 20%");
    capture("library_busy");
    click_label("LOAD DECK 1");            /* gate closed: must be ignored */
    pump(600);
    expect_label("LOADED");
    expect_label("D2 LOADED FROM #3");
    expect_label("TRACK LOADED");
    click_label("SOURCE");
    expect_label("CANCEL");
    capture("library_loaded");

    /* ---- Hot Cues: switch target to Deck 2 (loop pad shows LOOP) ---- */
    if (click_label("HOT CUES")) {
        click_label("D2");
        dj_ui_set_hotcue(1, 1, true, 30000, 4);
        dj_ui_set_hotcue_loop(1, 1, true);
        pump(64);
        expect_label("CUE A");
        expect_label("LOOP B");
        capture("hot_cues");
    }

    /* ---- Settings: controller name, DJ Link peers, tappable fields, record ---- */
    if (click_label("SETTINGS")) {
        expect_label("SYSTEM STATUS");
        expect_label("Controller (USB1): DDJ-400 connected");
        expect_label("CDJ-3000 #1   174.2 BPM   MASTER   ON AIR   PLAY");
        expect_label("XDJ-XZ #3   128.0 BPM   ON AIR");
        click_label("MAIN: USB (DDJ)");
        expect_label("MAIN: PCM5102A RCA");
        click_button("RECORD");
        expect_label("STOP REC");
        pump(2000);
        capture("settings");
    }

    /* DJ Link off: peers cleared, top-bar master pill reads LINK OFF. */
    dj_ui_set_link_enabled(false);
    lv_obj_t *link_sw = NULL;
    {
        lv_obj_t *lbl = find_visible_label("DJ LINK: ON P4 - CDJ-3000 #1 174.2 BPM ON AIR");
        lv_obj_t *boxp = lbl ? lv_obj_get_parent(lbl) : NULL;
        for (uint32_t i = 0; boxp && i < lv_obj_get_child_count(boxp); i++) {
            lv_obj_t *c = lv_obj_get_child(boxp, (int32_t)i);
            if (lv_obj_check_type(c, &lv_switch_class)) link_sw = c;
        }
    }
    if (!link_sw) {
        fail("DJ Link switch not found");
    } else {
        lv_obj_send_event(link_sw, LV_EVENT_VALUE_CHANGED, NULL);
        pump(64);
        expect_label("LINK OFF");
        expect_label("DJ LINK: OFF");
        expect_hidden_label("CDJ-3000 #1   174.2 BPM   MASTER   ON AIR   PLAY");
        capture("settings_link_off");
        lv_obj_add_state(link_sw, LV_STATE_CHECKED);
        lv_obj_send_event(link_sw, LV_EVENT_VALUE_CHANGED, NULL);
        pump(64);
    }

    /* ---- Back to Overview: the loaded Deck 2 track is shown ---- */
    click_label("OVERVIEW");
    expect_label("Kleu - Insane.mp3");
    capture("overview_restored");

    /* ---- Screensaver: animates on lv_layer_top, a tap wakes it ---- */
    dj_ui_set_screensaver(true);
    pump(600);
    if (!dj_ui_screensaver_active()) {
        fail("screensaver did not activate");
    }
    expect_label("touch me.....or don't ;)");
    capture("screensaver");
    tap(512, 300);
    if (dj_ui_screensaver_active()) {
        fail("tap did not dismiss the screensaver");
    }
    expect_label("WAKE");

    /* ---- Blackout test frame ---- */
    dj_ui_set_blackout(true);
    pump(64);
    expect_label("UI OFF - PLAYING");
    capture("blackout");
    dj_ui_set_blackout(false);
    pump(64);
    expect_hidden_label("UI OFF - PLAYING");

    wave_source_timing();
    bridge_scenario();

    if (s_failures != 0) {
        fprintf(stderr, "dj_ui simulator E2E failed: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("dj_ui simulator E2E scenario passed.\n");
    return 0;
}
