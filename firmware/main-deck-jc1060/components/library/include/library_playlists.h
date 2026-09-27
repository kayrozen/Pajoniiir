#pragma once

/* JC1060: rekordbox playlists flattened for the Library browser.
 *
 * The PDB playlist tree (folders + playlists) becomes one list of playable
 * playlists in rekordbox order: depth-first, siblings by sort_order. Folders
 * are not rows; each playlist remembers its immediate folder name instead.
 * Entries keep the rekordbox order and duplicates; entries whose track is not
 * in the published catalog (truncated or unknown ids) are skipped and counted.
 *
 * Built once per catalog publication (library_init), never per frame. */

#include <stdint.h>
#include "rekordbox_pdb.h"      /* esp_err_t (stubbed in host tests) */

#define LIBRARY_PLAYLIST_NAME_MAX  64
#define LIBRARY_PLAYLIST_DEPTH_MAX  8

typedef struct {
    char     name[LIBRARY_PLAYLIST_NAME_MAX];
    char     folder[LIBRARY_PLAYLIST_NAME_MAX];  /* "" at the root */
    uint32_t first;                              /* into library_playlist_set_t.keys */
    uint16_t count;                              /* playable entries */
    uint16_t missing;                            /* entries skipped (not in catalog) */
} library_playlist_t;

typedef struct {
    library_playlist_t *lists;
    int                 list_count;
    uint32_t           *keys;                    /* catalog track keys (= track_id) */
    int                 key_count;
} library_playlist_set_t;

/* `known_ids` is the published catalog's track_id set, sorted ascending.
 * Allocates in PSRAM; on failure `out` is left empty. */
esp_err_t library_playlists_build(const pdb_t *pdb,
                                  const uint32_t *known_ids, int known_count,
                                  library_playlist_set_t *out);
void library_playlists_free(library_playlist_set_t *set);
