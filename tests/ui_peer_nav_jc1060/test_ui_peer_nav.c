/* v311: the DJ Link playlist levels of the Library (ui_peer_nav): all
 * tracks -> playlist root -> folders -> a playlist, the button label of
 * each level, and the selection given back on the way out. */
#include "ui_peer_nav.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_walk_and_back(void)
{
    ui_peer_nav_t nav;
    ui_peer_nav_reset(&nav);
    uint32_t id = 99;
    assert(ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_ALL && id == 0);
    assert(strcmp(ui_peer_nav_button_label(&nav), "PLAYLISTS") == 0);
    assert(strcmp(ui_peer_nav_name(&nav), "") == 0);

    /* Track rows cannot be opened from all tracks. */
    int32_t sel = 37;
    assert(!ui_peer_nav_open(&nav, UI_PEER_NAV_FOLDER, 5, "X", &sel) && sel == 37);

    /* PLAYLISTS: the root folder, selection on its first row. */
    ui_peer_nav_button(&nav, &sel);
    assert(sel == 0 && nav.depth == 1);
    assert(ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_FOLDER && id == 0);
    assert(strcmp(ui_peer_nav_name(&nav), "PLAYLISTS") == 0);
    assert(strcmp(ui_peer_nav_button_label(&nav), "ALL TRACKS") == 0);

    /* Row 3 is a folder, its row 1 a playlist. */
    sel = 3;
    assert(ui_peer_nav_open(&nav, UI_PEER_NAV_FOLDER, 12, "House", &sel) && sel == 0);
    assert(ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_FOLDER && id == 12);
    assert(strcmp(ui_peer_nav_button_label(&nav), "BACK") == 0);
    sel = 1;
    assert(ui_peer_nav_open(&nav, UI_PEER_NAV_PLAYLIST, 40, "Warm up", &sel) && sel == 0);
    assert(ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_PLAYLIST && id == 40);
    assert(strcmp(ui_peer_nav_name(&nav), "Warm up") == 0);
    /* Nothing opens from a playlist's tracks. */
    sel = 4;
    assert(!ui_peer_nav_open(&nav, UI_PEER_NAV_FOLDER, 7, "Y", &sel) && sel == 4);
    assert(!ui_peer_nav_open(&nav, UI_PEER_NAV_ALL, 7, "Y", &sel));

    /* BACK gives each level its selection back, then all tracks row 37. */
    ui_peer_nav_button(&nav, &sel);
    assert(sel == 1 && ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_FOLDER && id == 12);
    ui_peer_nav_button(&nav, &sel);
    assert(sel == 3 && ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_FOLDER && id == 0);
    ui_peer_nav_button(&nav, &sel);
    assert(sel == 37 && nav.depth == 0 && ui_peer_nav_kind(&nav, &id) == UI_PEER_NAV_ALL);
}

static void test_depth_cap_and_names(void)
{
    ui_peer_nav_t nav;
    ui_peer_nav_reset(&nav);
    int32_t sel = 0;
    ui_peer_nav_button(&nav, &sel);
    for (uint32_t i = 1; i < UI_PEER_NAV_MAX_DEPTH; i++) {
        sel = (int32_t)i;
        assert(ui_peer_nav_open(&nav, UI_PEER_NAV_FOLDER, i, "F", &sel));
    }
    assert(nav.depth == UI_PEER_NAV_MAX_DEPTH);
    sel = 5;
    assert(!ui_peer_nav_open(&nav, UI_PEER_NAV_PLAYLIST, 99, "P", &sel) && sel == 5);

    /* A long name is cut, a NULL one is empty. */
    ui_peer_nav_reset(&nav);
    ui_peer_nav_button(&nav, &sel);
    char longname[200];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    assert(ui_peer_nav_open(&nav, UI_PEER_NAV_PLAYLIST, 1, longname, &sel));
    assert(strlen(ui_peer_nav_name(&nav)) == UI_PEER_NAV_NAME_MAX - 1);
    ui_peer_nav_button(&nav, &sel);
    assert(ui_peer_nav_open(&nav, UI_PEER_NAV_FOLDER, 2, NULL, &sel));
    assert(strcmp(ui_peer_nav_name(&nav), "") == 0);

    /* NULL-safe. */
    ui_peer_nav_reset(NULL);
    ui_peer_nav_button(NULL, &sel);
    ui_peer_nav_button(&nav, NULL);
    assert(ui_peer_nav_kind(NULL, NULL) == UI_PEER_NAV_ALL);
    assert(strcmp(ui_peer_nav_button_label(NULL), "PLAYLISTS") == 0);
}

int main(void)
{
    test_walk_and_back();
    test_depth_cap_and_names();
    puts("ui_peer_nav_jc1060: all tests passed");
    return 0;
}
