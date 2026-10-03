#pragma once
//
// v311: the levels of a DJ Link player's playlists, as the Library walks
// them: all tracks (depth 0), then the playlist root folder, sub-folders and
// a playlist's tracks, one level per open. Pure (no LVGL, no dj_link): the
// Library asks dj_link to list the level on top; host-tested.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PEER_NAV_MAX_DEPTH 10   /* root + nested folders + a playlist */
#define UI_PEER_NAV_NAME_MAX  48

typedef enum {
    UI_PEER_NAV_ALL = 0,        /* all tracks (the sortable list) */
    UI_PEER_NAV_FOLDER,         /* folders and playlists of folder `id` */
    UI_PEER_NAV_PLAYLIST,       /* the tracks of playlist `id` */
} ui_peer_nav_kind_t;

typedef struct {
    uint8_t  kind;              /* ui_peer_nav_kind_t */
    uint32_t id;
    int32_t  parent_selected;   /* selection to restore on the level below */
    char     name[UI_PEER_NAV_NAME_MAX];
} ui_peer_nav_level_t;

typedef struct {
    uint8_t depth;              /* 0 = all tracks */
    ui_peer_nav_level_t level[UI_PEER_NAV_MAX_DEPTH];
} ui_peer_nav_t;

void ui_peer_nav_reset(ui_peer_nav_t *nav);
/* The level shown: kind and id (ALL, 0 at depth 0). */
ui_peer_nav_kind_t ui_peer_nav_kind(const ui_peer_nav_t *nav, uint32_t *id);
/* Name of the level shown: "" at depth 0, "PLAYLISTS" at the root. */
const char *ui_peer_nav_name(const ui_peer_nav_t *nav);
/* PLAYLISTS button: depth 0 opens the playlist root, any other depth goes
 * one level back. *selected: the current selection in, the selection to
 * show out (0 on a new level, the saved one on the way back). */
void ui_peer_nav_button(ui_peer_nav_t *nav, int32_t *selected);
/* "PLAYLISTS" at depth 0, "ALL TRACKS" at the root, "BACK" deeper. */
const char *ui_peer_nav_button_label(const ui_peer_nav_t *nav);
/* Opens a folder or playlist row of the level shown (selected = its row).
 * False (nothing changes) at depth 0, past UI_PEER_NAV_MAX_DEPTH or for
 * another kind. */
bool ui_peer_nav_open(ui_peer_nav_t *nav, ui_peer_nav_kind_t kind, uint32_t id,
                      const char *name, int32_t *selected);

#ifdef __cplusplus
}
#endif
