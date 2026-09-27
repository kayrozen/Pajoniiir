// RGB565 palette of the scrolling main-waveform strip (ui_overview_wave_cache).
// Shared by the legacy PPA overview and the dj_ui bridge (UI migration phase 5)
// so both presentations burn the same colours. Index meanings match
// ui_waveform_palette_for_sample().
#pragma once

#include <stdint.h>

#define UI_RGB565(r, g, b) \
    (uint16_t)((((uint16_t)(r) & 0xF8u) << 8) | (((uint16_t)(g) & 0xFCu) << 3) | ((uint16_t)(b) >> 3))

/* "Punchy" waveform palette (2026-07-04): brighter cyan transients, true
 * white for transient tips, punchier blue/pink, stronger contrast. Kept in
 * sync with the two LVGL I8 canvas palettes (main WIN32 + mini) in
 * ui_create_overview_deck_panel. */
#define UI_OVERVIEW_WAVE_RGB565_PALETTE {                                   \
    UI_RGB565(0x00, 0x00, 0x00),  /* 0 background */                       \
    UI_RGB565(0xFF, 0x2E, 0x6E),  /* 1 hot pink/red body */                 \
    UI_RGB565(0x3A, 0x7B, 0xFF),  /* 2 blue mid energy */                   \
    UI_RGB565(0x26, 0xE0, 0xFF),  /* 3 bright cyan transient */             \
    UI_RGB565(0xFF, 0xFF, 0xFF),  /* 4 white (tips / center) */             \
    UI_RGB565(0x38, 0xF5, 0x8C),  /* 5 green */                             \
    UI_RGB565(0xFF, 0xB7, 0x33),  /* 6 amber */                             \
    UI_RGB565(0xB5, 0x7C, 0xFF),  /* 7 purple quiet detail */               \
    UI_RGB565(0x5A, 0x5D, 0x64),  /* 8 grey */                              \
    UI_RGB565(0xFF, 0x17, 0x44),  /* 9 red */                               \
    UI_RGB565(0x6B, 0x3F, 0x00),  /* 10 active-loop background (dim amber) */ \
    /* 11..18 hot-cue colours (slot 0..7); kept in sync with the mini       \
     * cue-line colours in ui_overview_update_cue_markers. */               \
    UI_RGB565(0x00, 0xE6, 0x76),  /* 11 cue 0 green */                      \
    UI_RGB565(0x00, 0xE5, 0xFF),  /* 12 cue 1 cyan */                       \
    UI_RGB565(0xFF, 0xAB, 0x00),  /* 13 cue 2 amber */                      \
    UI_RGB565(0xE0, 0x40, 0xFB),  /* 14 cue 3 magenta */                    \
    UI_RGB565(0xFF, 0xD6, 0x00),  /* 15 cue 4 yellow */                     \
    UI_RGB565(0xFF, 0x17, 0x44),  /* 16 cue 5 red */                        \
    UI_RGB565(0x7C, 0x4D, 0xFF),  /* 17 cue 6 purple */                     \
    UI_RGB565(0x29, 0x79, 0xFF),  /* 18 cue 7 blue */                       \
}
