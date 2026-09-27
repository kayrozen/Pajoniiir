#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ui_overlay_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t msync_us;
    uint32_t ppa_us;
    uint32_t total_us;
} ui_lvgl_backend_blit_perf_t;

typedef void (*ui_lvgl_backend_frame_cb_t)(void *user_ctx);

esp_err_t ui_lvgl_backend_init(uint16_t hor_res, uint16_t ver_res);
// Runs callback from the LVGL task on each delivered panel-refresh event.
// Register it before ui_lvgl_backend_start(). Refreshes that arrive while the
// task is busy are coalesced so stale UI work cannot build up.
esp_err_t ui_lvgl_backend_set_frame_callback(ui_lvgl_backend_frame_cb_t callback,
                                             void *user_ctx);
// Appends UI-level lines ('\n'-separated, NUL-terminated within size) to the
// periodic perf report. Runs in the LVGL task under the LVGL lock and must not
// do I/O: the backend's low-priority ui_perf task prints the text later.
// Register it before ui_lvgl_backend_start().
typedef void (*ui_lvgl_backend_perf_report_cb_t)(char *buf, size_t size);
esp_err_t ui_lvgl_backend_set_perf_report_callback(ui_lvgl_backend_perf_report_cb_t callback);

/* v289: extends the JC1060 LVGL stall report (UI diagnostics on). Called from
 * the lvgl_tick esp_timer callback: begin=true (buf NULL) when a stall window
 * opens, begin=false to append text at buf (NUL-terminated within size) when
 * it is reported; returns the characters written, below size. Loads and
 * snprintf only: no lock, no allocation, no I/O. Any time, NULL clears. */
typedef size_t (*ui_lvgl_backend_stall_probe_cb_t)(bool begin, char *buf, size_t size);
void ui_lvgl_backend_set_stall_probe_callback(ui_lvgl_backend_stall_probe_cb_t callback);

/* v287: rectangles a caller paints straight into the framebuffer, outside
 * LVGL (logical coords, NULL clears a slot). The flush callback records which
 * of them an LVGL refresh drew over; after every lv_timer_handler() the
 * post-refresh callback gets that mask (bit = slot, then cleared) so the
 * caller can repaint. LVGL task, under the LVGL lock, no I/O. Register the
 * callback before ui_lvgl_backend_start(). */
#define UI_LVGL_BACKEND_DIRECT_SLOTS 2u
typedef void (*ui_lvgl_backend_post_refresh_cb_t)(uint32_t repainted_mask, void *user_ctx);
esp_err_t ui_lvgl_backend_set_post_refresh_callback(ui_lvgl_backend_post_refresh_cb_t callback,
                                                    void *user_ctx);
void ui_lvgl_backend_set_direct_rect(uint8_t slot, const ui_overlay_rect_t *logical);

esp_err_t ui_lvgl_backend_start(void);
void ui_lvgl_lock(void);
void ui_lvgl_unlock(void);

void *ui_lvgl_backend_alloc_dma_buffer(size_t bytes, size_t *aligned_bytes);

esp_err_t ui_lvgl_backend_blit_rgb565_ppa270(const ui_overlay_rect_t *logical,
                                             const uint16_t *src,
                                             uint32_t src_w,
                                             uint32_t src_h,
                                             size_t src_bytes,
                                             ui_lvgl_backend_blit_perf_t *perf);

esp_err_t ui_lvgl_backend_blit_rgb565_ppa270_region(const ui_overlay_rect_t *logical,
                                                    const uint16_t *src,
                                                    uint32_t src_w,
                                                    uint32_t src_h,
                                                    uint32_t src_x,
                                                    uint32_t src_y,
                                                    uint32_t block_w,
                                                    uint32_t block_h,
                                                    size_t src_bytes,
                                                    ui_lvgl_backend_blit_perf_t *perf);

esp_err_t ui_lvgl_backend_draw_rect_rgb565(const ui_overlay_rect_t *logical, uint16_t color);

#ifdef __cplusplus
}
#endif
