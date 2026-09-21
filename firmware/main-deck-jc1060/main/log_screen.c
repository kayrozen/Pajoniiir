/*
 * log_screen.c - on-screen verbose log viewer.
 *
 * Installs a custom esp_log vprintf that tees every log line into a ring
 * buffer and renders the last N lines on a full-screen LVGL label, so the
 * device can be debugged without a serial console attached. The previous
 * vprintf (UART + Wi-Fi console) keeps working.
 */
#include "log_screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#define LS_MAX_LINES  30
#define LS_LINE_MAX   160
#define LS_COLOR_MAX  512   /* total bytes kept */

static const char* TAG = "log_screen";

static vprintf_like_t s_prev_vprintf;
static SemaphoreHandle_t s_lock;
static char s_lines[LS_MAX_LINES][LS_LINE_MAX];
static int s_head;              /* next slot to write */
static int s_count;
static lv_obj_t *s_label;
static lv_obj_t *s_screen;
static bool s_dirty;
static char s_render[LS_MAX_LINES * LS_LINE_MAX];

static void ls_render_locked(void);
static void ls_lvgl_task(void *arg);

static void ls_put_line(const char *line)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;                 /* never block the log path */
    }
    strlcpy(s_lines[s_head], line, LS_LINE_MAX);
    s_head = (s_head + 1) % LS_MAX_LINES;
    if (s_count < LS_MAX_LINES) {
        s_count++;
    }
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

/* esp_log vprintf: format, strip trailing newline, tee, chain. */
static int ls_vprintf(const char *fmt, va_list args)
{
    char buf[LS_LINE_MAX];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        /* esp_log emits one full line per call, usually ending in \n. */
        int n = (int)strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n > 0) {
            ls_put_line(buf);
        }
    }
    if (s_prev_vprintf) {
        va_list args2;
        va_copy(args2, args);
        s_prev_vprintf(fmt, args2);
        va_end(args2);
    }
    return len;
}

/* LVGL context: rebuild the label text from the ring buffer. */
static void ls_render_locked(void)
{
    /* Always refresh a status header (line count) + log tail. */
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "log screen: lines=%d dirty=%d head=%d\n",
             s_count, (int)s_dirty, s_head);
    if (s_label == NULL) {
        return;
    }
    size_t off = (size_t)snprintf(s_render, sizeof(s_render), "%s", hdr);
    for (int i = 0; i < s_count; i++) {
        int idx = (s_head - s_count + i + LS_MAX_LINES * 2) % LS_MAX_LINES;
        off += (size_t)snprintf(s_render + off, sizeof(s_render) - off, "%s\n",
                                s_lines[idx]);
        if (off >= sizeof(s_render) - LS_LINE_MAX) {
            break;
        }
    }
    lv_label_set_text(s_label, s_render);
    lv_obj_scroll_to_y(s_label, LV_COORD_MAX, LV_ANIM_OFF);
    s_dirty = false;
}

void log_screen_rehook(void)
{
    if (s_prev_vprintf) {
        esp_log_set_vprintf(ls_vprintf);
    }
}

void log_screen_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(ls_vprintf);

    if (lv_is_initialized() && lv_display_get_default() != NULL) {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101418), 0);
        lv_obj_set_style_border_width(s_screen, 0, 0);

        s_label = lv_label_create(s_screen);
        /* v60: back to fullscreen - the 200px bandeau (v54) is no longer
         * needed since the DMA2D flush removed the USB iso contention. */
        lv_obj_set_size(s_label, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_text_color(s_label, lv_color_hex(0x30d060), 0);
        lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_all(s_label, 6, 0);
        lv_label_set_text(s_label, "log screen: waiting");
        lv_scr_load(s_screen);
    }
    /* Marker line so we can tell "ring works, vprintf not hooked" from
     * "screen not rendering" at a glance. */
    ls_put_line("[log_screen] ready");
    /* esp_lvgl_port renders the screen automatically - no local task needed. */
}

/* v91: heartbeat - proves the main loop + screen render are alive, and
 * shows uptime so boot-time vs run-time is obvious. */
static int s_alive_tick;

void log_screen_task(void)
{
    if (s_lock == NULL) {
        return;
    }
    /* v68 color test removed (v70): diagnostic complete — the panel needs
     * INVOFF (fixed in v69). Resume normal log rendering. */
    /* esp_lvgl_port owns the render task; take its lock to touch widgets. */
    if (!bsp_display_lock(50)) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        ls_render_locked();
        xSemaphoreGive(s_lock);
    }
    bsp_display_unlock();

    /* v91: alive marker every ~5 s (25 x 200 ms loop). */
    if (++s_alive_tick % 25 == 0) {
        char line[64];
        snprintf(line, sizeof(line), "[alive] uptime=%ds",
                 (int)(esp_timer_get_time() / 1000000LL));
        ls_put_line(line);
    }
}
