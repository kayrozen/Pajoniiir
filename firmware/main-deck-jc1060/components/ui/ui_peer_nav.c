// v311: DJ Link playlist levels for the Library (see ui_peer_nav.h).
#include "ui_peer_nav.h"

#include <stdio.h>
#include <string.h>

void ui_peer_nav_reset(ui_peer_nav_t *nav)
{
    if (nav) {
        memset(nav, 0, sizeof(*nav));
    }
}

ui_peer_nav_kind_t ui_peer_nav_kind(const ui_peer_nav_t *nav, uint32_t *id)
{
    if (id) {
        *id = 0u;
    }
    if (!nav || nav->depth == 0u) {
        return UI_PEER_NAV_ALL;
    }
    const ui_peer_nav_level_t *top = &nav->level[nav->depth - 1u];
    if (id) {
        *id = top->id;
    }
    return (ui_peer_nav_kind_t)top->kind;
}

const char *ui_peer_nav_name(const ui_peer_nav_t *nav)
{
    return nav && nav->depth ? nav->level[nav->depth - 1u].name : "";
}

static bool push(ui_peer_nav_t *nav, ui_peer_nav_kind_t kind, uint32_t id, const char *name,
                 int32_t *selected)
{
    if (nav->depth >= UI_PEER_NAV_MAX_DEPTH) {
        return false;
    }
    ui_peer_nav_level_t *l = &nav->level[nav->depth++];
    l->kind = (uint8_t)kind;
    l->id = id;
    l->parent_selected = *selected;
    snprintf(l->name, sizeof(l->name), "%s", name ? name : "");
    *selected = 0;
    return true;
}

void ui_peer_nav_button(ui_peer_nav_t *nav, int32_t *selected)
{
    if (!nav || !selected) {
        return;
    }
    if (nav->depth == 0u) {
        (void)push(nav, UI_PEER_NAV_FOLDER, 0u, "PLAYLISTS", selected);
        return;
    }
    *selected = nav->level[--nav->depth].parent_selected;
    memset(&nav->level[nav->depth], 0, sizeof(nav->level[0]));
}

const char *ui_peer_nav_button_label(const ui_peer_nav_t *nav)
{
    if (!nav || nav->depth == 0u) {
        return "PLAYLISTS";
    }
    return nav->depth == 1u ? "ALL TRACKS" : "BACK";
}

bool ui_peer_nav_open(ui_peer_nav_t *nav, ui_peer_nav_kind_t kind, uint32_t id,
                      const char *name, int32_t *selected)
{
    if (!nav || !selected || nav->depth == 0u ||
        nav->level[nav->depth - 1u].kind != UI_PEER_NAV_FOLDER ||
        (kind != UI_PEER_NAV_FOLDER && kind != UI_PEER_NAV_PLAYLIST)) {
        return false;
    }
    return push(nav, kind, id, name, selected);
}
