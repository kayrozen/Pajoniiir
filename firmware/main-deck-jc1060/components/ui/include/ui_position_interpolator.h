#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* v231: how far the display may run ahead of the last engine position
 * (one LVGL refresh at CONFIG_LV_DEF_REFR_PERIOD=33). The engine position is
 * read live every frame, so this only bridges output-block granularity and
 * short commit delays. */
#define UI_POSITION_INTERPOLATOR_MAX_LEAD_MS 33u

typedef struct {
    bool initialized;
    bool last_playing;
    /* Last engine position and when the UI first saw it. */
    uint32_t anchor_position_ms;
    uint64_t anchor_time_us;
    /* Displayed position in microseconds; never decreases while playing
     * unless the engine position itself goes backwards (seek/cue/loop). */
    uint64_t display_us;
} ui_position_interpolator_t;

void ui_position_interpolator_init(ui_position_interpolator_t *interp);

uint32_t ui_position_interpolator_update(ui_position_interpolator_t *interp,
                                         uint32_t snapshot_position_ms,
                                         uint32_t duration_ms,
                                         bool playing,
                                         /* Zero disables prediction and makes
                                          * each snapshot authoritative. */
                                         uint32_t speed_permille,
                                         uint64_t now_us);

#ifdef __cplusplus
}
#endif
