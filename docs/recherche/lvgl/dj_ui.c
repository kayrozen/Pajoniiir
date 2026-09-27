#include "dj_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define ZOOM_WINDOW_MS 8000   /* visible time in zoomed waveform */
#define BAR_W    3
#define BAR_STEP 4
#define OV_STEP  4
#define OV_MAX   256

#define F12 (&lv_font_montserrat_12)
#define F14 (&lv_font_montserrat_14)
#define F16 (&lv_font_montserrat_16)
#define F20 (&lv_font_montserrat_20)
#define F24 (&lv_font_montserrat_24)
#define F28 (&lv_font_montserrat_28)
#define F32 (&lv_font_montserrat_32)

#define C_BG        lv_color_hex(0x0d0f11)
#define C_WAVEBG    lv_color_hex(0x08090b)
#define C_PANEL     lv_color_hex(0x181a1d)
#define C_LINE      lv_color_hex(0x2b2e32)
#define C_ROWLINE   lv_color_hex(0x1f2124)
#define C_BORDER2   lv_color_hex(0x4a4e54)
#define C_INK       lv_color_hex(0xeef0f2)
#define C_MUTED     lv_color_hex(0x9a9ea4)
#define C_DIM       lv_color_hex(0x6e7278)
#define C_DARK      lv_color_hex(0x0d0f11)
#define C_WHITE     lv_color_hex(0xf6f6f6)
#define C_GRID_BEAT lv_color_hex(0x1f2226)
#define C_GRID_BAR  lv_color_hex(0x4a4e55)
#define C_OK        lv_color_hex(0x6fd48a)
#define C_WARN      lv_color_hex(0xefb453)
#define C_INFO      lv_color_hex(0x6fb0f0)
#define C_ERR       lv_color_hex(0xf06a5f)

#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 3
#define DJ_LONG_DOT LV_LABEL_LONG_MODE_DOTS
#else
#define DJ_LONG_DOT LV_LABEL_LONG_DOT
#endif

#define UD(i)  ((void *)(intptr_t)(i))
#define IDX(e) ((int)(intptr_t)lv_event_get_user_data(e))

static const uint32_t DECK_COL[2]  = { 0x48d4a8, 0xee8fd0 };
static const uint32_t DECK_CORE[2] = { 0xb8f2dd, 0xfcd0ee };
static const uint32_t CUE_COL[8]   = { 0xe8695f, 0xe58a3c, 0xb9a92c, 0x2fc596, 0x20b6d0, 0x6c9bf5, 0xb07fe8, 0xe06aa9 };

typedef struct {
    lv_color_t color, core;
    /* overview widgets */
    lv_obj_t *info_trk, *info_src, *info_key, *info_tempo, *info_bpm;
    lv_obj_t *zoom, *zoom_time;
    lv_obj_t *pad[DJ_HOTCUES];
    lv_obj_t *ft_title, *ft_artist, *ft_art_lbl, *ft_art, *ft_remain, *ft_tempo, *ft_bpm_cap, *ft_bpm, *ov;
    /* data */
    const uint8_t *peaks, *cores;
    uint32_t count; uint16_t pps;
    uint32_t len_ms, pos_ms, first_beat_ms;
    float bpm, tempo;
    int32_t last_tenth;
    bool cue_set[DJ_HOTCUES]; uint32_t cue_ms[DJ_HOTCUES]; uint8_t cue_col[DJ_HOTCUES];
    uint8_t ov[OV_MAX]; int32_t ov_n; bool ov_valid;
} deck_t;

static struct {
    deck_t deck[DJ_DECKS];
    lv_obj_t *tab_btn[4], *page[4];
    bool grid;
    lv_obj_t *fx_name, *fx_ch, *fx_beat[4], *fx_time, *fx_level, *fx_level_lbl, *fx_on;
    lv_obj_t *row[DJ_LIB_ROWS], *row_lbl[DJ_LIB_ROWS][5];
    uint8_t rows; int8_t sel; int8_t loaded[DJ_DECKS];
    lv_obj_t *lib_info, *lib_strip, *lib_deck, *lib_status, *sort_btn[4];
    dj_sort_t sort;
    lv_obj_t *tgt_btn[DJ_DECKS], *hc_card[DJ_HOTCUES], *hc_name[DJ_HOTCUES], *hc_val[DJ_HOTCUES];
    uint8_t target;
    lv_obj_t *bright, *bright_lbl, *wl_sw, *wl_lbl;
    lv_obj_t *f_lbl[DJ_F_COUNT], *f_box[DJ_F_COUNT];
    dj_ui_callbacks_t cb;
} g;

/* ---------------------------------------------------------------- helpers */

static lv_obj_t *box(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t bg, lv_color_t bd)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, bd, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 4, 0);
    return o;
}

static lv_obj_t *plain(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = box(p, x, y, w, h, C_BG, C_BG);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    return o;
}

static lv_obj_t *strip(lv_obj_t *p, int32_t h, lv_color_t c)
{
    lv_obj_t *o = box(p, 0, 0, 3, h, c, c);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    return o;
}

static lv_obj_t *txt(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *s, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *txt_r(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *s, int32_t x_ofs, int32_t y)
{
    lv_obj_t *l = txt(p, f, c, s, 0, 0);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, x_ofs, y);
    return l;
}

static lv_obj_t *cap(lv_obj_t *p, const char *s, int32_t x, int32_t y)
{
    lv_obj_t *l = txt(p, F12, C_MUTED, s, x, y);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

static lv_obj_t *btn(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h, const char *s, const lv_font_t *f,
                     lv_color_t bg, lv_color_t fg, lv_color_t bd, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *o = box(p, x, y, w, h, bg, bd);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(o, C_INK, LV_STATE_PRESSED);
    lv_obj_t *l = txt(o, f, fg, s, 0, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, ud);
    return o;
}

static void btn_style(lv_obj_t *o, lv_color_t bg, lv_color_t fg, lv_color_t bd)
{
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_border_color(o, bd, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(o, 0), fg, 0);
}

static lv_color_t tone_col(dj_tone_t t)
{
    switch (t) {
    case DJ_TONE_MUTED: return C_MUTED;
    case DJ_TONE_OK:    return C_OK;
    case DJ_TONE_WARN:  return C_WARN;
    case DJ_TONE_INFO:  return C_INFO;
    case DJ_TONE_ERROR: return C_ERR;
    default:            return C_INK;
    }
}

static void apply_tone(dj_field_t f, dj_tone_t t)
{
    lv_color_t c = tone_col(t);
    lv_obj_set_style_text_color(g.f_lbl[f], c, 0);
    if (g.f_box[f]) lv_obj_set_style_border_color(g.f_box[f], t == DJ_TONE_NORMAL ? C_BORDER2 : c, 0);
}

static void field_txt(lv_obj_t *p, dj_field_t f, const lv_font_t *font, int32_t x, int32_t y, int32_t w,
                      const char *s, dj_tone_t t)
{
    lv_obj_t *l = txt(p, font, C_INK, s, x, y);
    lv_label_set_long_mode(l, DJ_LONG_DOT);
    lv_obj_set_width(l, w);
    g.f_lbl[f] = l; g.f_box[f] = NULL;
    apply_tone(f, t);
}

static void field_box(lv_obj_t *p, dj_field_t f, int32_t x, int32_t y, int32_t w, int32_t h, const char *s, dj_tone_t t)
{
    lv_obj_t *b = box(p, x, y, w, h, C_PANEL, C_BORDER2);
    lv_obj_t *l = txt(b, F14, C_INK, s, 0, 0);
    lv_obj_center(l);
    g.f_box[f] = b; g.f_lbl[f] = l;
    apply_tone(f, t);
}

static void chip(lv_obj_t *p, dj_field_t f, const char *s, dj_tone_t t)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 7, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_t *l = txt(b, F12, C_INK, s, 0, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    g.f_box[f] = b; g.f_lbl[f] = l;
    apply_tone(f, t);
}

static lv_obj_t *flex_row(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = plain(p, x, y, w, h);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 8, 0);
    return o;
}

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = c; r.bg_opa = opa; r.radius = 0;
    r.border_width = 0; r.outline_width = 0; r.shadow_width = 0;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &r, &a);
}

static void fmt_mmss(char *b, size_t n, uint32_t ms)
{
    snprintf(b, n, "%02lu:%02lu.%lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60),
             (unsigned long)(ms / 100 % 10));
}

static void fmt_hms(char *b, size_t n, uint32_t ms)
{
    unsigned long s = ms / 1000;
    snprintf(b, n, "%02lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
}

/* ---------------------------------------------------------------- waveforms */

static void zoom_draw(lv_event_t *e)
{
    deck_t *d = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t cx = a.x1 + w / 2, mid = a.y1 + h / 2, maxh = (h - 20) / 2;
    float mspp = (float)ZOOM_WINDOW_MS / (float)w;
    float pos = (float)d->pos_ms;

    if (g.grid && d->bpm > 1.f) {
        float beat = 60000.f / d->bpm;
        float t0 = pos - (w / 2) * mspp;
        long n = (long)ceilf((t0 - (float)d->first_beat_ms) / beat);
        for (;; n++) {
            float t = (float)d->first_beat_ms + n * beat;
            int32_t x = cx + (int32_t)((t - pos) / mspp);
            if (x > a.x2) break;
            if (x < a.x1) continue;
            fill(layer, x, a.y1, x, a.y2, (((n % 4) + 4) % 4 == 0) ? C_GRID_BAR : C_GRID_BEAT, LV_OPA_COVER);
        }
    }

    if (d->peaks && d->count && d->pps) {
        for (int32_t x = a.x1; x + BAR_W - 1 <= a.x2; x += BAR_STEP) {
            float t = pos + (x + BAR_W / 2 - cx) * mspp;
            if (t < 0) continue;
            uint32_t i = (uint32_t)(t * d->pps / 1000.f);
            if (i >= d->count) break;
            int32_t hh = d->peaks[i] * maxh / 255;
            if (hh < 1) hh = 1;
            lv_opa_t op = (x + BAR_W / 2 < cx) ? LV_OPA_60 : LV_OPA_COVER;
            fill(layer, x, mid - hh, x + BAR_W - 1, mid + hh, d->color, op);
            int32_t ch = hh * (d->cores ? d->cores[i] : 140) / 255;
            if (ch > 0) fill(layer, x, mid - ch, x + BAR_W - 1, mid + ch, d->core, op);
        }
    }
    fill(layer, cx - 1, a.y1, cx, a.y2, C_WHITE, LV_OPA_COVER);
}

static void overview_draw(lv_event_t *e)
{
    deck_t *d = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t n = w / OV_STEP;
    if (n > OV_MAX) n = OV_MAX;
    if (!d->peaks || !d->count || !d->len_ms) return;

    if (!d->ov_valid || d->ov_n != n) {
        for (int32_t k = 0; k < n; k++) {
            uint32_t i0 = (uint32_t)((uint64_t)k * d->count / n), i1 = (uint32_t)((uint64_t)(k + 1) * d->count / n);
            if (i1 <= i0) i1 = i0 + 1;
            uint8_t m = 0;
            for (uint32_t i = i0; i < i1 && i < d->count; i++) if (d->peaks[i] > m) m = d->peaks[i];
            d->ov[k] = m;
        }
        d->ov_n = n; d->ov_valid = true;
    }

    int32_t px = a.x1 + (int32_t)((uint64_t)(d->pos_ms > d->len_ms ? d->len_ms : d->pos_ms) * w / d->len_ms);
    int32_t base = a.y2 - 3, maxh = h - 6;
    for (int32_t k = 0; k < n; k++) {
        int32_t x = a.x1 + k * OV_STEP;
        int32_t hh = d->ov[k] * maxh / 255;
        if (hh < 1) hh = 1;
        fill(layer, x, base - hh + 1, x + OV_STEP - 2, base, d->color, x < px ? LV_OPA_40 : LV_OPA_COVER);
    }
    for (int k = 0; k < DJ_HOTCUES; k++) {
        if (!d->cue_set[k]) continue;
        int32_t x = a.x1 + (int32_t)((uint64_t)d->cue_ms[k] * w / d->len_ms);
        fill(layer, x - 2, a.y1, x + 1, a.y1 + 3, lv_color_hex(CUE_COL[d->cue_col[k]]), LV_OPA_COVER);
    }
    fill(layer, px, a.y1, px + 1, a.y2, C_WHITE, LV_OPA_COVER);
}

/* ---------------------------------------------------------------- refresh */

static void refresh_bpm(deck_t *d)
{
    char b[16];
    snprintf(b, sizeof b, "%.1f", (double)(d->bpm * (1.f + d->tempo / 100.f)));
    lv_label_set_text(d->info_bpm, b);
    lv_label_set_text(d->ft_bpm, b);
    snprintf(b, sizeof b, "%+.1f%%", (double)d->tempo);
    lv_label_set_text(d->info_tempo, b);
    lv_label_set_text(d->ft_tempo, b);
}

static void refresh_hc(void)
{
    deck_t *d = &g.deck[g.target];
    char b[16];
    for (int k = 0; k < DJ_HOTCUES; k++) {
        if (d->cue_set[k]) {
            lv_color_t c = lv_color_hex(CUE_COL[d->cue_col[k]]);
            lv_obj_set_style_bg_color(g.hc_card[k], c, 0);
            lv_obj_set_style_border_color(g.hc_card[k], c, 0);
            lv_obj_set_style_text_color(g.hc_name[k], C_DARK, 0);
            lv_obj_set_style_text_color(g.hc_val[k], C_DARK, 0);
            fmt_hms(b, sizeof b, d->cue_ms[k]);
            lv_label_set_text(g.hc_val[k], b);
        } else {
            lv_obj_set_style_bg_color(g.hc_card[k], C_PANEL, 0);
            lv_obj_set_style_border_color(g.hc_card[k], C_LINE, 0);
            lv_obj_set_style_text_color(g.hc_name[k], C_INK, 0);
            lv_obj_set_style_text_color(g.hc_val[k], C_MUTED, 0);
            lv_label_set_text(g.hc_val[k], "EMPTY");
        }
    }
    for (int i = 0; i < DJ_DECKS; i++) {
        if (i == g.target) btn_style(g.tgt_btn[i], g.deck[i].color, C_DARK, g.deck[i].color);
        else btn_style(g.tgt_btn[i], C_PANEL, C_INK, C_LINE);
    }
}

static void refresh_cues(uint8_t deck)
{
    deck_t *d = &g.deck[deck];
    for (int k = 0; k < DJ_HOTCUES; k++) {
        if (d->cue_set[k]) {
            lv_color_t c = lv_color_hex(CUE_COL[d->cue_col[k]]);
            btn_style(d->pad[k], c, C_DARK, c);
        } else {
            btn_style(d->pad[k], C_PANEL, C_DIM, C_LINE);
        }
    }
    lv_obj_invalidate(d->ov);
    if (deck == g.target) refresh_hc();
}

static void refresh_rows(void)
{
    for (int r = 0; r < DJ_LIB_ROWS; r++) {
        lv_color_t bg = C_WAVEBG, fg = C_INK;
        if (r == g.sel) bg = C_LINE;
        if (r == g.loaded[0]) { bg = g.deck[0].color; fg = C_DARK; }
        if (r == g.loaded[1]) { bg = g.deck[1].color; fg = C_DARK; }
        lv_obj_set_style_bg_color(g.row[r], bg, 0);
        for (int c = 0; c < 5; c++) lv_obj_set_style_text_color(g.row_lbl[r][c], fg, 0);
    }
}

static void refresh_sorts(void)
{
    for (int k = 0; k < 4; k++) {
        if (g.sort == (dj_sort_t)(k + 1)) btn_style(g.sort_btn[k], C_INK, C_DARK, C_INK);
        else btn_style(g.sort_btn[k], C_PANEL, C_INK, C_LINE);
    }
}

/* ---------------------------------------------------------------- events */

static void tab_click(lv_event_t *e)
{
    dj_ui_show_tab((dj_tab_t)IDX(e));
    if (g.cb.on_tab) g.cb.on_tab((dj_tab_t)IDX(e));
}

static void pad_click(lv_event_t *e)
{
    int v = IDX(e);
    if (g.cb.on_hotcue) g.cb.on_hotcue((uint8_t)(v / 8), (uint8_t)(v % 8));
}

static void hc_click(lv_event_t *e)
{
    if (g.cb.on_hotcue) g.cb.on_hotcue(g.target, (uint8_t)IDX(e));
}

static void tgt_click(lv_event_t *e)
{
    g.target = (uint8_t)IDX(e);
    refresh_hc();
}

static void fx_beat_click(lv_event_t *e) { if (g.cb.on_fx_beat) g.cb.on_fx_beat((uint8_t)IDX(e)); }
static void fx_on_click(lv_event_t *e) { (void)e; if (g.cb.on_fx_toggle) g.cb.on_fx_toggle(); }

static void row_click(lv_event_t *e)
{
    g.sel = (int8_t)IDX(e);
    refresh_rows();
    if (g.cb.on_lib_select) g.cb.on_lib_select((uint8_t)g.sel);
}

static void load_click(lv_event_t *e)
{
    if (g.sel >= 0 && g.cb.on_lib_load) g.cb.on_lib_load((uint8_t)IDX(e), (uint8_t)g.sel);
}

static void sort_click(lv_event_t *e)
{
    dj_sort_t s = (dj_sort_t)IDX(e);
    g.sort = (g.sort == s) ? DJ_SORT_NONE : s;
    refresh_sorts();
    if (g.cb.on_lib_sort) g.cb.on_lib_sort(g.sort);
}

static void page_click(lv_event_t *e) { if (g.cb.on_lib_page) g.cb.on_lib_page((int8_t)IDX(e)); }

static void bright_cb(lv_event_t *e)
{
    (void)e;
    int32_t v = lv_slider_get_value(g.bright);
    lv_label_set_text_fmt(g.bright_lbl, "%d%%", (int)v);
    if (g.cb.on_brightness) g.cb.on_brightness((uint8_t)v);
}

static void wl_cb(lv_event_t *e)
{
    (void)e;
    bool on = lv_obj_has_state(g.wl_sw, LV_STATE_CHECKED);
    lv_label_set_text(g.wl_lbl, on ? "P4 REMOTE: ON" : "P4 REMOTE: OFF");
    if (g.cb.on_wireless) g.cb.on_wireless(on);
}

/* ---------------------------------------------------------------- screens */

static void build_overview(lv_obj_t *pg)
{
    for (int i = 0; i < DJ_DECKS; i++) {
        deck_t *d = &g.deck[i];
        int32_t y = i * 149, x0 = i * 507;
        char num[4], deck_lbl[12];
        snprintf(num, sizeof num, "%d", i + 1);
        snprintf(deck_lbl, sizeof deck_lbl, "DECK %d", i + 1);

        /* info card */
        lv_obj_t *c = box(pg, 0, y, 148, 143, C_PANEL, C_LINE);
        strip(c, 141, d->color);
        cap(c, deck_lbl, 10, 8);
        d->info_trk = txt_r(c, F12, C_MUTED, "--/--", -8, 8);
        d->info_src = txt(c, F16, C_INK, "-", 10, 26);
        cap(c, "KEY", 10, 60);
        d->info_key = txt(c, F20, d->color, "--", 10, 76);
        cap(c, "TEMPO", 72, 60);
        d->info_tempo = txt(c, F20, C_INK, "+0.0%", 72, 76);
        d->info_bpm = txt(c, F28, C_INK, "---.-", 10, 104);
        txt(c, F12, C_MUTED, "BPM", 104, 118);

        /* zoomed waveform */
        d->zoom = box(pg, 154, y, 698, 143, C_WAVEBG, C_LINE);
        lv_obj_add_event_cb(d->zoom, zoom_draw, LV_EVENT_DRAW_MAIN_END, d);
        lv_obj_set_style_text_letter_space(txt(d->zoom, F12, d->color, num, 8, 6), 1, 0);
        d->zoom_time = txt_r(d->zoom, F12, C_MUTED, "00:00.0", -8, 6);

        /* hot cue pads */
        for (int k = 0; k < DJ_HOTCUES; k++) {
            char l[2] = { (char)('A' + k), 0 };
            d->pad[k] = btn(pg, x0 + k * 63, 298, 59, 34, l, F14, C_PANEL, C_DIM, C_LINE, pad_click, UD(i * 8 + k));
        }

        /* footer */
        lv_obj_t *f = box(pg, x0, 338, 501, 208, C_PANEL, C_LINE);
        lv_obj_t *badge = box(f, 10, 8, 22, 22, d->color, d->color);
        lv_obj_set_style_radius(badge, 3, 0);
        lv_obj_center(txt(badge, F14, C_DARK, num, 0, 0));
        d->ft_title = txt(f, F16, C_INK, "-", 40, 6);
        lv_label_set_long_mode(d->ft_title, DJ_LONG_DOT);
        lv_obj_set_width(d->ft_title, 400);
        d->ft_artist = txt(f, F12, C_MUTED, "", 40, 26);
        lv_label_set_long_mode(d->ft_artist, DJ_LONG_DOT);
        lv_obj_set_width(d->ft_artist, 400);
        lv_obj_t *art = box(f, 453, 6, 36, 36, C_LINE, C_BORDER2);
        lv_obj_set_style_radius(art, 3, 0);
        d->ft_art_lbl = txt(art, F12, C_MUTED, "ART", 0, 0);
        lv_obj_center(d->ft_art_lbl);
        d->ft_art = lv_image_create(art);
        lv_obj_set_pos(d->ft_art, 0, 0);
        lv_obj_set_size(d->ft_art, 34, 34);
        lv_obj_add_flag(d->ft_art, LV_OBJ_FLAG_HIDDEN);

        cap(f, "REMAIN", 10, 50);
        d->ft_remain = txt(f, F32, C_INK, "00:00.0", 10, 64);
        cap(f, "TEMPO", 210, 50);
        d->ft_tempo = txt(f, F16, C_INK, "+0.0%", 210, 72);
        d->ft_bpm_cap = txt_r(f, F12, C_MUTED, "BPM", -10, 50);
        lv_obj_set_style_text_letter_space(d->ft_bpm_cap, 1, 0);
        d->ft_bpm = txt_r(f, F28, d->color, "---.-", -10, 64);

        d->ov = plain(f, 10, 108, 479, 90);
        lv_obj_set_style_bg_color(d->ov, C_WAVEBG, 0);
        lv_obj_set_style_bg_opa(d->ov, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(d->ov, 3, 0);
        lv_obj_add_event_cb(d->ov, overview_draw, LV_EVENT_DRAW_MAIN_END, d);
    }

    /* FX panel */
    lv_obj_t *fx = box(pg, 858, 0, 150, 292, C_PANEL, C_LINE);
    cap(fx, "FX", 10, 10);
    g.fx_ch = box(fx, 94, 8, 44, 20, C_LINE, C_LINE);
    lv_obj_set_style_radius(g.fx_ch, 3, 0);
    lv_obj_center(txt(g.fx_ch, F12, C_INK, "CH 1", 0, 0));
    g.fx_name = txt(fx, F24, C_INK, "-", 10, 34);
    cap(fx, "BEAT", 10, 72);
    static const char *beats[4] = { "1/4", "1/2", "1", "2" };
    for (int k = 0; k < 4; k++)
        g.fx_beat[k] = btn(fx, 10 + k * 33, 88, 29, 26, beats[k], F12, C_LINE, C_INK, C_LINE, fx_beat_click, UD(k));
    cap(fx, "TIME", 10, 126);
    g.fx_time = txt(fx, F20, C_INK, "- ms", 10, 140);
    cap(fx, "LEVEL", 10, 176);
    g.fx_level_lbl = txt_r(fx, F12, C_INK, "0%", -10, 176);
    g.fx_level = lv_bar_create(fx);
    lv_obj_set_pos(g.fx_level, 10, 194);
    lv_obj_set_size(g.fx_level, 128, 8);
    lv_bar_set_range(g.fx_level, 0, 100);
    lv_obj_set_style_bg_color(g.fx_level, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g.fx_level, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g.fx_level, C_INK, LV_PART_INDICATOR);
    g.fx_on = btn(fx, 10, 244, 128, 38, "ON", F16, C_PANEL, C_INK, C_LINE, fx_on_click, NULL);
}

static void build_library(lv_obj_t *pg)
{
    static const char *hdr[5] = { "TITLE", "ARTIST", "KEY", "BPM", "TIME" };
    static const int32_t cx[5] = { 14, 446, 648, 712, 776 }, cw[5] = { 420, 190, 52, 52, 60 };

    lv_obj_t *t = box(pg, 0, 0, 852, 500, C_WAVEBG, C_LINE);
    for (int c = 0; c < 5; c++) {
        lv_obj_t *l = cap(t, hdr[c], cx[c], 9);
        if (c >= 2) { lv_obj_set_width(l, cw[c]); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0); }
    }
    lv_obj_t *sep = strip(t, 1, C_LINE);
    lv_obj_set_pos(sep, 0, 30);
    lv_obj_set_width(sep, 850);

    for (int r = 0; r < DJ_LIB_ROWS; r++) {
        lv_obj_t *row = box(t, 0, 31 + r * 52, 850, 52, C_WAVEBG, C_ROWLINE);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row, row_click, LV_EVENT_CLICKED, UD(r));
        for (int c = 0; c < 5; c++) {
            lv_obj_t *l = txt(row, c < 2 ? F16 : F14, C_INK, "", 0, 0);
            lv_label_set_long_mode(l, DJ_LONG_DOT);
            lv_obj_set_width(l, cw[c]);
            if (c >= 2) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, cx[c], 0);
            g.row_lbl[r][c] = l;
        }
        g.row[r] = row;
    }

    btn(pg, 0, 506, 100, 40, "PREV", F14, C_PANEL, C_INK, C_LINE, page_click, UD(-1));
    g.lib_info = txt(pg, F12, C_MUTED, "", 106, 519);
    lv_obj_set_width(g.lib_info, 640);
    lv_obj_set_style_text_align(g.lib_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(g.lib_info, 1, 0);
    btn(pg, 752, 506, 100, 40, "NEXT", F14, C_PANEL, C_INK, C_LINE, page_click, UD(1));

    lv_obj_t *dc = box(pg, 858, 0, 150, 70, C_PANEL, C_LINE);
    g.lib_strip = strip(dc, 68, g.deck[0].color);
    g.lib_deck = txt(dc, F24, C_INK, "DECK 1", 0, 0);
    lv_obj_align(g.lib_deck, LV_ALIGN_TOP_MID, 0, 10);
    g.lib_status = txt(dc, F12, g.deck[0].color, "READY", 0, 0);
    lv_obj_set_style_text_letter_space(g.lib_status, 1, 0);
    lv_obj_align(g.lib_status, LV_ALIGN_TOP_MID, 0, 44);

    btn(pg, 858, 76, 150, 44, "LOAD DECK 1", F14, g.deck[0].color, C_DARK, g.deck[0].color, load_click, UD(0));
    btn(pg, 858, 126, 150, 44, "LOAD DECK 2", F14, g.deck[1].color, C_DARK, g.deck[1].color, load_click, UD(1));
    static const char *sorts[4] = { "SORT ARTIST", "SORT NAME", "SORT BPM", "SORT KEY" };
    for (int k = 0; k < 4; k++)
        g.sort_btn[k] = btn(pg, 858, 184 + k * 50, 150, 44, sorts[k], F12, C_PANEL, C_INK, C_LINE, sort_click, UD(k + 1));
}

static void build_hotcues(lv_obj_t *pg)
{
    cap(pg, "TARGET", 414, 13);
    g.tgt_btn[0] = btn(pg, 478, 2, 64, 36, "D1", F14, C_PANEL, C_INK, C_LINE, tgt_click, UD(0));
    g.tgt_btn[1] = btn(pg, 546, 2, 64, 36, "D2", F14, C_PANEL, C_INK, C_LINE, tgt_click, UD(1));

    for (int k = 0; k < DJ_HOTCUES; k++) {
        char name[8];
        snprintf(name, sizeof name, "CUE %c", 'A' + k);
        lv_obj_t *c = box(pg, (k % 4) * 254, 48 + (k / 4) * 222, 244, 212, C_PANEL, C_LINE);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(c, C_INK, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, hc_click, LV_EVENT_CLICKED, UD(k));
        g.hc_name[k] = txt(c, F20, C_INK, name, 14, 12);
        g.hc_val[k] = txt(c, F14, C_MUTED, "EMPTY", 0, 0);
        lv_obj_align(g.hc_val[k], LV_ALIGN_BOTTOM_RIGHT, -14, -12);
        g.hc_card[k] = c;
    }

    lv_obj_t *sb = box(pg, 0, 490, 1008, 56, C_PANEL, C_LINE);
    cap(sb, "HOT CUE STATUS", 12, 20);
    lv_obj_t *row = flex_row(sb, 150, 0, 840, 54);
    chip(row, DJ_F_HC_CUES, "CUE A-H", DJ_TONE_OK);
    chip(row, DJ_F_HC_LOOPS, "LOOP CUES", DJ_TONE_WARN);
    chip(row, DJ_F_HC_ANLZ, "ANLZ DATA", DJ_TONE_INFO);
    chip(row, DJ_F_HC_TARGET, "D1/D2 TARGET", DJ_TONE_NORMAL);
}

static void build_settings(lv_obj_t *pg)
{
    lv_color_t acc = g.deck[0].color;
    lv_obj_t *b;

    b = box(pg, 0, 0, 501, 86, C_PANEL, C_LINE);
    cap(b, "DISPLAY", 14, 12);
    g.bright = lv_slider_create(b);
    lv_obj_set_pos(g.bright, 24, 50);
    lv_obj_set_size(g.bright, 380, 8);
    lv_slider_set_range(g.bright, 5, 100);
    lv_slider_set_value(g.bright, 78, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g.bright, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g.bright, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g.bright, acc, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g.bright, C_INK, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g.bright, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(g.bright, 16);
    lv_obj_add_event_cb(g.bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    g.bright_lbl = txt_r(b, F16, C_INK, "78%", -14, 43);

    b = box(pg, 0, 92, 501, 90, C_PANEL, C_LINE);
    cap(b, "MASTER OUTPUT", 14, 12);
    field_box(b, DJ_F_MASTER, 14, 36, 160, 40, "MASTER: 0 dB", DJ_TONE_NORMAL);
    field_txt(b, DJ_F_MASTER_HINT, F14, 188, 47, 296, "Lower if limiter stays active", DJ_TONE_MUTED);

    b = box(pg, 0, 188, 501, 100, C_PANEL, C_LINE);
    cap(b, "OUTPUT", 14, 12);
    field_box(b, DJ_F_OUT_MAIN, 14, 34, 230, 36, "MAIN: USB (DDJ)", DJ_TONE_INFO);
    field_box(b, DJ_F_UI_RENDER, 256, 34, 230, 36, "UI render: normal", DJ_TONE_NORMAL);
    field_txt(b, DJ_F_OUT_CUE, F12, 14, 76, 230, "CUE: DDJ-400", DJ_TONE_INFO);
    field_txt(b, DJ_F_LOCAL, F12, 256, 76, 230, "LOCAL: disabled", DJ_TONE_MUTED);

    b = box(pg, 507, 0, 501, 182, C_PANEL, C_LINE);
    cap(b, "SYSTEM STATUS", 14, 12);
    field_txt(b, DJ_F_SYS_CONTROLLER, F14, 14, 34, 470, "Controller (USB1): Connected", DJ_TONE_OK);
    txt(b, F14, C_INK, "SD Card", 14, 62);
    field_txt(b, DJ_F_SYS_SD, F12, 14, 82, 470, "Mounted: 0.0 MB free / 0.0 MB", DJ_TONE_OK);
    field_txt(b, DJ_F_SYS_SD_LOG, F12, 14, 100, 470, "SD Log: OK  95KB  drop 0", DJ_TONE_MUTED);
    field_txt(b, DJ_F_SYS_FW, F12, 14, 126, 470, "P4: 245 [ota_0]", DJ_TONE_OK);
    field_txt(b, DJ_F_SYS_RESET, F14, 14, 150, 470, "Last reset: Power-on", DJ_TONE_MUTED);

    b = box(pg, 507, 188, 501, 100, C_PANEL, C_LINE);
    cap(b, "WIRELESS", 14, 12);
    g.wl_sw = lv_switch_create(b);
    lv_obj_set_pos(g.wl_sw, 14, 40);
    lv_obj_set_size(g.wl_sw, 56, 30);
    lv_obj_set_style_bg_color(g.wl_sw, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g.wl_sw, acc, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(g.wl_sw, C_INK, LV_PART_KNOB);
    lv_obj_add_event_cb(g.wl_sw, wl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    g.wl_lbl = txt(b, F14, C_INK, "P4 REMOTE: OFF", 84, 46);

    b = box(pg, 0, 294, 1008, 84, C_PANEL, C_LINE);
    cap(b, "MIXER STATUS", 14, 12);
    lv_obj_t *row = flex_row(b, 14, 34, 978, 40);
    chip(row, DJ_F_MIX_MIXER, "MIXER: DDJ-400", DJ_TONE_NORMAL);
    chip(row, DJ_F_MIX_FADERS, "CH FADERS", DJ_TONE_INFO);
    chip(row, DJ_F_MIX_XFADER, "CROSSFADER", DJ_TONE_NORMAL);
    chip(row, DJ_F_MIX_PFL, "PFL D1/D2", DJ_TONE_WARN);
    lv_obj_t *sp = plain(row, 0, 0, 1, 1);
    lv_obj_set_flex_grow(sp, 1);
    chip(row, DJ_F_MIX_CUE, "CUE: STEREO", DJ_TONE_NORMAL);
}

/* ---------------------------------------------------------------- public API */

void dj_ui_create(lv_obj_t *parent)
{
    memset(&g, 0, sizeof g);
    g.grid = true; g.sel = -1; g.loaded[0] = g.loaded[1] = -1;
    for (int i = 0; i < DJ_DECKS; i++) {
        g.deck[i].color = lv_color_hex(DECK_COL[i]);
        g.deck[i].core = lv_color_hex(DECK_CORE[i]);
        g.deck[i].last_tenth = -1;
    }

    lv_obj_set_style_bg_color(parent, C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) g.page[i] = plain(parent, 8, 46, 1008, 546);
    build_overview(g.page[0]);
    build_library(g.page[1]);
    build_hotcues(g.page[2]);
    build_settings(g.page[3]);

    static const char *names[4] = { "OVERVIEW", "LIBRARY", "HOT CUES", "SETTINGS" };
    for (int i = 0; i < 4; i++)
        g.tab_btn[i] = btn(parent, 8 + i * 253, 8, 249, 32, names[i], F14, C_PANEL, C_INK, C_LINE, tab_click, UD(i));

    for (int i = 0; i < DJ_DECKS; i++) refresh_cues((uint8_t)i);
    refresh_hc();
    refresh_sorts();
    dj_ui_set_fx("-", 1, 1, 0, 0, false);
    dj_ui_show_tab(DJ_TAB_OVERVIEW);
}

void dj_ui_set_callbacks(const dj_ui_callbacks_t *cb) { g.cb = *cb; }

void dj_ui_show_tab(dj_tab_t tab)
{
    for (int i = 0; i < 4; i++) {
        if (i == (int)tab) {
            lv_obj_remove_flag(g.page[i], LV_OBJ_FLAG_HIDDEN);
            btn_style(g.tab_btn[i], C_INK, C_DARK, C_INK);
        } else {
            lv_obj_add_flag(g.page[i], LV_OBJ_FLAG_HIDDEN);
            btn_style(g.tab_btn[i], C_PANEL, C_INK, C_LINE);
        }
    }
}

void dj_ui_set_track(uint8_t deck, const char *title, const char *artist, const char *source,
                     uint16_t track_no, uint16_t track_count, uint32_t len_ms)
{
    if (deck >= DJ_DECKS) return;
    deck_t *d = &g.deck[deck];
    lv_label_set_text(d->ft_title, title ? title : "");
    lv_label_set_text(d->ft_artist, artist ? artist : "");
    lv_label_set_text(d->info_src, source ? source : "");
    lv_label_set_text_fmt(d->info_trk, "%02u/%u", (unsigned)track_no, (unsigned)track_count);
    d->len_ms = len_ms;
    d->ov_valid = false;
    d->last_tenth = -1;
    dj_ui_set_position(deck, d->pos_ms);
}

void dj_ui_set_key(uint8_t deck, const char *key)
{
    if (deck >= DJ_DECKS) return;
    lv_label_set_text(g.deck[deck].info_key, key);
    lv_label_set_text_fmt(g.deck[deck].ft_bpm_cap, "BPM / %s", key);
}

void dj_ui_set_bpm(uint8_t deck, float track_bpm, uint32_t first_beat_ms)
{
    if (deck >= DJ_DECKS) return;
    g.deck[deck].bpm = track_bpm;
    g.deck[deck].first_beat_ms = first_beat_ms;
    refresh_bpm(&g.deck[deck]);
    lv_obj_invalidate(g.deck[deck].zoom);
}

void dj_ui_set_tempo(uint8_t deck, float percent)
{
    if (deck >= DJ_DECKS) return;
    g.deck[deck].tempo = percent;
    refresh_bpm(&g.deck[deck]);
}

void dj_ui_set_position(uint8_t deck, uint32_t pos_ms)
{
    if (deck >= DJ_DECKS) return;
    deck_t *d = &g.deck[deck];
    d->pos_ms = pos_ms;
    int32_t tenth = (int32_t)(pos_ms / 100);
    if (tenth != d->last_tenth) {
        char b[16];
        d->last_tenth = tenth;
        fmt_mmss(b, sizeof b, pos_ms);
        lv_label_set_text(d->zoom_time, b);
        fmt_mmss(b, sizeof b, d->len_ms > pos_ms ? d->len_ms - pos_ms : 0);
        lv_label_set_text(d->ft_remain, b);
    }
    lv_obj_invalidate(d->zoom);
    lv_obj_invalidate(d->ov);
}

void dj_ui_set_waveform(uint8_t deck, const uint8_t *peaks, const uint8_t *cores, uint32_t count, uint16_t pps)
{
    if (deck >= DJ_DECKS) return;
    deck_t *d = &g.deck[deck];
    d->peaks = peaks; d->cores = cores; d->count = count; d->pps = pps;
    d->ov_valid = false;
    lv_obj_invalidate(d->zoom);
    lv_obj_invalidate(d->ov);
}

void dj_ui_set_hotcue(uint8_t deck, uint8_t index, bool set, uint32_t pos_ms, uint8_t color)
{
    if (deck >= DJ_DECKS || index >= DJ_HOTCUES) return;
    deck_t *d = &g.deck[deck];
    d->cue_set[index] = set;
    d->cue_ms[index] = pos_ms;
    d->cue_col[index] = color & 7;
    refresh_cues(deck);
}

void dj_ui_set_artwork(uint8_t deck, const void *src)
{
    if (deck >= DJ_DECKS) return;
    deck_t *d = &g.deck[deck];
    if (src) {
        lv_image_set_src(d->ft_art, src);
        lv_obj_remove_flag(d->ft_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(d->ft_art_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d->ft_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(d->ft_art_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void dj_ui_set_beat_grid_visible(bool visible)
{
    g.grid = visible;
    for (int i = 0; i < DJ_DECKS; i++) lv_obj_invalidate(g.deck[i].zoom);
}

void dj_ui_set_fx(const char *name, uint8_t channel, uint8_t beat_index, uint16_t time_ms, uint8_t level, bool on)
{
    lv_label_set_text(g.fx_name, name);
    lv_label_set_text_fmt(lv_obj_get_child(g.fx_ch, 0), "CH %u", (unsigned)channel);
    for (int k = 0; k < 4; k++) {
        if (k == beat_index) btn_style(g.fx_beat[k], C_INK, C_DARK, C_INK);
        else btn_style(g.fx_beat[k], C_LINE, C_INK, C_LINE);
    }
    lv_label_set_text_fmt(g.fx_time, "%u ms", (unsigned)time_ms);
    if (level > 100) level = 100;
    lv_bar_set_value(g.fx_level, level, LV_ANIM_OFF);
    lv_label_set_text_fmt(g.fx_level_lbl, "%u%%", (unsigned)level);
    if (on) btn_style(g.fx_on, C_INK, C_DARK, C_INK);
    else btn_style(g.fx_on, C_PANEL, C_MUTED, C_LINE);
    lv_label_set_text(lv_obj_get_child(g.fx_on, 0), on ? "ON" : "OFF");
}

void dj_ui_library_set_rows(const dj_track_t *rows, uint8_t count)
{
    if (count > DJ_LIB_ROWS) count = DJ_LIB_ROWS;
    for (int r = 0; r < DJ_LIB_ROWS; r++) {
        if (r >= count) { lv_obj_add_flag(g.row[r], LV_OBJ_FLAG_HIDDEN); continue; }
        const dj_track_t *t = &rows[r];
        lv_obj_remove_flag(g.row[r], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g.row_lbl[r][0], t->title ? t->title : "");
        lv_label_set_text(g.row_lbl[r][1], t->artist ? t->artist : "");
        lv_label_set_text(g.row_lbl[r][2], t->key ? t->key : "");
        lv_label_set_text_fmt(g.row_lbl[r][3], "%d", (int)(t->bpm + 0.5f));
        lv_label_set_text_fmt(g.row_lbl[r][4], "%lu:%02lu", (unsigned long)(t->len_ms / 60000),
                              (unsigned long)(t->len_ms / 1000 % 60));
    }
    g.rows = count;
    if (g.sel >= count) g.sel = -1;
    refresh_rows();
}

void dj_ui_library_set_loaded(uint8_t deck, int8_t row)
{
    if (deck >= DJ_DECKS) return;
    g.loaded[deck] = row;
    refresh_rows();
}

void dj_ui_library_set_info(const char *source, uint16_t total, uint16_t page, uint16_t pages)
{
    lv_label_set_text_fmt(g.lib_info, "%s     %u TRACKS     PAGE %u/%u", source, (unsigned)total,
                          (unsigned)page, (unsigned)pages);
}

void dj_ui_library_set_deck_status(uint8_t deck, const char *status)
{
    if (deck >= DJ_DECKS) return;
    lv_label_set_text_fmt(g.lib_deck, "DECK %u", (unsigned)(deck + 1));
    lv_label_set_text(g.lib_status, status);
    lv_obj_set_style_text_color(g.lib_status, g.deck[deck].color, 0);
    lv_obj_set_style_bg_color(g.lib_strip, g.deck[deck].color, 0);
}

void dj_ui_set_field(dj_field_t field, const char *text, dj_tone_t tone)
{
    if (field >= DJ_F_COUNT || !g.f_lbl[field]) return;
    if (text) lv_label_set_text(g.f_lbl[field], text);
    apply_tone(field, tone);
}

void dj_ui_set_brightness(uint8_t pct)
{
    lv_slider_set_value(g.bright, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(g.bright_lbl, "%u%%", (unsigned)lv_slider_get_value(g.bright));
}

void dj_ui_set_wireless(bool on)
{
    if (on) lv_obj_add_state(g.wl_sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(g.wl_sw, LV_STATE_CHECKED);
    lv_label_set_text(g.wl_lbl, on ? "P4 REMOTE: ON" : "P4 REMOTE: OFF");
}
