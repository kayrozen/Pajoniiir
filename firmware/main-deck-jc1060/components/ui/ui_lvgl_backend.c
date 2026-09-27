#include "ui_lvgl_backend.h"
#include "ui.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui_diagnostics.h"

static const char *TAG = "ui";

static esp_err_t ui_lvgl_backend_validate_rgb565_region_args(const ui_overlay_rect_t *logical,
                                                             const uint16_t *src,
                                                             uint32_t src_w,
                                                             uint32_t src_h,
                                                             uint32_t src_x,
                                                             uint32_t src_y,
                                                             uint32_t block_w,
                                                             uint32_t block_h,
                                                             size_t src_bytes)
{
    if (!logical || !src || logical->w <= 0 || logical->h <= 0 ||
        src_w == 0 || src_h == 0 || block_w == 0 || block_h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (logical->x > INT_MAX - logical->w ||
        logical->y > INT_MAX - logical->h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (logical->x < 0 || logical->y < 0 ||
        block_w > INT_MAX || block_h > INT_MAX ||
        logical->w != (int)block_w || logical->h != (int)block_h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_x >= src_w || src_y >= src_h ||
        block_w > src_w - src_x || block_h > src_h - src_y) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_w > ((size_t)-1) / sizeof(uint16_t) ||
        src_h > (((size_t)-1) / sizeof(uint16_t)) / src_w) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required_bytes = (size_t)src_w * (size_t)src_h * sizeof(uint16_t);
    if (src_bytes < required_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

#ifdef WIN32
esp_err_t ui_lvgl_backend_init(uint16_t hor_res, uint16_t ver_res)
{
    (void)hor_res;
    (void)ver_res;
    return ESP_OK;
}

esp_err_t ui_lvgl_backend_set_frame_callback(ui_lvgl_backend_frame_cb_t callback,
                                             void *user_ctx)
{
    (void)callback;
    (void)user_ctx;
    return ESP_OK;
}

esp_err_t ui_lvgl_backend_set_perf_report_callback(ui_lvgl_backend_perf_report_cb_t callback)
{
    (void)callback;
    return ESP_OK;
}

esp_err_t ui_lvgl_backend_set_post_refresh_callback(ui_lvgl_backend_post_refresh_cb_t callback,
                                                    void *user_ctx)
{
    (void)callback;
    (void)user_ctx;
    return ESP_OK;
}

void ui_lvgl_backend_set_stall_probe_callback(ui_lvgl_backend_stall_probe_cb_t callback)
{
    (void)callback;
}

void ui_lvgl_backend_set_direct_rect(uint8_t slot, const ui_overlay_rect_t *logical)
{
    (void)slot;
    (void)logical;
}

esp_err_t ui_lvgl_backend_start(void)
{
    return ESP_OK;
}

void ui_lvgl_lock(void) {}
void ui_lvgl_unlock(void) {}

void *ui_lvgl_backend_alloc_dma_buffer(size_t bytes, size_t *aligned_bytes)
{
    if (aligned_bytes) {
        *aligned_bytes = bytes;
    }
    return malloc(bytes);
}

esp_err_t ui_lvgl_backend_blit_rgb565_ppa270(const ui_overlay_rect_t *logical,
                                             const uint16_t *src,
                                             uint32_t src_w,
                                             uint32_t src_h,
                                             size_t src_bytes,
                                             ui_lvgl_backend_blit_perf_t *perf)
{
    return ui_lvgl_backend_blit_rgb565_ppa270_region(logical,
                                                     src,
                                                     src_w,
                                                     src_h,
                                                     0,
                                                     0,
                                                     src_w,
                                                     src_h,
                                                     src_bytes,
                                                     perf);
}

esp_err_t ui_lvgl_backend_blit_rgb565_ppa270_region(const ui_overlay_rect_t *logical,
                                                    const uint16_t *src,
                                                    uint32_t src_w,
                                                    uint32_t src_h,
                                                    uint32_t src_x,
                                                    uint32_t src_y,
                                                    uint32_t block_w,
                                                    uint32_t block_h,
                                                    size_t src_bytes,
                                                    ui_lvgl_backend_blit_perf_t *perf)
{
    esp_err_t err = ui_lvgl_backend_validate_rgb565_region_args(logical,
                                                                src,
                                                                src_w,
                                                                src_h,
                                                                src_x,
                                                                src_y,
                                                                block_w,
                                                                block_h,
                                                                src_bytes);
    if (err != ESP_OK) {
        return err;
    }
    if (perf) {
        memset(perf, 0, sizeof(*perf));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ui_lvgl_backend_draw_rect_rgb565(const ui_overlay_rect_t *logical, uint16_t color)
{
    if (!logical || logical->w <= 0 || logical->h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
#else
#include "app_settings.h"
#include "audio_engine.h"
#include "bsp_jc4880.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_private/esp_cache_private.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include <stdio.h>
#include <sys/lock.h>

/* v292: EXT_RAM_BSS_ATTR moves a static to PSRAM when
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set; empty on host builds. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define LVGL_TICK_PERIOD_MS        2
#define LVGL_TASK_STACK            (24 * 1024)
#define LVGL_TASK_PRIO             4
/* Driven by the BSP so the two cannot disagree. There is no inactive-buffer
 * swap in this backend: LVGL renders partial rows into one PSRAM draw buffer and
 * the flush callback PPA-rotates them straight into the single DPI framebuffer.
 * Asking the driver for more would reserve full-screen buffers that are never
 * scanned. */
#define UI_DSI_FB_COUNT            BSP_LCD_FRAMEBUFFER_COUNT
_Static_assert(UI_DSI_FB_COUNT == 1u,
               "this backend supports exactly one DPI framebuffer");
/* v278: 128 rows (was 80, upstream value). LVGL splits an area into
 * buf/stride rows: a DJUI zoom surface is 696x119, which 80 rows (117 rows at
 * that width) cut into two flushes every frame - two PPA writes at different
 * scan positions, i.e. a guaranteed tear seam. 128 rows keep any area up to
 * 188 rows at zoom width in one flush. +96 KiB PSRAM, allocated once at init. */
#define UI_LVGL_PARTIAL_BUF_ROWS   128u
#define UI_LVGL_NOTIFY_REFRESH     (1u << 0)
#define ALIGN_UP_BY(n, a)          (((n) + ((a) - 1)) & ~((a) - 1))

#ifdef UI_TARGET_JC1060
/* v278 anti-tear scan gate. The single DPI framebuffer is scanned while LVGL
 * flushes into it, so a flush that overlaps the scanline shows half old, half
 * new pixels - most visible on the scrolling zoom waveform. The P4 DSI bridge
 * raises on_refresh_done on its VSYNC event; from that timestamp and the
 * measured frame period the flush estimates the scanned row and delays a large
 * write until the scan is clear of it. Only the LVGL task waits (the UI loses);
 * nothing here touches audio priority or pacing.
 * JC1060-only divergence from upstream: rotation 0 makes framebuffer rows the
 * scan rows. Timing mirrors bsp_board_config.h (not includable here: it
 * redefines BSP_LCD_H_RES/V_RES with different spelling). */
#define UI_SCAN_V_LEAD_LINES       33    /* V_SYNC 10 + V_BACK_PORCH 23 */
#define UI_SCAN_V_TOTAL_LINES      645   /* 600 + 10 + 23 + 12 (front porch) */
#define UI_SCAN_GUARD_LINES        24    /* bridge FIFO prefetch + timing error */
#define UI_SCAN_GATE_MIN_PX        16384u /* small label flushes are not worth a wait */
#define UI_SCAN_MAX_WAIT_US        8000u
/* v281: total gate wait per lv_timer_handler() call. Two zoom surfaces are
 * rendered top-down about as fast as the panel scans, so each of their flushes
 * can meet the scanline; per-flush waits of up to 8 ms then stack and push the
 * refresh past the 33 ms LVGL period (one skipped panel refresh = a visible
 * waveform hitch). Past the budget a flush tears rather than wait. */
#define UI_SCAN_REFR_BUDGET_US     5000u
#define UI_SCAN_FRAME_US_DEFAULT   16530u /* 54 MHz / (1384 x 645) */
#define UI_SCAN_COPY_NS_PX_DEFAULT 16u
#endif

static _lock_t s_lvgl_lock;
static lv_display_t *s_disp = NULL;
static ppa_client_handle_t s_ppa = NULL;

/* JC1060 panel is native landscape: no PPA rotation. The upstream JC4880
 * panel is portrait and needs a 270-degree hardware rotation. */
#ifdef UI_TARGET_JC1060
#define UI_PPA_ROTATION_ANGLE PPA_SRM_ROTATION_ANGLE_0
#else
#define UI_PPA_ROTATION_ANGLE PPA_SRM_ROTATION_ANGLE_270
#endif

static void *s_dsi_fb[UI_DSI_FB_COUNT] = { NULL };
static int s_dsi_active_fb_idx = 0;
static size_t s_cache_align = 64;
static uint16_t s_hor_res = 800;
static uint16_t s_ver_res = 480;
static TaskHandle_t s_lvgl_task_handle = NULL;
static ui_lvgl_backend_frame_cb_t s_frame_callback = NULL;
static void *s_frame_callback_ctx = NULL;
/* v287: direct-painted rectangles and the slots LVGL flushed over since the
 * last post-refresh callback. LVGL task only. */
static ui_lvgl_backend_post_refresh_cb_t s_post_refresh_callback = NULL;
static void *s_post_refresh_callback_ctx = NULL;
static ui_overlay_rect_t s_direct_rect[UI_LVGL_BACKEND_DIRECT_SLOTS];
static uint32_t s_direct_repainted;

/* v283: diagnostics never print from the LVGL task. v280-v282 logged "spike
 * window max" lines to the 115200-baud UART from inside flush_cb and the
 * REFR/RENDER events, i.e. inside the very phases being timed: a line costs
 * several ms of blocking console I/O, so each spiking window planted a spike
 * in the next one and the report sustained itself. Phases now only update
 * these windows; every UI_PERF_REPORT_MS the LVGL task formats one text
 * snapshot (no I/O) and the priority-1 ui_perf task prints it. */
typedef struct {
    uint32_t n;
    uint32_t max;
    uint64_t sum;
} ui_perf_win_t;

/* One refresh, REFR_START to REFR_READY. */
typedef struct {
    uint32_t refr_us;
    uint32_t flush_us;       /* msync + PPA, all flushes */
    uint32_t gate_us;        /* scan-gate waits, all flushes */
    uint32_t flushes;
    uint32_t flush_px;
    uint32_t inval_count;
    uint32_t inval_px;
    lv_area_t inval_max;
} ui_perf_refr_t;

static ui_perf_win_t s_win_handler_interval;
static ui_perf_win_t s_win_handler_duration;
static ui_perf_win_t s_win_frame_cb;
static ui_perf_win_t s_win_refr_interval;
static ui_perf_win_t s_win_refr_total;
static ui_perf_win_t s_win_refr_draw;
static ui_perf_win_t s_win_refr_flush;
static ui_perf_win_t s_win_render_total;
static ui_perf_win_t s_win_flush_ppa;
static ui_perf_win_t s_win_flush_msync;
static ui_perf_refr_t s_refr_cur;
static ui_perf_refr_t s_refr_worst;
static uint32_t s_refr_flush_px_max;
static uint32_t s_refr_flushes_max;

static int64_t s_lvgl_refr_start_us = 0;
static int64_t s_lvgl_render_start_us = 0;
static int64_t s_lvgl_last_refr_start_us = 0;
static uint32_t s_lvgl_inval_count = 0;
static uint32_t s_lvgl_inval_total_px = 0;
static uint32_t s_lvgl_inval_max_px = 0;
static lv_area_t s_lvgl_inval_max_area = {0};

/* LVGL task -> ui_perf task: one preformatted snapshot, handed over with a
 * busy flag like the audio engine's heartbeat report. */
#define UI_PERF_REPORT_MS          5000u
#define UI_PERF_TEXT_MAX           2048u
#define UI_PERF_TASK_STACK         4096
#define UI_PERF_TASK_PRIO          1
static const char *PERF_TAG = "ui_perf";
static TaskHandle_t s_perf_task = NULL;
static char *s_perf_text = NULL;
static bool s_perf_busy = false;
static int64_t s_perf_window_start_us = 0;
static ui_lvgl_backend_perf_report_cb_t s_perf_report_cb = NULL;

#ifdef UI_TARGET_JC1060
/* Low 32 bits of esp_timer: single-word ISR->task handoff, differences only. */
static volatile uint32_t s_scan_vsync_us = 0;
static volatile uint32_t s_scan_frame_us = UI_SCAN_FRAME_US_DEFAULT;
static uint32_t s_scan_copy_ns_px = UI_SCAN_COPY_NS_PX_DEFAULT;
/* LVGL task: gate time spent in the current lv_timer_handler() call, and the
 * gated flushes that gave up (tore) since the last report. */
static uint32_t s_scan_refr_wait_us = 0;
static ui_perf_win_t s_win_refr_gate;
static uint32_t s_scan_giveups = 0;

/* v288 stall probe. The v287 UI freezes while a MIDI fader moves, audio keeps
 * running, and nothing on the fader path blocks the LVGL task by reading. The
 * LVGL task only stamps a heartbeat and its current phase; the lvgl_tick
 * esp_timer callback (esp_timer task, core 0) samples every 10 ms which task
 * each core runs and the LVGL task state while the heartbeat is stale. On
 * recovery it formats one report for the ui_perf task: Ready + another task
 * owning core 1 is starvation, Blocked names the phase that waits. Plain
 * loads and eTaskGetState() only: no allocation, no I/O, no priority change. */
typedef enum {
    UI_STALL_PH_WAIT = 0,
    UI_STALL_PH_LOCK,
    UI_STALL_PH_FRAME_CB,
    UI_STALL_PH_HANDLER,
    UI_STALL_PH_FLUSH,
    UI_STALL_PH_TOUCH,
    UI_STALL_PH_POST,
    UI_STALL_PH_COUNT,
} ui_stall_phase_t;
static const char *const UI_STALL_PHASE_NAME[UI_STALL_PH_COUNT] = {
    "wait", "lock", "frame_cb", "handler", "flush", "touch", "post",
};
#define UI_STALL_THRESHOLD_US  250000u
#define UI_STALL_SAMPLE_TICKS  5u      /* x LVGL_TICK_PERIOD_MS = 10 ms */
#define UI_STALL_REPORT_SAMPLES 500u   /* an ongoing stall reports every 5 s */
#define UI_STALL_SLOTS         6u
#define UI_STALL_TEXT_MAX      1024u
typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    uint16_t n;
} ui_stall_slot_t;
typedef struct {
    bool active;
    uint32_t start_us;
    uint8_t phase;
    uint16_t samples;
    uint16_t lvgl_ready;
    uint16_t lvgl_blocked;
    uint16_t phase_n[UI_STALL_PH_COUNT];
    ui_stall_slot_t core[2][UI_STALL_SLOTS];
    uint16_t core_other[2];
} ui_stall_window_t;
/* LVGL task -> esp_timer task. */
static volatile uint32_t s_stall_beat_us = 0;
static volatile uint8_t s_stall_phase = UI_STALL_PH_WAIT;
/* esp_timer task only, apart from the text handed to ui_perf. */
EXT_RAM_BSS_ATTR static ui_stall_window_t s_stall;
static uint32_t s_stall_tick = 0;
EXT_RAM_BSS_ATTR static char s_stall_text[UI_STALL_TEXT_MAX];
static bool s_stall_text_ready = false;
static ui_lvgl_backend_stall_probe_cb_t s_stall_probe_cb = NULL;

static inline void ui_stall_mark(ui_stall_phase_t phase)
{
    if (!ui_diagnostics_enabled()) {
        return;   /* v293: the probe only runs in diagnostics builds */
    }
    s_stall_phase = (uint8_t)phase;
    s_stall_beat_us = (uint32_t)esp_timer_get_time();
}
#endif

static bool IRAM_ATTR ui_lvgl_dpi_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                                  esp_lcd_dpi_panel_event_data_t *edata,
                                                  void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

#ifdef UI_TARGET_JC1060
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint32_t prev_us = s_scan_vsync_us;
    if (prev_us != 0) {
        uint32_t period_us = now_us - prev_us;
        if (period_us > 10000u && period_us < 40000u) {
            s_scan_frame_us = period_us;
        }
    }
    s_scan_vsync_us = now_us;
#endif

    TaskHandle_t task = s_lvgl_task_handle;
    if (task == NULL) {
        return false;
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    xTaskNotifyFromISR(task,
                       UI_LVGL_NOTIFY_REFRESH,
                       eSetBits,
                       &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static uint32_t ui_lvgl_backend_perf_elapsed_us(int64_t start_us)
{
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    return elapsed_us > 0 ? (uint32_t)elapsed_us : 0u;
}

static void ui_perf_win_add(ui_perf_win_t *win, uint32_t us)
{
    win->n++;
    win->sum += us;
    if (us > win->max) {
        win->max = us;
    }
}

static uint32_t ui_perf_win_avg(const ui_perf_win_t *win)
{
    return win->n ? (uint32_t)(win->sum / win->n) : 0u;
}

static void ui_lvgl_display_event_cb(lv_event_t *e)
{
    if (!ui_diagnostics_enabled()) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
    case LV_EVENT_INVALIDATE_AREA: {
        lv_area_t *area = lv_event_get_invalidated_area(e);
        if (!area) {
            break;
        }
        int32_t w = area->x2 - area->x1 + 1;
        int32_t h = area->y2 - area->y1 + 1;
        if (w <= 0 || h <= 0) {
            break;
        }
        uint32_t px = (uint32_t)w * (uint32_t)h;
        s_lvgl_inval_count++;
        s_lvgl_inval_total_px += px;
        if (px > s_lvgl_inval_max_px) {
            s_lvgl_inval_max_px = px;
            s_lvgl_inval_max_area = *area;
        }
        break;
    }
    case LV_EVENT_REFR_START:
        s_lvgl_refr_start_us = esp_timer_get_time();
        /* Start-to-start: the refresh rate actually reaching the panel. */
        if (s_lvgl_last_refr_start_us != 0) {
            ui_perf_win_add(&s_win_refr_interval,
                            (uint32_t)(s_lvgl_refr_start_us - s_lvgl_last_refr_start_us));
        }
        s_lvgl_last_refr_start_us = s_lvgl_refr_start_us;
        s_refr_cur = (ui_perf_refr_t){
            .inval_count = s_lvgl_inval_count,
            .inval_px = s_lvgl_inval_total_px,
            .inval_max = s_lvgl_inval_max_area,
        };
        s_lvgl_inval_count = 0;
        s_lvgl_inval_total_px = 0;
        s_lvgl_inval_max_px = 0;
        s_lvgl_inval_max_area = (lv_area_t){0};
        break;
    case LV_EVENT_REFR_READY:
        if (s_lvgl_refr_start_us != 0) {
            s_refr_cur.refr_us = ui_lvgl_backend_perf_elapsed_us(s_lvgl_refr_start_us);
            uint32_t copy_us = s_refr_cur.flush_us + s_refr_cur.gate_us;
            ui_perf_win_add(&s_win_refr_total, s_refr_cur.refr_us);
            ui_perf_win_add(&s_win_refr_flush, copy_us);
            /* Everything that is not the copy out: LVGL drawing, area joins
             * and any time the task was preempted or blocked. */
            ui_perf_win_add(&s_win_refr_draw,
                            s_refr_cur.refr_us > copy_us ? s_refr_cur.refr_us - copy_us : 0u);
            if (s_refr_cur.flush_px > s_refr_flush_px_max) s_refr_flush_px_max = s_refr_cur.flush_px;
            if (s_refr_cur.flushes > s_refr_flushes_max) s_refr_flushes_max = s_refr_cur.flushes;
            if (s_refr_cur.refr_us >= s_refr_worst.refr_us) {
                s_refr_worst = s_refr_cur;
            }
        }
#ifdef UI_TARGET_JC1060
        ui_perf_win_add(&s_win_refr_gate, s_scan_refr_wait_us);
#endif
        break;
    case LV_EVENT_RENDER_START:
        s_lvgl_render_start_us = esp_timer_get_time();
        break;
    case LV_EVENT_RENDER_READY:
        if (s_lvgl_render_start_us != 0) {
            ui_perf_win_add(&s_win_render_total,
                            ui_lvgl_backend_perf_elapsed_us(s_lvgl_render_start_us));
        }
        break;
    default:
        break;
    }
}

static esp_err_t ui_lvgl_backend_blit_rgb565_ppa270_mapped(const ui_overlay_rect_t *logical,
                                                           const ui_overlay_rect_t *physical,
                                                           const uint16_t *src,
                                                           uint32_t src_w,
                                                           uint32_t src_h,
                                                           uint32_t src_x,
                                                           uint32_t src_y,
                                                           uint32_t block_w,
                                                           uint32_t block_h,
                                                           size_t src_bytes,
                                                           ui_lvgl_backend_blit_perf_t *perf)
{
    if (perf) {
        memset(perf, 0, sizeof(*perf));
    }
    esp_err_t arg_err = ui_lvgl_backend_validate_rgb565_region_args(logical,
                                                                    src,
                                                                    src_w,
                                                                    src_h,
                                                                    src_x,
                                                                    src_y,
                                                                    block_w,
                                                                    block_h,
                                                                    src_bytes);
    if (arg_err != ESP_OK || !physical || physical->w <= 0 || physical->h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ppa || s_dsi_active_fb_idx < 0 || s_dsi_active_fb_idx >= UI_DSI_FB_COUNT ||
        !s_dsi_fb[s_dsi_active_fb_idx]) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t total_start_us = esp_timer_get_time();
    int64_t msync_start_us = esp_timer_get_time();
    esp_cache_msync((void *)src,
                    src_bytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    uint32_t msync_us = ui_lvgl_backend_perf_elapsed_us(msync_start_us);

    ppa_srm_oper_config_t op = {
        .in.buffer          = (void *)src,
        .in.pic_w           = src_w,
        .in.pic_h           = src_h,
        .in.block_w         = block_w,
        .in.block_h         = block_h,
        .in.block_offset_x  = src_x,
        .in.block_offset_y  = src_y,
        .in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565,

        .out.buffer         = s_dsi_fb[s_dsi_active_fb_idx],
        .out.buffer_size    = ALIGN_UP_BY((size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2,
                                          s_cache_align),
        .out.pic_w          = BSP_LCD_H_RES,
        .out.pic_h          = BSP_LCD_V_RES,
        .out.block_offset_x = (uint32_t)physical->x,
        .out.block_offset_y = (uint32_t)physical->y,
        .out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565,

        .rotation_angle     = UI_PPA_ROTATION_ANGLE,
        .scale_x            = 1.0,
        .scale_y            = 1.0,
        .rgb_swap           = 0,
        .byte_swap          = 0,
        .mode               = PPA_TRANS_MODE_BLOCKING,
    };

    int64_t ppa_start_us = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    uint32_t ppa_us = ui_lvgl_backend_perf_elapsed_us(ppa_start_us);
    uint32_t total_us = ui_lvgl_backend_perf_elapsed_us(total_start_us);

    if (perf) {
        perf->msync_us = msync_us;
        perf->ppa_us = ppa_us;
        perf->total_us = total_us;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "RGB565 PPA270 blit failed: %s logical=(%d,%d %dx%d) physical=(%d,%d %dx%d)",
                 esp_err_to_name(err),
                 logical->x, logical->y, logical->w, logical->h,
                 physical->x, physical->y, physical->w, physical->h);
    }
    return err;
}

#ifdef UI_TARGET_JC1060
/* Framebuffer row the DPI scan is reading now; negative inside the vertical
 * blank before row 0. false when VSYNC has not been seen recently. */
static bool ui_scan_line_now(int32_t *line, uint32_t *line_ns)
{
    uint32_t vsync_us = s_scan_vsync_us;
    uint32_t frame_us = s_scan_frame_us;
    if (vsync_us == 0 || frame_us == 0) {
        return false;
    }
    uint32_t elapsed_us = (uint32_t)esp_timer_get_time() - vsync_us;
    if (elapsed_us > 4u * frame_us) {
        return false;
    }
    uint32_t ns = (uint32_t)(((uint64_t)frame_us * 1000u) / UI_SCAN_V_TOTAL_LINES);
    uint32_t phase_us = elapsed_us % frame_us;
    *line = (int32_t)(((uint64_t)phase_us * 1000u) / ns) - UI_SCAN_V_LEAD_LINES;
    *line_ns = ns;
    return true;
}

static bool ui_scan_band_hits(int32_t b1, int32_t b2, int32_t top, int32_t bot)
{
    return b1 <= bot && b2 >= top;
}

/* Delays until writing rows [y, y+h) cannot cross the scanline. Returns the
 * time waited. Bounded: gives up (and tears) rather than stall the UI. */
static uint32_t ui_scan_gate(int32_t y, int32_t h, uint32_t px)
{
    if (px < UI_SCAN_GATE_MIN_PX) {
        return 0;
    }
    int64_t start_us = esp_timer_get_time();
    uint32_t max_wait_us = s_scan_refr_wait_us < UI_SCAN_REFR_BUDGET_US
                         ? UI_SCAN_REFR_BUDGET_US - s_scan_refr_wait_us : 0u;
    if (max_wait_us > UI_SCAN_MAX_WAIT_US) {
        max_wait_us = UI_SCAN_MAX_WAIT_US;
    }
    int32_t top = y - UI_SCAN_GUARD_LINES;
    int32_t bot = y + h - 1 + UI_SCAN_GUARD_LINES;
    for (int tries = 0; tries < 4; ++tries) {
        int32_t line;
        uint32_t line_ns;
        if (!ui_scan_line_now(&line, &line_ns)) {
            break;
        }
        int32_t copy_lines = (int32_t)(((uint64_t)px * s_scan_copy_ns_px) / line_ns) + 1;
        int32_t b1 = line;
        int32_t b2 = line + copy_lines;
        if (!ui_scan_band_hits(b1, b2, top, bot) &&
            !ui_scan_band_hits(b1 - UI_SCAN_V_TOTAL_LINES, b2 - UI_SCAN_V_TOTAL_LINES, top, bot)) {
            break;
        }
        /* The scan is in the rows or will reach them mid-copy: let it pass. */
        int32_t wait_lines = bot + 1 - line;
        if (wait_lines <= 0) {
            wait_lines += UI_SCAN_V_TOTAL_LINES;
        }
        uint32_t wait_us = (uint32_t)(((uint64_t)wait_lines * line_ns) / 1000u) + 1u;
        uint32_t waited_us = ui_lvgl_backend_perf_elapsed_us(start_us);
        if (waited_us + wait_us > max_wait_us) {
            s_scan_giveups++;
            break;
        }
        if (wait_us >= 2000u) {
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000u));
        } else {
            esp_rom_delay_us(wait_us);
        }
    }
    uint32_t waited_us = ui_lvgl_backend_perf_elapsed_us(start_us);
    s_scan_refr_wait_us += waited_us;
    return waited_us;
}

/* Moving average of msync + PPA cost per pixel, for the next copy estimate. */
static void ui_scan_learn_copy(uint32_t px, uint32_t copy_us)
{
    if (px < UI_SCAN_GATE_MIN_PX || copy_us == 0) {
        return;
    }
    uint32_t ns = (uint32_t)(((uint64_t)copy_us * 1000u) / px);
    if (ns < 2u) ns = 2u;
    if (ns > 64u) ns = 64u;
    s_scan_copy_ns_px = (s_scan_copy_ns_px * 7u + ns + 7u) / 8u;
}
#endif

static void ui_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!area || !px_map) {
        lv_display_flush_ready(disp);
        return;
    }

    int32_t area_w = area->x2 - area->x1 + 1;
    int32_t area_h = area->y2 - area->y1 + 1;
    if (area_w <= 0 || area_h <= 0) {
        lv_display_flush_ready(disp);
        return;
    }

    ui_overlay_rect_t logical = {
        .x = area->x1,
        .y = area->y1,
        .w = area_w,
        .h = area_h,
    };
    ui_overlay_rect_t physical;
    if (!ui_overlay_map_ppa270(logical, s_hor_res, s_ver_res, &physical)) {
        ESP_LOGW(TAG,
                 "LVGL flush area outside canvas: x=%d y=%d w=%d h=%d",
                 logical.x, logical.y, logical.w, logical.h);
        lv_display_flush_ready(disp);
        return;
    }

    for (uint32_t i = 0; i < UI_LVGL_BACKEND_DIRECT_SLOTS; i++) {
        const ui_overlay_rect_t *r = &s_direct_rect[i];
        if (r->w > 0 && logical.x < r->x + r->w && r->x < logical.x + logical.w &&
            logical.y < r->y + r->h && r->y < logical.y + logical.h) {
            s_direct_repainted |= 1u << i;
        }
    }

#ifdef UI_TARGET_JC1060
    ui_stall_mark(UI_STALL_PH_FLUSH);
    uint32_t px = (uint32_t)area_w * (uint32_t)area_h;
    s_refr_cur.gate_us += ui_scan_gate(physical.y, physical.h, px);
#endif

    ui_lvgl_backend_blit_perf_t perf = {0};
    esp_err_t err = ui_lvgl_backend_blit_rgb565_ppa270_mapped(&logical,
                                                              &physical,
                                                              (const uint16_t *)px_map,
                                                              (uint32_t)area_w,
                                                              (uint32_t)area_h,
                                                              0,
                                                              0,
                                                              (uint32_t)area_w,
                                                              (uint32_t)area_h,
                                                              (size_t)area_w * (size_t)area_h * sizeof(uint16_t),
                                                              &perf);
    if (ui_diagnostics_enabled()) {
        ui_perf_win_add(&s_win_flush_msync, perf.msync_us);
        ui_perf_win_add(&s_win_flush_ppa, perf.ppa_us);
        s_refr_cur.flush_us += perf.total_us;
        s_refr_cur.flushes++;
        s_refr_cur.flush_px += (uint32_t)area_w * (uint32_t)area_h;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LVGL flush PPA failed: %s", esp_err_to_name(err));
    }
#ifdef UI_TARGET_JC1060
    else {
        ui_scan_learn_copy(px, perf.total_us);
    }
    ui_stall_mark(UI_STALL_PH_HANDLER);
#endif
    lv_display_flush_ready(disp);
}

#ifdef UI_TARGET_JC1060
static void ui_stall_count_core(uint8_t core)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
    const char *name = task ? pcTaskGetName(task) : "?";
    ui_stall_slot_t *slots = s_stall.core[core];
    for (uint32_t i = 0; i < UI_STALL_SLOTS; i++) {
        if (slots[i].n == 0u) {
            /* Copied now: the task runs, so its name is valid. */
            strlcpy(slots[i].name, name, sizeof(slots[i].name));
        } else if (strncmp(slots[i].name, name, sizeof(slots[i].name)) != 0) {
            continue;
        }
        slots[i].n++;
        return;
    }
    s_stall.core_other[core]++;
}

static void ui_stall_report(uint32_t now_us, bool ongoing)
{
    if (!s_perf_task || __atomic_load_n(&s_stall_text_ready, __ATOMIC_ACQUIRE)) {
        return;   /* the previous report is still being printed: drop this one */
    }
    char *buf = s_stall_text;
    size_t size = sizeof(s_stall_text);
    size_t off = 0;
#define UI_STALL_APPEND(...)                                                 \
    do {                                                                     \
        if (off < size) {                                                    \
            int n_ = snprintf(buf + off, size - off, __VA_ARGS__);           \
            if (n_ > 0) off += (size_t)n_;                                   \
        }                                                                    \
    } while (0)
    UI_STALL_APPEND("lvgl STALL %s %u ms: entered in %s | lvgl ready %u blocked %u"
                    " of %u samples | phase",
                    ongoing ? "ongoing" : "ended",
                    (unsigned)((now_us - s_stall.start_us) / 1000u),
                    UI_STALL_PHASE_NAME[s_stall.phase],
                    (unsigned)s_stall.lvgl_ready, (unsigned)s_stall.lvgl_blocked,
                    (unsigned)s_stall.samples);
    for (uint32_t p = 0; p < UI_STALL_PH_COUNT; p++) {
        if (s_stall.phase_n[p] != 0u) {
            UI_STALL_APPEND(" %s %u", UI_STALL_PHASE_NAME[p], (unsigned)s_stall.phase_n[p]);
        }
    }
    for (uint8_t core = 0; core < 2u; core++) {
        UI_STALL_APPEND("\nlvgl STALL core%u runs:", (unsigned)core);
        for (uint32_t i = 0; i < UI_STALL_SLOTS && s_stall.core[core][i].n != 0u; i++) {
            UI_STALL_APPEND(" %.*s %u", (int)sizeof(s_stall.core[core][i].name),
                            s_stall.core[core][i].name, (unsigned)s_stall.core[core][i].n);
        }
        if (s_stall.core_other[core] != 0u) {
            UI_STALL_APPEND(" other %u", (unsigned)s_stall.core_other[core]);
        }
    }
#undef UI_STALL_APPEND
    const ui_lvgl_backend_stall_probe_cb_t probe =
        __atomic_load_n(&s_stall_probe_cb, __ATOMIC_ACQUIRE);
    if (probe && off < size) {
        off += probe(false, buf + off, size - off);
    }
    buf[size - 1] = '\0';
    __atomic_store_n(&s_stall_text_ready, true, __ATOMIC_RELEASE);
    xTaskNotifyGive(s_perf_task);
}

/* esp_timer task: every UI_STALL_SAMPLE_TICKS ticks, a few loads. */
static void ui_stall_sample(void)
{
    if (!s_lvgl_task_handle || ++s_stall_tick < UI_STALL_SAMPLE_TICKS) {
        return;
    }
    s_stall_tick = 0;
    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    const uint32_t beat_us = s_stall_beat_us;
    const bool stale = beat_us != 0u && now_us - beat_us >= UI_STALL_THRESHOLD_US;
    if (!stale) {
        if (s_stall.active) {
            ui_stall_report(now_us, false);
            s_stall.active = false;
        }
        return;
    }
    const ui_lvgl_backend_stall_probe_cb_t probe =
        __atomic_load_n(&s_stall_probe_cb, __ATOMIC_ACQUIRE);
    if (!s_stall.active) {
        s_stall = (ui_stall_window_t){
            .active = true,
            .start_us = beat_us,
            .phase = s_stall_phase < UI_STALL_PH_COUNT ? s_stall_phase : UI_STALL_PH_WAIT,
        };
        if (probe) {
            (void)probe(true, NULL, 0);
        }
    }
    const uint8_t phase = s_stall_phase;
    if (phase < UI_STALL_PH_COUNT) {
        s_stall.phase_n[phase]++;
    }
    const eTaskState state = eTaskGetState(s_lvgl_task_handle);
    if (state == eRunning || state == eReady) {
        s_stall.lvgl_ready++;
    } else {
        s_stall.lvgl_blocked++;
    }
    ui_stall_count_core(0);
    ui_stall_count_core(1);
    if (++s_stall.samples >= UI_STALL_REPORT_SAMPLES) {
        ui_stall_report(now_us, true);
        const uint32_t start_us = s_stall.start_us;
        const uint8_t entered = s_stall.phase;
        s_stall = (ui_stall_window_t){
            .active = true, .start_us = start_us, .phase = entered,
        };
        if (probe) {
            (void)probe(true, NULL, 0);
        }
    }
}
#endif

static void ui_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
#ifdef UI_TARGET_JC1060
    if (ui_diagnostics_enabled()) {
        ui_stall_sample();
    }
#endif
}

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = lv_indev_get_user_data(indev);
    if (tp == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_point_data_t point = {0};
    uint8_t cnt = 0;
#ifdef UI_TARGET_JC1060
    ui_stall_mark(UI_STALL_PH_TOUCH);
#endif
    esp_lcd_touch_read_data(tp);
#ifdef UI_TARGET_JC1060
    ui_stall_mark(UI_STALL_PH_HANDLER);
#endif
    esp_err_t rc = esp_lcd_touch_get_data(tp, &point, &cnt, 1);
    if (rc == ESP_OK && cnt > 0) {
        /* Any touch counts as activity. The screensaver is a separate LVGL
         * screen with no widgets, so a dismissing touch cannot also press
         * whatever sits underneath. */
        (void)ui_activity_notice();
        data->point.x = point.x;
        data->point.y = point.y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* v204 play-crash test mode (Settings -> OUTPUT, NVS "ui_blk"): while any
 * deck plays, the LVGL task does no UI work at all - no ui_update(), no
 * lv_timer_handler(), so no render, PPA flush or waveform blit. Answers "does
 * the WDT on PLAY disappear without the UI load?". The DPI panel keeps
 * scanning the last frame from PSRAM; touch and controller browse/load are
 * frozen until every deck is paused. */
static bool ui_lvgl_play_blackout_active(void)
{
    if (!app_settings_get().ui_blackout_play) {
        return false;
    }
    for (uint8_t deck = 0; deck < AUDIO_ENGINE_DECK_COUNT; ++deck) {
        if (audio_engine_deck_is_playing(deck)) {
            return true;
        }
    }
    return false;
}

static void ui_perf_print(const char *line)
{
    while (*line) {
        const char *end = strchr(line, '\n');
        int len = end ? (int)(end - line) : (int)strlen(line);
        if (len > 0) {
            ESP_LOGI(PERF_TAG, "%.*s", len, line);
        }
        line += len + (end ? 1 : 0);
    }
}

static void ui_perf_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Two producers share the notification; each text has its own flag
         * so a stall report never prints a window the LVGL task is writing. */
        if (__atomic_load_n(&s_perf_busy, __ATOMIC_ACQUIRE)) {
            ui_perf_print(s_perf_text);
            __atomic_store_n(&s_perf_busy, false, __ATOMIC_RELEASE);
        }
#ifdef UI_TARGET_JC1060
        if (__atomic_load_n(&s_stall_text_ready, __ATOMIC_ACQUIRE)) {
            ui_perf_print(s_stall_text);
            __atomic_store_n(&s_stall_text_ready, false, __ATOMIC_RELEASE);
        }
#endif
    }
}

/* LVGL task, after lv_timer_handler(): formats the window, never prints. */
static void ui_perf_publish(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_perf_window_start_us == 0) {
        s_perf_window_start_us = now_us;
        return;
    }
    if (!s_perf_task || __atomic_load_n(&s_perf_busy, __ATOMIC_ACQUIRE) ||
        now_us - s_perf_window_start_us < (int64_t)UI_PERF_REPORT_MS * 1000) {
        return;
    }
    char *buf = s_perf_text;
    size_t size = UI_PERF_TEXT_MAX;
    size_t off = 0;
    const ui_perf_refr_t *w = &s_refr_worst;
#define UI_PERF_APPEND(...)                                                  \
    do {                                                                     \
        if (off < size) {                                                    \
            int n_ = snprintf(buf + off, size - off, __VA_ARGS__);           \
            if (n_ > 0) off += (size_t)n_;                                   \
        }                                                                    \
    } while (0)
    UI_PERF_APPEND("lvgl refr n=%u interval avg/max %u/%u us | refr avg/max %u/%u us"
                   " = draw %u/%u + flush %u/%u us | render max %u us\n",
                   (unsigned)s_win_refr_total.n,
                   (unsigned)ui_perf_win_avg(&s_win_refr_interval), (unsigned)s_win_refr_interval.max,
                   (unsigned)ui_perf_win_avg(&s_win_refr_total), (unsigned)s_win_refr_total.max,
                   (unsigned)ui_perf_win_avg(&s_win_refr_draw), (unsigned)s_win_refr_draw.max,
                   (unsigned)ui_perf_win_avg(&s_win_refr_flush), (unsigned)s_win_refr_flush.max,
                   (unsigned)s_win_render_total.max);
    UI_PERF_APPEND("lvgl handler n=%u interval max %u us, duration avg/max %u/%u us |"
                   " frame cb avg/max %u/%u us | PPA avg/max %u/%u us, msync max %u us,"
                   " per refr max %u flushes %u px\n",
                   (unsigned)s_win_handler_duration.n, (unsigned)s_win_handler_interval.max,
                   (unsigned)ui_perf_win_avg(&s_win_handler_duration), (unsigned)s_win_handler_duration.max,
                   (unsigned)ui_perf_win_avg(&s_win_frame_cb), (unsigned)s_win_frame_cb.max,
                   (unsigned)ui_perf_win_avg(&s_win_flush_ppa), (unsigned)s_win_flush_ppa.max,
                   (unsigned)s_win_flush_msync.max,
                   (unsigned)s_refr_flushes_max, (unsigned)s_refr_flush_px_max);
    UI_PERF_APPEND("lvgl worst refr %u us: flush %u us (%u flushes, %u px) gate %u us;"
                   " inval %u areas %u px, max (%d,%d %dx%d)\n",
                   (unsigned)w->refr_us, (unsigned)w->flush_us, (unsigned)w->flushes,
                   (unsigned)w->flush_px, (unsigned)w->gate_us,
                   (unsigned)w->inval_count, (unsigned)w->inval_px,
                   (int)w->inval_max.x1, (int)w->inval_max.y1,
                   (int)(w->inval_max.x2 - w->inval_max.x1 + 1),
                   (int)(w->inval_max.y2 - w->inval_max.y1 + 1));
#ifdef UI_TARGET_JC1060
    UI_PERF_APPEND("lvgl scan-gate per refr avg/max %u/%u us, gave up (tore) %u\n",
                   (unsigned)ui_perf_win_avg(&s_win_refr_gate), (unsigned)s_win_refr_gate.max,
                   (unsigned)s_scan_giveups);
    s_win_refr_gate = (ui_perf_win_t){0};
    s_scan_giveups = 0;
#endif
#undef UI_PERF_APPEND
    if (s_perf_report_cb && off < size) {
        s_perf_report_cb(buf + off, size - off);
    }
    buf[size - 1] = '\0';

    s_win_handler_interval = (ui_perf_win_t){0};
    s_win_handler_duration = (ui_perf_win_t){0};
    s_win_frame_cb = (ui_perf_win_t){0};
    s_win_refr_interval = (ui_perf_win_t){0};
    s_win_refr_total = (ui_perf_win_t){0};
    s_win_refr_draw = (ui_perf_win_t){0};
    s_win_refr_flush = (ui_perf_win_t){0};
    s_win_render_total = (ui_perf_win_t){0};
    s_win_flush_ppa = (ui_perf_win_t){0};
    s_win_flush_msync = (ui_perf_win_t){0};
    s_refr_worst = (ui_perf_refr_t){0};
    s_refr_flush_px_max = 0;
    s_refr_flushes_max = 0;
    s_perf_window_start_us = now_us;
    __atomic_store_n(&s_perf_busy, true, __ATOMIC_RELEASE);
    xTaskNotifyGive(s_perf_task);
}

static void ui_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL handler task started");
    uint64_t last_handler_start_us = 0;
    bool refresh_pending = false;
    bool blackout = false;
    int64_t blackout_start_us = 0;
    if (app_settings_get().ui_blackout_play) {
        ESP_LOGW(TAG, "TEST UI blackout on PLAY is armed (Settings -> OUTPUT)");
    }
    while (1) {
        bool blackout_now = ui_lvgl_play_blackout_active();
        if (blackout_now != blackout) {
            blackout = blackout_now;
            if (blackout) {
                blackout_start_us = esp_timer_get_time();
                ESP_LOGW(TAG, "TEST UI blackout: rendering stopped while playing");
            } else {
                ESP_LOGW(TAG, "TEST UI blackout: rendering resumed after %u ms",
                         (unsigned)((esp_timer_get_time() - blackout_start_us) / 1000));
                last_handler_start_us = 0;
                refresh_pending = false;
            }
        }
        if (blackout) {
#ifdef UI_TARGET_JC1060
            ui_stall_mark(UI_STALL_PH_WAIT);
#endif
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint64_t handler_start_us = (uint64_t)esp_timer_get_time();
        bool diag = ui_diagnostics_enabled();
        if (diag && last_handler_start_us != 0) {
            ui_perf_win_add(&s_win_handler_interval,
                            (uint32_t)(handler_start_us - last_handler_start_us));
        }
        last_handler_start_us = handler_start_us;

#ifdef UI_TARGET_JC1060
        ui_stall_mark(UI_STALL_PH_LOCK);
#endif
        _lock_acquire_recursive(&s_lvgl_lock);
        if (refresh_pending && s_frame_callback != NULL) {
#ifdef UI_TARGET_JC1060
            ui_stall_mark(UI_STALL_PH_FRAME_CB);
#endif
            int64_t frame_cb_start_us = esp_timer_get_time();
            s_frame_callback(s_frame_callback_ctx);
            if (diag) {
                ui_perf_win_add(&s_win_frame_cb, ui_lvgl_backend_perf_elapsed_us(frame_cb_start_us));
            }
        }
#ifdef UI_TARGET_JC1060
        s_scan_refr_wait_us = 0;
        ui_stall_mark(UI_STALL_PH_HANDLER);
#endif
        uint32_t next_ms = lv_timer_handler();
#ifdef UI_TARGET_JC1060
        ui_stall_mark(UI_STALL_PH_POST);
#endif
        if (s_post_refresh_callback != NULL) {
            uint32_t repainted = s_direct_repainted;
            s_direct_repainted = 0;
            s_post_refresh_callback(repainted, s_post_refresh_callback_ctx);
        }
        if (diag) {
            ui_perf_win_add(&s_win_handler_duration,
                            (uint32_t)((uint64_t)esp_timer_get_time() - handler_start_us));
            ui_perf_publish();
        }
        _lock_release_recursive(&s_lvgl_lock);

        if (next_ms > 100) next_ms = 100;
        if (next_ms < 5)   next_ms = 5;

        // Panel refreshes wake the task immediately. The timeout still follows
        // LVGL's requested cadence, preserving timers, input and animations
        // even if the display interrupt stops arriving.
        uint32_t notifications = 0;
#ifdef UI_TARGET_JC1060
        ui_stall_mark(UI_STALL_PH_WAIT);
#endif
        BaseType_t notified = xTaskNotifyWait(0,
                                              UINT32_MAX,
                                              &notifications,
                                              pdMS_TO_TICKS(next_ms));
        refresh_pending = notified == pdTRUE &&
                          (notifications & UI_LVGL_NOTIFY_REFRESH) != 0;
    }
}

esp_err_t ui_lvgl_backend_init(uint16_t hor_res, uint16_t ver_res)
{
    if (hor_res == 0 || ver_res == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hor_res = hor_res;
    s_ver_res = ver_res;
    _lock_init_recursive(&s_lvgl_lock);

    esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();
    if (panel == NULL) {
        ESP_LOGE(TAG, "panel handle is NULL - call bsp_display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    s_dsi_active_fb_idx = 0;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, UI_DSI_FB_COUNT,
                                                       &s_dsi_fb[0]));
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM, &s_cache_align));

    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ppa));

    lv_init();
#if LV_USE_TJPGD
    /* v281: TJpgDec is built for ui_artwork_thumb only. lv_init() registers
     * LVGL's JPEG image decoder at the head of the decoder list, where it would
     * be asked first on every image draw (the image cache is off): drop it so
     * the draw path is the one before the artwork feature. */
    lv_tjpgd_deinit();
#endif

    s_disp = lv_display_create(s_hor_res, s_ver_res);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(s_disp, panel);
    lv_display_set_flush_cb(s_disp, ui_lvgl_flush_cb);
    lv_display_add_event_cb(s_disp, ui_lvgl_display_event_cb, LV_EVENT_ALL, NULL);

    size_t buf_sz = ALIGN_UP_BY((size_t)s_hor_res *
                                UI_LVGL_PARTIAL_BUF_ROWS *
                                sizeof(uint16_t),
                                s_cache_align);
    void *buf1 = heap_caps_aligned_alloc(s_cache_align, buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf1) {
        ESP_LOGE(TAG, "failed to allocate %u-byte LVGL draw buffer from PSRAM", (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, buf1, NULL, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    const esp_timer_create_args_t tick_args = {
        .callback = ui_lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    esp_lcd_touch_handle_t tp = bsp_touch_get_handle();
    if (tp != NULL) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_user_data(indev, tp);
        lv_indev_set_read_cb(indev, ui_touch_read_cb);
        ESP_LOGI(TAG, "GT911 registered as LVGL pointer input");
    } else {
        ESP_LOGW(TAG, "no touch handle - UI will be display-only");
    }

    // Register last so a partially initialised backend can never receive a
    // refresh notification. The ISR only wakes the LVGL task; it does no UI
    // or LVGL work itself.
    const esp_lcd_dpi_panel_event_callbacks_t panel_cbs = {
        .on_refresh_done = ui_lvgl_dpi_refresh_done_cb,
    };
    esp_err_t panel_cb_rc = esp_lcd_dpi_panel_register_event_callbacks(panel,
                                                                       &panel_cbs,
                                                                       NULL);
    if (panel_cb_rc != ESP_OK) {
        ESP_LOGE(TAG,
                 "failed to register DPI refresh callback: %s",
                 esp_err_to_name(panel_cb_rc));
        /* Last failure path in this function, and the only one after the draw
         * buffer exists. Hand it back rather than leaving a full partial-render
         * buffer stranded in PSRAM on a boot that is about to be retried. */
        lv_display_set_buffers(s_disp, NULL, NULL, 0, LV_DISPLAY_RENDER_MODE_PARTIAL);
        heap_caps_free(buf1);
        return panel_cb_rc;
    }

    ESP_LOGI(TAG,
             "LVGL backend ready (%ux%u canvas -> PPA-rotated to %dx%d, RGB565, one DPI framebuffer)",
             (unsigned)s_hor_res,
             (unsigned)s_ver_res,
             BSP_LCD_H_RES,
             BSP_LCD_V_RES);
    return ESP_OK;
}

esp_err_t ui_lvgl_backend_set_frame_callback(ui_lvgl_backend_frame_cb_t callback,
                                             void *user_ctx)
{
    if (s_lvgl_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_frame_callback = callback;
    s_frame_callback_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t ui_lvgl_backend_set_perf_report_callback(ui_lvgl_backend_perf_report_cb_t callback)
{
    if (s_lvgl_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_perf_report_cb = callback;
    return ESP_OK;
}

void ui_lvgl_backend_set_stall_probe_callback(ui_lvgl_backend_stall_probe_cb_t callback)
{
#ifdef UI_TARGET_JC1060
    __atomic_store_n(&s_stall_probe_cb, callback, __ATOMIC_RELEASE);
#else
    (void)callback;
#endif
}

esp_err_t ui_lvgl_backend_set_post_refresh_callback(ui_lvgl_backend_post_refresh_cb_t callback,
                                                    void *user_ctx)
{
    if (s_lvgl_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_post_refresh_callback = callback;
    s_post_refresh_callback_ctx = user_ctx;
    return ESP_OK;
}

void ui_lvgl_backend_set_direct_rect(uint8_t slot, const ui_overlay_rect_t *logical)
{
    if (slot >= UI_LVGL_BACKEND_DIRECT_SLOTS) {
        return;
    }
    s_direct_rect[slot] = logical ? *logical : (ui_overlay_rect_t){0};
}

/* Diagnostics only, allocated once here. Without it the windows still run and
 * nothing is printed. Same core as LVGL, below everything that matters. */
static void ui_perf_task_start(void)
{
    if (!ui_diagnostics_enabled()) {
        return;
    }
    s_perf_text = heap_caps_calloc(1, UI_PERF_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_perf_text ||
        xTaskCreatePinnedToCoreWithCaps(ui_perf_task, "ui_perf", UI_PERF_TASK_STACK, NULL,
                                        UI_PERF_TASK_PRIO, &s_perf_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(s_perf_text);
        s_perf_text = NULL;
        s_perf_task = NULL;
        ESP_LOGW(TAG, "ui_perf task not started: UI perf reports disabled");
        return;
    }
    /* Default level is WARN: the reports are INFO under their own tag. */
    esp_log_level_set(PERF_TAG, ESP_LOG_INFO);
}

esp_err_t ui_lvgl_backend_start(void)
{
    if (s_lvgl_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ui_perf_task_start();
    if (xTaskCreatePinnedToCore(ui_lvgl_task,
                                "lvgl",
                                LVGL_TASK_STACK,
                                NULL,
                                LVGL_TASK_PRIO,
                                &s_lvgl_task_handle,
                                1) != pdPASS) {
        s_lvgl_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ui_lvgl_lock(void)
{
    _lock_acquire_recursive(&s_lvgl_lock);
}

void ui_lvgl_unlock(void)
{
    _lock_release_recursive(&s_lvgl_lock);
}

void *ui_lvgl_backend_alloc_dma_buffer(size_t bytes, size_t *aligned_bytes)
{
    if (bytes == 0) {
        if (aligned_bytes) {
            *aligned_bytes = 0;
        }
        return NULL;
    }

    size_t aligned = ALIGN_UP_BY(bytes, s_cache_align);
    if (aligned_bytes) {
        *aligned_bytes = aligned;
    }

    void *buf = heap_caps_aligned_alloc(s_cache_align,
                                        aligned,
                                        MALLOC_CAP_INTERNAL |
                                            MALLOC_CAP_DMA |
                                            MALLOC_CAP_8BIT);
    if (buf) {
        return buf;
    }
    buf = heap_caps_aligned_alloc(s_cache_align,
                                  aligned,
                                  MALLOC_CAP_SPIRAM |
                                      MALLOC_CAP_DMA |
                                      MALLOC_CAP_8BIT);
    if (buf) {
        return buf;
    }
    return heap_caps_aligned_alloc(s_cache_align,
                                   aligned,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

esp_err_t ui_lvgl_backend_blit_rgb565_ppa270(const ui_overlay_rect_t *logical,
                                             const uint16_t *src,
                                             uint32_t src_w,
                                             uint32_t src_h,
                                             size_t src_bytes,
                                             ui_lvgl_backend_blit_perf_t *perf)
{
    return ui_lvgl_backend_blit_rgb565_ppa270_region(logical,
                                                     src,
                                                     src_w,
                                                     src_h,
                                                     0,
                                                     0,
                                                     src_w,
                                                     src_h,
                                                     src_bytes,
                                                     perf);
}

esp_err_t ui_lvgl_backend_blit_rgb565_ppa270_region(const ui_overlay_rect_t *logical,
                                                    const uint16_t *src,
                                                    uint32_t src_w,
                                                    uint32_t src_h,
                                                    uint32_t src_x,
                                                    uint32_t src_y,
                                                    uint32_t block_w,
                                                    uint32_t block_h,
                                                    size_t src_bytes,
                                                    ui_lvgl_backend_blit_perf_t *perf)
{
    if (perf) {
        memset(perf, 0, sizeof(*perf));
    }
    esp_err_t arg_err = ui_lvgl_backend_validate_rgb565_region_args(logical,
                                                                    src,
                                                                    src_w,
                                                                    src_h,
                                                                    src_x,
                                                                    src_y,
                                                                    block_w,
                                                                    block_h,
                                                                    src_bytes);
    if (arg_err != ESP_OK) {
        return arg_err;
    }

    ui_overlay_rect_t physical;
    if (!ui_overlay_map_ppa270(*logical, s_hor_res, s_ver_res, &physical)) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef UI_TARGET_JC1060
    /* v287: direct blits (dj_ui zoom strips) write the scanned framebuffer
     * like a flush does, so they take the same anti-tear gate and budget. */
    uint32_t px = block_w * block_h;
    (void)ui_scan_gate(physical.y, physical.h, px);
    ui_lvgl_backend_blit_perf_t local_perf;
    if (!perf) {
        perf = &local_perf;
    }
#endif
    esp_err_t err = ui_lvgl_backend_blit_rgb565_ppa270_mapped(logical,
                                                              &physical,
                                                              src,
                                                              src_w,
                                                              src_h,
                                                              src_x,
                                                              src_y,
                                                              block_w,
                                                              block_h,
                                                              src_bytes,
                                                              perf);
#ifdef UI_TARGET_JC1060
    if (err == ESP_OK) {
        ui_scan_learn_copy(px, perf->total_us);
    }
#endif
    return err;
}

esp_err_t ui_lvgl_backend_draw_rect_rgb565(const ui_overlay_rect_t *logical, uint16_t color)
{
    if (!logical || logical->w <= 0 || logical->h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_dsi_active_fb_idx < 0 || s_dsi_active_fb_idx >= UI_DSI_FB_COUNT ||
        !s_dsi_fb[s_dsi_active_fb_idx]) {
        return ESP_ERR_INVALID_STATE;
    }

    ui_overlay_rect_t physical;
    if (!ui_overlay_map_ppa270(*logical, s_hor_res, s_ver_res, &physical)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *fb = (uint16_t *)s_dsi_fb[s_dsi_active_fb_idx];
    uint32_t stride = BSP_LCD_H_RES;

    for (int y = physical.y; y < physical.y + physical.h; y++) {
        uint16_t *row = fb + (size_t)y * stride;
        for (int x = physical.x; x < physical.x + physical.w; x++) {
            row[x] = color;
        }
    }

    // Cache sync to RAM for DMA controller to pick it up
    esp_cache_msync((void *)(fb + (size_t)physical.y * stride),
                    (size_t)physical.h * stride * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    return ESP_OK;
}
#endif
