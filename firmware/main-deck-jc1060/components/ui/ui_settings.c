#include "ui_settings.h"
#ifndef WIN32
#if CONFIG_AUDIO_RECORDER_ENABLED
#include "audio_recorder.h"
#endif
#include "service_log.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <string.h>

bool ui_settings_should_poll(uint32_t now_ms,
                             uint32_t last_poll_ms,
                             bool force,
                             uint32_t interval_ms)
{
    return force || last_poll_ms == 0 || (uint32_t)(now_ms - last_poll_ms) >= interval_ms;
}

const char *ui_settings_cue_mode_name(uint8_t mode)
{
    switch (mode) {
    case 1:
        return "CUE: SPLIT MONO";
    default:
        return "CUE: STEREO";
    }
}

typedef struct {
    const char *label;
    float gain;
} ui_settings_master_trim_preset_t;

static const ui_settings_master_trim_preset_t s_master_trim_presets[] = {
    { "MASTER: 0 dB", 1.0f },
    { "MASTER: -3 dB", 0.7079458f },
    { "MASTER: -6 dB", 0.5011872f },
};

uint8_t ui_settings_master_trim_preset_count(void)
{
    return (uint8_t)(sizeof(s_master_trim_presets) / sizeof(s_master_trim_presets[0]));
}

uint8_t ui_settings_master_trim_sanitize_preset(uint8_t preset)
{
    return preset < ui_settings_master_trim_preset_count() ? preset : 0u;
}

uint8_t ui_settings_master_trim_next_preset(uint8_t current)
{
    uint8_t count = ui_settings_master_trim_preset_count();
    if (current >= count || count == 0u) {
        return 0u;
    }
    return (uint8_t)((current + 1u) % count);
}

float ui_settings_master_trim_gain(uint8_t preset)
{
    preset = ui_settings_master_trim_sanitize_preset(preset);
    return s_master_trim_presets[preset].gain;
}

const char *ui_settings_master_trim_label(uint8_t preset)
{
    preset = ui_settings_master_trim_sanitize_preset(preset);
    return s_master_trim_presets[preset].label;
}

#ifndef UI_SETTINGS_HOST_TEST

#include "esp_log.h"
#include "ui_theme.h"

#ifndef WIN32
#include "app_settings.h"
#include "audio_engine.h"
#include "bsp_jc4880.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "firmware_health.h"
#include "controller_profile_manager.h"
#include "deck_core.h"
#include "dj_link.h"
#include "ui_djui_bridge.h"
#endif

static const char *TAG = "ui_settings";

static uint8_t s_master_trim_preset = 0;
static ui_settings_wifi_toggle_cb_t s_wifi_toggle_cb = NULL;
static ui_settings_recording_toggle_cb_t s_recording_toggle_cb = NULL;
static ui_settings_dj_link_toggle_cb_t s_dj_link_toggle_cb = NULL;

#ifndef WIN32
static const char *ui_settings_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power-on";
    case ESP_RST_EXT:       return "External pin";
    case ESP_RST_SW:        return "Software";
    case ESP_RST_PANIC:     return "PANIC / exception";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power dip)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB peripheral";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "Unknown";
    }
}
#endif

void ui_settings_set_recording_toggle_cb(ui_settings_recording_toggle_cb_t cb)
{
    s_recording_toggle_cb = cb;
}

#ifndef WIN32

static const char *jog_mode_name(uint8_t cdj)
{
    return cdj ? "JOG: CDJ" : "JOG: VINYL";
}

/* v267: persisted jog mode, applied to deck_core at once (docs/JOG_MODES_VINYL_VS_CDJ.md). */
static uint8_t jog_mode_toggle(void)
{
    uint8_t next = app_settings_get().jog_cdj_mode ? 0u : 1u;
    app_settings_set_jog_cdj_mode(next);
    deck_core_set_jog_cdj_mode(next != 0u);
    ESP_LOGI(TAG, "Jog mode saved: %s", jog_mode_name(next));
    return next;
}

static const char *tempo_range_name(uint8_t pct)
{
    switch (pct) {
    case 6:  return "TEMPO: +/-6%";
    case 16: return "TEMPO: +/-16%";
    default: return "TEMPO: +/-10%";
    }
}

/* v275: persisted tempo fader range for both decks, cycles 6 -> 10 -> 16
 * like Shift+TEMPO RANGE; deck_core rescales the pitch faders on its task. */
static uint8_t tempo_range_cycle(void)
{
    uint8_t cur = (uint8_t)deck_core_get_tempo_range_percent();
    uint8_t next = cur == 6u ? 10u : (cur == 10u ? 16u : 6u);
    app_settings_set_tempo_range_pct(next);
    deck_core_set_tempo_range_percent(next);
    ESP_LOGI(TAG, "Tempo range saved: %s", tempo_range_name(next));
    return next;
}

/* v275: Shift+TEMPO RANGE changes the range on the deck task, which must not
 * block on NVS; the UI task persists it here, like every other setting.
 * Returns the live range. */
static uint8_t tempo_range_sync(void)
{
    static uint8_t s_tried;   /* one NVS attempt per value, not one per frame */
    uint8_t live = (uint8_t)deck_core_get_tempo_range_percent();
    if (app_settings_get().tempo_range_pct == live) {
        s_tried = 0u;
        return live;
    }
    if (s_tried != live) {
        s_tried = live;
        app_settings_set_tempo_range_pct(live);
        ESP_LOGI(TAG, "Tempo range saved: %s", tempo_range_name(live));
    }
    return live;
}

#endif

void ui_settings_set_wifi_toggle_cb(ui_settings_wifi_toggle_cb_t cb)
{
    s_wifi_toggle_cb = cb;
}

void ui_settings_set_dj_link_toggle_cb(ui_settings_dj_link_toggle_cb_t cb)
{
    s_dj_link_toggle_cb = cb;
}

#ifndef WIN32

#endif

#ifndef WIN32
/* UI migration phase 2: the same settings, actions and polls behind the dj_ui
 * Settings page. The legacy widgets above are never built in this mode; the
 * action bodies mirror their event callbacks so the legacy build is untouched. */
static ui_djui_settings_view_t s_djui_view;
static bool s_djui_settings_visible;
static uint32_t s_djui_poll_ms;
static uint32_t s_djui_link_poll_ms;
static char s_djui_controller[CPM_ID_MAX];
static uint32_t s_djui_controller_generation;
static bool s_djui_controller_valid;
static char s_djui_link_text[80];

void ui_settings_djui_init(void)
{
    app_settings_t cfg = app_settings_get();
    ui_djui_settings_view_t *v = &s_djui_view;
    memset(v, 0, sizeof(*v));

    /* What ui_settings_create() applies at boot. */
    s_master_trim_preset = ui_settings_master_trim_sanitize_preset(cfg.master_trim_preset);
    audio_engine_set_master_trim(ui_settings_master_trim_gain(s_master_trim_preset));

    v->brightness_pct = ui_djui_bridge_brightness_clamp(cfg.backlight_pct);
    v->wireless_on = cfg.wifi_remote != 0;
    v->master_trim = ui_settings_master_trim_label(s_master_trim_preset);
    v->main_out_usb = cfg.main_out_usb != 0;
    v->ui_blackout_play = cfg.ui_blackout_play != 0;
#if defined(CONFIG_BSP_ES8311_MONITOR) && CONFIG_BSP_ES8311_MONITOR
    v->local_monitor = true;
#endif
    v->cue_mode = ui_settings_cue_mode_name(cfg.cue_mode);
    v->jog_cdj = cfg.jog_cdj_mode != 0u;
    v->tempo_range_pct = cfg.tempo_range_pct;
    v->controller_name = s_djui_controller;
    v->sd_state = UI_DJUI_SD_CHECKING;

    firmware_health_info_t info;
    if (firmware_health_get_info(&info) == ESP_OK) {
        v->fw_version = info.version;
        v->fw_partition = info.partition_label;
    }
    esp_reset_reason_t rr = esp_reset_reason();
    v->reset_reason = ui_settings_reset_reason_str();
    v->reset_bad = rr == ESP_RST_PANIC || rr == ESP_RST_BROWNOUT || rr == ESP_RST_INT_WDT ||
                   rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;

    v->link_enabled = cfg.dj_link_enable != 0;
    v->link_status = v->link_enabled ? "DJ LINK: ON" : "DJ LINK: OFF";
    v->link_tone = v->link_enabled ? DJ_TONE_NORMAL : DJ_TONE_MUTED;
#if CONFIG_AUDIO_RECORDER_ENABLED
    v->rec_state = UI_DJUI_REC_IDLE;
#else
    v->rec_state = UI_DJUI_REC_OFF;
#endif

    s_djui_settings_visible = false;
    s_djui_poll_ms = 0;
    s_djui_link_poll_ms = 0;
    s_djui_controller[0] = '\0';
    s_djui_controller_valid = false;
    ui_djui_bridge_settings_invalidate();
}

void ui_settings_djui_set_visible(bool visible)
{
    if (visible && !s_djui_settings_visible) {
        s_djui_poll_ms = 0;   /* fresh SD / log / recorder values on entry */
    }
    s_djui_settings_visible = visible;
}

static void ui_settings_djui_poll_link(ui_djui_settings_view_t *v, uint32_t now_ms)
{
    if (!ui_settings_should_poll(now_ms, s_djui_link_poll_ms, false, 500u)) {
        return;
    }
    s_djui_link_poll_ms = now_ms;

    /* Static: the peer names below point into it until the next poll. */
    static dj_link_summary_t s_summary;
    const dj_link_summary_t *summary = &s_summary;
    dj_link_get_summary(&s_summary);
    dj_link_format_status(summary, s_djui_link_text, sizeof(s_djui_link_text));
    v->link_status = s_djui_link_text;
    v->link_tone = DJ_TONE_MUTED;
    if (summary->state == DJ_LINK_STATE_WAIT_IP || summary->state == DJ_LINK_STATE_ERROR) {
        v->link_tone = DJ_TONE_WARN;
    } else if (summary->state == DJ_LINK_STATE_LISTENING) {
        v->link_tone = summary->has_master ? DJ_TONE_OK : DJ_TONE_NORMAL;
    }

    /* Players with a known IP; BPM and flags only for the master, the only
     * peer the summary carries in full. */
    uint8_t count = 0;
    for (uint8_t i = 0; i < summary->player_count && count < DJ_LINK_ROWS; i++) {
        const dj_link_player_t *p = &summary->players[i];
        const dj_link_peer_t *m = &summary->master;
        bool is_master = summary->has_master && m->device_number == p->number;
        v->link_peers[count++] = (dj_ui_link_peer_t){
            .number = p->number,
            .name = p->name,
            .bpm = is_master ? m->bpm : 0.0f,
            .master = is_master && summary->master_confirmed,
            .on_air = is_master && m->has_status && m->on_air,
            .playing = is_master && m->has_status && m->playing,
        };
    }
    v->link_peer_count = count;

    /* Top bar. Polled at 2 Hz, so the beat-in-bar would lag: left unknown. */
    v->link_master_valid = summary->state != DJ_LINK_STATE_OFF;
    v->link_master = (dj_ui_link_master_t){
        .player = summary->has_master ? summary->master.device_number : 0,
        .bpm = summary->has_master ? summary->master.bpm : 0.0f,
        .beat = 0,
        .confirmed = summary->master_confirmed,
    };
}

static void ui_settings_djui_poll_controller(ui_djui_settings_view_t *v)
{
    uint32_t generation = controller_profile_manager_active_generation();
    if (s_djui_controller_valid && generation == s_djui_controller_generation) {
        return;
    }
    char name[CPM_ID_MAX];
    if (!controller_profile_manager_get_active_short_name(name, sizeof(name))) {
        return;   /* manager busy: retry next frame */
    }
    snprintf(s_djui_controller, sizeof(s_djui_controller), "%s", name);
    v->controller_name = s_djui_controller;
    s_djui_controller_generation = generation;
    s_djui_controller_valid = true;
}

static void ui_settings_djui_poll_page(ui_djui_settings_view_t *v)
{
    bsp_sd_status_t sd;
    if (bsp_sd_get_status(&sd) == ESP_OK && sd.mounted) {
        v->sd_state = UI_DJUI_SD_MOUNTED;
        v->sd_free_bytes = sd.free_bytes;
        v->sd_total_bytes = sd.total_bytes;
    } else {
        v->sd_state = UI_DJUI_SD_OFFLINE;
    }

    service_log_status_t log;
    v->sd_log_valid = service_log_get_status(&log) == ESP_OK;
    if (v->sd_log_valid) {
        v->sd_log_available = log.available;
        v->sd_log_kb = (uint32_t)(log.current_bytes >> 10);
        v->sd_log_dropped = (uint32_t)log.dropped;
    }

#if CONFIG_AUDIO_RECORDER_ENABLED
    audio_recorder_status_t rec;
    if (audio_recorder_get_status(&rec) == ESP_OK) {
        bool active = rec.state == AUDIO_RECORDER_RECORDING || rec.state == AUDIO_RECORDER_STARTING ||
                      rec.state == AUDIO_RECORDER_STOPPING;
        v->rec_state = rec.state == AUDIO_RECORDER_ERROR ? UI_DJUI_REC_ERROR
                       : active                          ? UI_DJUI_REC_ACTIVE
                                                         : UI_DJUI_REC_IDLE;
        v->rec_secs = rec.sample_rate > 0u ? (uint32_t)(rec.frames_written / rec.sample_rate) : 0u;
        v->rec_mb = (uint32_t)(rec.bytes_written >> 20);
    }
#endif
}

void ui_settings_djui_update(const ui_frame_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    ui_djui_settings_view_t *v = &s_djui_view;
    v->controller_connected = ctx->deck_state[CTRL_DECK_1].controller_connected;
    bool playing = false;
    for (uint8_t deck = 0; deck < DECK_CORE_DECK_COUNT; deck++) {
        playing = playing || ctx->deck_state[deck].playing;
    }
    /* Best effort: ui_lvgl_backend stops rendering once it sees the deck
     * play, so this frame only shows when it was drawn first. */
    v->blackout_now = v->ui_blackout_play && playing;

    v->tempo_range_pct = tempo_range_sync();
    ui_settings_djui_poll_link(v, ctx->now_ms);
    ui_settings_djui_poll_controller(v);
    if (s_djui_settings_visible &&
        ui_settings_should_poll(ctx->now_ms, s_djui_poll_ms, false, 1000u)) {
        s_djui_poll_ms = ctx->now_ms;
        ui_settings_djui_poll_page(v);
    }
    ui_djui_bridge_settings_update(v);
}

void ui_settings_djui_on_brightness(uint8_t pct)
{
    uint8_t val = ui_djui_bridge_brightness_clamp(pct);
    s_djui_view.brightness_pct = val;
    bsp_display_set_backlight(val);
    app_settings_set_backlight(val);
    ESP_LOGI(TAG, "Backlight brightness set to %u%%", (unsigned)val);
}

void ui_settings_djui_on_wireless(bool on)
{
    s_djui_view.wireless_on = on;
    app_settings_set_wifi_remote(on ? 1 : 0);
    if (s_wifi_toggle_cb) {
        s_wifi_toggle_cb(on);
    }
    ESP_LOGI(TAG, "Wi-Fi remote: %s", on ? "on" : "off");
}

void ui_settings_djui_on_link(bool on)
{
    ui_djui_settings_view_t *v = &s_djui_view;
    app_settings_set_dj_link_enable(on ? 1 : 0);
    if (s_dj_link_toggle_cb) {
        s_dj_link_toggle_cb(on);
    }
    /* Show the switch position now; the next poll brings the observer state. */
    v->link_enabled = on;
    v->link_status = on ? "DJ LINK: ON" : "DJ LINK: OFF";
    v->link_tone = on ? DJ_TONE_NORMAL : DJ_TONE_MUTED;
    s_djui_link_poll_ms = 0;
    ESP_LOGI(TAG, "DJ Link: %s", on ? "on" : "off");
}

void ui_settings_djui_on_record(void)
{
#if CONFIG_AUDIO_RECORDER_ENABLED
    if (!s_recording_toggle_cb) {
        return;
    }
    audio_recorder_state_t st = audio_recorder_get_state();
    bool active = (st == AUDIO_RECORDER_RECORDING || st == AUDIO_RECORDER_STARTING);
    bool ok = s_recording_toggle_cb(!active);
    ESP_LOGI(TAG, "recording toggle -> %s (%s)", active ? "stop" : "start", ok ? "ok" : "failed");
    ui_settings_djui_poll_page(&s_djui_view);
#endif
}

void ui_settings_djui_on_field(dj_field_t field)
{
    ui_djui_settings_view_t *v = &s_djui_view;
    switch (field) {
    case DJ_F_MASTER: {
        s_master_trim_preset = ui_settings_master_trim_next_preset(s_master_trim_preset);
        float gain = ui_settings_master_trim_gain(s_master_trim_preset);
        audio_engine_set_master_trim(gain);
        app_settings_set_master_trim_preset(s_master_trim_preset);
        v->master_trim = ui_settings_master_trim_label(s_master_trim_preset);
        ESP_LOGI(TAG, "Master trim set: %s (gain %.3f)", v->master_trim, (double)gain);
        break;
    }
    case DJ_F_OUT_MAIN: {
        uint8_t next = app_settings_get().main_out_usb ? 0u : 1u;
        app_settings_set_main_out_usb(next);
        audio_engine_main_sink_refresh();
        v->main_out_usb = next != 0u;
        ESP_LOGI(TAG, "MAIN output: %s", next ? "USB UAC (DDJ)" : "PCM5102A I2S");
        break;
    }
    case DJ_F_UI_RENDER: {
        uint8_t next = app_settings_get().ui_blackout_play ? 0u : 1u;
        app_settings_set_ui_blackout_play(next);
        v->ui_blackout_play = next != 0u;
        ESP_LOGW(TAG, "TEST UI blackout on PLAY: %s", next ? "ON" : "OFF");
        break;
    }
    case DJ_F_MIX_CUE: {
        uint8_t next = (uint8_t)((app_settings_get().cue_mode + 1u) % 2u);
        app_settings_set_cue_mode(next);
        audio_engine_set_cue_mode(next);
        v->cue_mode = ui_settings_cue_mode_name(next);
        ESP_LOGI(TAG, "Cue mode saved: %s", v->cue_mode);
        break;
    }
    case DJ_F_MIX_JOG:
        v->jog_cdj = jog_mode_toggle() != 0u;
        break;
    case DJ_F_MIX_TEMPO:
        v->tempo_range_pct = tempo_range_cycle();
        break;
    default:
        break;   /* DJ_F_LOCAL: build-time route, nothing to toggle */
    }
}
#endif  /* !WIN32 */

#endif
