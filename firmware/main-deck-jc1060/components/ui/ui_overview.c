#include "ui_overview.h"

#include "esp_log.h"
#include "ui_overview_window.h"

/* v293 (UI migration phase 6): the legacy Overview widgets are gone. What is
 * left is the controller zoom step, which the dj_ui bridge reads through
 * ui_overview_zoom_window_ms() for both zoom strips. */

static const char *TAG = "ui_overview";

static uint8_t s_overview_zoom_step = 2u;

uint32_t ui_overview_zoom_window_ms(uint16_t bpm_x100, uint16_t deck_bpm)
{
    return ui_overview_window_ms_from_bpm_x100_for_zoom(bpm_x100, deck_bpm, s_overview_zoom_step);
}

esp_err_t ui_overview_zoom_delta(int delta)
{
    if (delta == 0) {
        return ESP_OK;
    }

    uint8_t next = ui_overview_zoom_apply_delta(s_overview_zoom_step, delta);
    if (next == s_overview_zoom_step) {
        return ESP_OK;
    }

    s_overview_zoom_step = next;
    ESP_LOGI(TAG,
             "overview waveform zoom: %u beats",
             (unsigned)ui_overview_zoom_visible_beats_for_step(s_overview_zoom_step));
    return ESP_OK;
}
