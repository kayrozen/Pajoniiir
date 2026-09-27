/* Host test: export.pdb PlaylistTree (0x07) / PlaylistEntries (0x08) parsing
 * and the Library flattening built on it (JC1060). The PDB is synthetic:
 * page 0 header, page 1 tree, pages 2 -> 3 entries (chained). */
#include "rekordbox_pdb.h"
#include "library_playlists.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 512u
#define PDB_PATH "test_pdb_playlists_jc1060.pdb"

static int s_failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_failures++; } } while (0)

static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* One data page being filled: heap grows up from 0x28, slots from the end. */
typedef struct {
    uint8_t *page;
    uint32_t heap;
    uint32_t nrows;
    uint16_t rowpf;
} page_writer_t;

static void page_begin(page_writer_t *w, uint8_t *page, uint32_t next)
{
    memset(page, 0, PAGE);
    w->page = page;
    w->heap = 0;
    w->nrows = 0;
    w->rowpf = 0;
    wr32(page + 0x0C, next);
}

/* Single row group (<= 16 rows): [end-2] tranrf, [end-4] rowpf, slots below. */
static void page_add_row(page_writer_t *w, const uint8_t *row, uint32_t len, int present)
{
    memcpy(w->page + 0x28 + w->heap, row, len);
    wr16(w->page + PAGE - 4 - 2 * (w->nrows + 1), (uint16_t)w->heap);
    if (present) w->rowpf |= (uint16_t)(1u << w->nrows);
    w->nrows++;
    w->heap += (len + 3u) & ~3u;
    wr32(w->page + 0x18, w->nrows);
    wr16(w->page + PAGE - 4, w->rowpf);
}

static void tree_row(page_writer_t *w, uint32_t parent, uint32_t sort, uint32_t id,
                     int folder, const char *name, int present)
{
    uint8_t row[0x14 + 128] = {0};
    size_t n = strlen(name);
    wr32(row + 0x00, parent);
    wr32(row + 0x08, sort);
    wr32(row + 0x0C, id);
    wr32(row + 0x10, folder ? 1u : 0u);
    row[0x14] = (uint8_t)(((n + 1u) << 1) | 1u);    /* short DeviceSQL string */
    memcpy(row + 0x15, name, n);
    page_add_row(w, row, (uint32_t)(0x15 + n), present);
}

static void entry_row(page_writer_t *w, uint32_t index, uint32_t track, uint32_t list)
{
    uint8_t row[12];
    wr32(row + 0, index);
    wr32(row + 4, track);
    wr32(row + 8, list);
    page_add_row(w, row, sizeof row, 1);
}

static void write_pdb(int with_playlists)
{
    static uint8_t file[4 * PAGE];
    memset(file, 0, sizeof file);
    wr32(file + 4, PAGE);
    wr32(file + 8, 3);
    /* table pointers at 0x1C: type, empty_candidate, first_page, last_page */
    wr32(file + 0x1C + 0, 0x00);                      /* tracks: empty */
    wr32(file + 0x1C + 8, 0xFFFFFFFFu);
    wr32(file + 0x2C + 0, with_playlists ? 0x07 : 0x10);
    wr32(file + 0x2C + 8, 1);
    wr32(file + 0x3C + 0, with_playlists ? 0x08 : 0x11);
    wr32(file + 0x3C + 8, 2);

    char long_name[101];
    memset(long_name, 'L', 100);
    long_name[100] = '\0';

    page_writer_t w;
    page_begin(&w, file + 1 * PAGE, 0xFFFFFFFFu);
    tree_row(&w, 0, 1, 10, 1, "Sets", 1);
    tree_row(&w, 10, 2, 11, 0, "Friday", 1);
    tree_row(&w, 10, 1, 12, 0, "Warmup", 1);
    tree_row(&w, 0, 0, 13, 0, "Favorites", 1);
    tree_row(&w, 0, 2, 14, 0, "Empty", 1);
    tree_row(&w, 0, 5, 15, 0, "Deleted", 0);          /* rowpf bit clear */
    tree_row(&w, 0, 3, 16, 0, long_name, 1);
    tree_row(&w, 0, 4, 0, 0, "NoId", 1);               /* id 0: skipped */

    page_begin(&w, file + 2 * PAGE, 3);
    entry_row(&w, 2, 102, 11);                         /* out of order */
    entry_row(&w, 1, 101, 11);
    entry_row(&w, 0, 7, 13);
    entry_row(&w, 1, 7, 13);                           /* duplicate kept */
    page_begin(&w, file + 3 * PAGE, 0xFFFFFFFFu);
    entry_row(&w, 2, 999, 13);                         /* not in catalog */
    entry_row(&w, 0, 5, 12);
    entry_row(&w, 1, 0, 12);                           /* track 0: skipped */
    entry_row(&w, 0, 8, 0);                            /* list 0: skipped */

    FILE *fp = fopen(PDB_PATH, "wb");
    CHECK(fp != NULL);
    if (!fp) return;
    CHECK(fwrite(file, 1, sizeof file, fp) == sizeof file);
    fclose(fp);
}

static void test_parse(void)
{
    write_pdb(1);
    pdb_t *pdb = NULL;
    CHECK(pdb_open(PDB_PATH, &pdb) == ESP_OK);
    if (!pdb) return;

    CHECK(pdb_track_count(pdb) == 0);
    CHECK(pdb_playlist_count(pdb) == 6);              /* Deleted + id 0 dropped */
    pdb_playlist_t pl;
    CHECK(pdb_get_playlist(pdb, 0, &pl) == ESP_OK);
    CHECK(pl.id == 10 && pl.is_folder && pl.parent_id == 0 && strcmp(pl.name, "Sets") == 0);
    CHECK(pdb_get_playlist(pdb, 1, &pl) == ESP_OK);
    CHECK(pl.id == 11 && !pl.is_folder && pl.parent_id == 10 && pl.sort_order == 2);
    CHECK(pdb_get_playlist(pdb, 5, &pl) == ESP_OK);
    CHECK(pl.id == 16 && strlen(pl.name) == PDB_PLAYLIST_NAME_MAX - 1u);
    CHECK(pdb_get_playlist(pdb, 6, &pl) == ESP_ERR_INVALID_ARG);
    CHECK(pdb_get_playlist(pdb, -1, &pl) == ESP_ERR_INVALID_ARG);

    uint32_t ids[8];
    CHECK(pdb_playlist_track_ids(pdb, 11, NULL, 0) == 2);
    CHECK(pdb_playlist_track_ids(pdb, 11, ids, 8) == 2);
    CHECK(ids[0] == 101 && ids[1] == 102);            /* entry_index order */
    CHECK(pdb_playlist_track_ids(pdb, 13, ids, 8) == 3);
    CHECK(ids[0] == 7 && ids[1] == 7 && ids[2] == 999);
    CHECK(pdb_playlist_track_ids(pdb, 13, ids, 2) == 2);   /* bounded */
    CHECK(pdb_playlist_track_ids(pdb, 12, ids, 8) == 1 && ids[0] == 5);
    CHECK(pdb_playlist_track_ids(pdb, 14, ids, 8) == 0);
    CHECK(pdb_playlist_track_ids(pdb, 0, ids, 8) == 0);
    CHECK(pdb_playlist_track_ids(pdb, 12345, ids, 8) == 0);

    /* Flattened: root by sort_order, folder children depth-first. */
    static const uint32_t known[] = { 5, 7, 101, 102 };
    library_playlist_set_t set = {0};
    CHECK(library_playlists_build(pdb, known, 4, &set) == ESP_OK);
    CHECK(set.list_count == 5);
    if (set.list_count == 5) {
        static const char *names[] = { "Favorites", "Warmup", "Friday", "Empty" };
        static const char *folders[] = { "", "Sets", "Sets", "" };
        static const uint16_t counts[] = { 2, 1, 2, 0 };
        static const uint16_t missing[] = { 1, 0, 0, 0 };
        for (int i = 0; i < 4; i++) {
            CHECK(strcmp(set.lists[i].name, names[i]) == 0);
            CHECK(strcmp(set.lists[i].folder, folders[i]) == 0);
            CHECK(set.lists[i].count == counts[i]);
            CHECK(set.lists[i].missing == missing[i]);
        }
        CHECK(strlen(set.lists[4].name) == LIBRARY_PLAYLIST_NAME_MAX - 1);
        const library_playlist_t *fri = &set.lists[2];
        CHECK(set.keys[fri->first] == 101 && set.keys[fri->first + 1] == 102);
        CHECK(set.keys[set.lists[0].first] == 7);
        CHECK(set.key_count == 5);
    }
    library_playlists_free(&set);
    CHECK(set.lists == NULL && set.keys == NULL && set.list_count == 0);

    pdb_close(pdb);
}

static void test_no_playlist_tables(void)
{
    write_pdb(0);
    pdb_t *pdb = NULL;
    CHECK(pdb_open(PDB_PATH, &pdb) == ESP_OK);
    if (!pdb) return;
    CHECK(pdb_playlist_count(pdb) == 0);
    CHECK(pdb_playlist_track_ids(pdb, 11, NULL, 0) == 0);
    library_playlist_set_t set = {0};
    CHECK(library_playlists_build(pdb, NULL, 0, &set) == ESP_OK);
    CHECK(set.list_count == 0);
    library_playlists_free(&set);
    pdb_close(pdb);
}

static void test_null_args(void)
{
    pdb_playlist_t pl;
    uint32_t ids[2];
    CHECK(pdb_playlist_count(NULL) == 0);
    CHECK(pdb_get_playlist(NULL, 0, &pl) != ESP_OK);
    CHECK(pdb_playlist_track_ids(NULL, 1, ids, 2) == 0);
    library_playlists_free(NULL);
}

int main(void)
{
    test_parse();
    test_no_playlist_tables();
    test_null_args();
    remove(PDB_PATH);
    if (s_failures) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("rekordbox PDB playlist tests passed\n");
    return 0;
}
