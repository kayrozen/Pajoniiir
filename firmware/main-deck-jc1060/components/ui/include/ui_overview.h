#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Controller zoom step of the Overview waveforms (LVGL task). */
esp_err_t ui_overview_zoom_delta(int delta);
/* Main-waveform window for the current controller zoom step; the dj_ui bridge
 * sizes both zoom strips with it. */
uint32_t ui_overview_zoom_window_ms(uint16_t bpm_x100, uint16_t deck_bpm);

#ifdef __cplusplus
}
#endif
