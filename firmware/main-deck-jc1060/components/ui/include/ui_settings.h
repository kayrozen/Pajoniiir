#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ui_settings_should_poll(uint32_t now_ms,
                             uint32_t last_poll_ms,
                             bool force,
                             uint32_t interval_ms);
uint8_t ui_settings_master_trim_preset_count(void);
uint8_t ui_settings_master_trim_sanitize_preset(uint8_t preset);
uint8_t ui_settings_master_trim_next_preset(uint8_t current);
float ui_settings_master_trim_gain(uint8_t preset);
const char *ui_settings_master_trim_label(uint8_t preset);

#ifndef UI_SETTINGS_HOST_TEST

#include "lvgl.h"
#include "ui_frame_context.h"

// Action invoked when the user flips the Wi-Fi remote switch. Registered by
// app_main so the UI stays decoupled from the wifi_link/web_server transport
// (avoids a ui -> wifi_link -> web_server -> ui component dependency cycle).
typedef void (*ui_settings_wifi_toggle_cb_t)(bool enable);
void ui_settings_set_wifi_toggle_cb(ui_settings_wifi_toggle_cb_t cb);

/* Master-output recorder toggle. The callback starts (enable=true) or stops
 * (enable=false) recording and returns true on success. */
typedef bool (*ui_settings_recording_toggle_cb_t)(bool enable);
void ui_settings_set_recording_toggle_cb(ui_settings_recording_toggle_cb_t cb);

/* v246: Pioneer DJ Link observer switch (Ethernet only). The UI persists the
 * setting; the callback, registered by app_main, starts/stops dj_link. */
typedef void (*ui_settings_dj_link_toggle_cb_t)(bool enable);
void ui_settings_set_dj_link_toggle_cb(ui_settings_dj_link_toggle_cb_t cb);

#ifndef WIN32
/* UI migration phase 2: drives the dj_ui Settings page through
 * ui_djui_bridge. LVGL task only. */
#include "dj_ui.h"

void ui_settings_djui_init(void);
void ui_settings_djui_set_visible(bool visible);
void ui_settings_djui_update(const ui_frame_context_t *ctx);
void ui_settings_djui_on_brightness(uint8_t pct);
void ui_settings_djui_on_wireless(bool on);
void ui_settings_djui_on_link(bool on);
void ui_settings_djui_on_record(void);
void ui_settings_djui_on_field(dj_field_t field);
#endif

#endif
