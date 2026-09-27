// UTF-8 text fitting for the dj_ui presentation (UI migration phase 4).
//
// Pure C, no LVGL: host-tested in tests/ui_djui_text. Catalog tags, rekordbox
// PDB strings and DJ Link names (UTF-16 converted to UTF-8) reach dj_ui as
// UTF-8, while the fonts it draws with cover less (R1 in
// docs/UI_MIGRATION_PLAN_DJUI.md).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 once dj_ui draws with the Latin-1 + Latin Extended-A family (U+00A0..
 * U+017F, U+2013..U+2026): those code points then pass through unchanged.
 * 0 = built-in Montserrat, ASCII only: they fold to ASCII ("Été" -> "Ete"). */
#ifndef UI_DJUI_FONT_LATIN
#define UI_DJUI_FONT_LATIN 0
#endif

/* Copies src into dst (cap bytes, always terminated) as text the dj_ui fonts
 * can draw: foldable code points become ASCII, anything else and malformed
 * bytes become '?', control characters a space. Never splits a UTF-8
 * sequence; a sequence cut short at the end of src is dropped. The output is
 * never longer than the input, so dst may equal src. Returns strlen(dst). */
size_t ui_djui_text_fit(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif
