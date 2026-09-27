#include "library_playlists.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef REKORDBOX_PDB_STANDALONE_TEST
#include "esp_heap_caps.h"
#endif

static void *playlists_alloc(size_t bytes)
{
    if (bytes == 0u) bytes = 1u;
#ifndef REKORDBOX_PDB_STANDALONE_TEST
    void *mem = heap_caps_calloc(1u, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return mem ? mem : calloc(1u, bytes);
#else
    return calloc(1u, bytes);
#endif
}

static void playlists_copy_str(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    while (src && i + 1u < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool known_id(const uint32_t *ids, int count, uint32_t id)
{
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ids[mid] == id) return true;
        if (ids[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

typedef struct {
    const pdb_t     *pdb;
    const uint32_t  *known_ids;
    int              known_count;
    pdb_playlist_t  *nodes;          /* PDB tree copy */
    int              node_count;
    bool            *visited;
    uint32_t        *ids;            /* scratch for pdb_playlist_track_ids */
    int              ids_cap;
    library_playlist_set_t *out;
    int              lists_cap;
    int              keys_cap;
} flatten_t;

static void emit_playlist(flatten_t *f, const pdb_playlist_t *node, const char *folder)
{
    if (f->out->list_count >= f->lists_cap) return;
    library_playlist_t *pl = &f->out->lists[f->out->list_count++];
    playlists_copy_str(pl->name, sizeof(pl->name), node->name[0] ? node->name : "Untitled");
    playlists_copy_str(pl->folder, sizeof(pl->folder), folder);
    pl->first = (uint32_t)f->out->key_count;

    int n = pdb_playlist_track_ids(f->pdb, node->id, f->ids, f->ids_cap);
    for (int i = 0; i < n; i++) {
        if (!known_id(f->known_ids, f->known_count, f->ids[i]) ||
            f->out->key_count >= f->keys_cap || pl->count == UINT16_MAX) {
            if (pl->missing < UINT16_MAX) pl->missing++;
            continue;
        }
        f->out->keys[f->out->key_count++] = f->ids[i];
        pl->count++;
    }
}

/* Children of `parent_id` in (sort_order, row) order; a selection scan per
 * child is O(n^2) over at most PDB_MAX_PLAYLISTS nodes, once per publish. */
static void emit_children(flatten_t *f, uint32_t parent_id, const char *folder, int depth)
{
    if (depth >= LIBRARY_PLAYLIST_DEPTH_MAX) return;
    for (;;) {
        int best = -1;
        for (int i = 0; i < f->node_count; i++) {
            if (f->visited[i] || f->nodes[i].parent_id != parent_id) continue;
            if (best < 0 || f->nodes[i].sort_order < f->nodes[best].sort_order) best = i;
        }
        if (best < 0) return;
        f->visited[best] = true;
        const pdb_playlist_t *node = &f->nodes[best];
        if (node->is_folder) {
            emit_children(f, node->id, node->name, depth + 1);
        } else {
            emit_playlist(f, node, folder);
        }
    }
}

void library_playlists_free(library_playlist_set_t *set)
{
    if (!set) return;
    free(set->lists);
    free(set->keys);
    memset(set, 0, sizeof(*set));
}

esp_err_t library_playlists_build(const pdb_t *pdb,
                                  const uint32_t *known_ids, int known_count,
                                  library_playlist_set_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    int node_count = pdb_playlist_count(pdb);
    if (node_count <= 0) return ESP_OK;

    int entry_total = 0;
    int playlist_nodes = 0;
    int ids_cap = 0;
    flatten_t f = {
        .pdb = pdb, .known_ids = known_ids, .known_count = known_ids ? known_count : 0,
        .node_count = node_count, .out = out,
    };
    f.nodes = (pdb_playlist_t *)playlists_alloc((size_t)node_count * sizeof(pdb_playlist_t));
    f.visited = (bool *)playlists_alloc((size_t)node_count * sizeof(bool));
    if (!f.nodes || !f.visited) goto oom;
    for (int i = 0; i < node_count; i++) {
        pdb_get_playlist(pdb, i, &f.nodes[i]);
        if (f.nodes[i].is_folder) continue;
        playlist_nodes++;
        int n = pdb_playlist_track_ids(pdb, f.nodes[i].id, NULL, 0);
        entry_total += n;
        if (n > ids_cap) ids_cap = n;
    }
    f.lists_cap = playlist_nodes;
    f.keys_cap = entry_total;
    f.ids_cap = ids_cap;
    out->lists = (library_playlist_t *)playlists_alloc((size_t)playlist_nodes * sizeof(library_playlist_t));
    out->keys = (uint32_t *)playlists_alloc((size_t)entry_total * sizeof(uint32_t));
    f.ids = (uint32_t *)playlists_alloc((size_t)ids_cap * sizeof(uint32_t));
    if (!out->lists || !out->keys || !f.ids) goto oom;

    emit_children(&f, 0u, "", 0);
    /* Orphans (parent missing, cycles, or deeper than the depth cap) still
     * show up, at the end, rather than silently disappearing. */
    for (int i = 0; i < node_count; i++) {
        if (!f.visited[i] && !f.nodes[i].is_folder) {
            f.visited[i] = true;
            emit_playlist(&f, &f.nodes[i], "");
        }
    }

    free(f.nodes);
    free(f.visited);
    free(f.ids);
    return ESP_OK;

oom:
    free(f.nodes);
    free(f.visited);
    free(f.ids);
    library_playlists_free(out);
    return ESP_ERR_NO_MEM;
}
