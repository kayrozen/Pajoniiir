/* Host test: export.pdb Artwork table (0x0D) parsing and the track
 * artwork_id link (JC1060). The PDB is synthetic: page 0 header, page 1
 * tracks, pages 2 -> 3 artwork (chained). An optional argument parses a real
 * export.pdb and checks that every track's artwork_id resolves. */
#include "rekordbox_pdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 1024u
#define PDB_PATH "test_pdb_artwork_jc1060.pdb"

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

/* Minimal track row: subtype, artwork_id @0x1C, track id @0x48, no strings. */
static void track_row(page_writer_t *w, uint32_t id, uint32_t artwork_id)
{
    uint8_t row[0x88] = {0};
    wr16(row + 0x00, 0x0024);
    wr32(row + 0x1C, artwork_id);
    wr32(row + 0x48, id);
    page_add_row(w, row, sizeof row, 1);
}

static void artwork_row(page_writer_t *w, uint32_t id, const char *path, int present)
{
    uint8_t row[4 + 128] = {0};
    size_t n = strlen(path);
    wr32(row + 0x00, id);
    row[0x04] = (uint8_t)(((n + 1u) << 1) | 1u);    /* short DeviceSQL string */
    memcpy(row + 0x05, path, n);
    page_add_row(w, row, (uint32_t)(0x05 + n), present);
}

static void write_pdb(int with_artwork)
{
    static uint8_t file[4 * PAGE];
    memset(file, 0, sizeof file);
    wr32(file + 4, PAGE);
    wr32(file + 8, 2);
    /* table pointers at 0x1C: type, empty_candidate, first_page, last_page */
    wr32(file + 0x1C + 0, 0x00);
    wr32(file + 0x1C + 8, 1);
    wr32(file + 0x2C + 0, with_artwork ? 0x0D : 0x05); /* keys: rows ignored */
    wr32(file + 0x2C + 8, with_artwork ? 2 : 0xFFFFFFFFu);

    page_writer_t w;
    page_begin(&w, file + 1 * PAGE, 0xFFFFFFFFu);
    track_row(&w, 101, 3);
    track_row(&w, 102, 0);                            /* no artwork */
    track_row(&w, 103, 1);
    track_row(&w, 104, 3);                            /* shared artwork */
    track_row(&w, 105, 77);                           /* dangling id */

    char long_path[80];
    memset(long_path, 'x', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';

    page_begin(&w, file + 2 * PAGE, 3);
    artwork_row(&w, 3, "/PIONEER/Artwork/00001/a3.jpg", 1);
    artwork_row(&w, 0, "/PIONEER/Artwork/00001/a0.jpg", 1);   /* id 0: skipped */
    artwork_row(&w, 9, "/PIONEER/Artwork/00001/a9.jpg", 0);   /* rowpf clear */
    artwork_row(&w, 4, long_path, 1);                         /* does not fit */
    page_begin(&w, file + 3 * PAGE, 0xFFFFFFFFu);
    artwork_row(&w, 1, "/PIONEER/Artwork/00001/a1.jpg", 1);   /* out of order */
    artwork_row(&w, 5, "relative/a5.jpg", 1);                 /* not absolute */
    artwork_row(&w, 2, "/PIONEER/Artwork/00001/a2.jpg", 1);

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

    CHECK(pdb_artwork_count(pdb) == 3);
    pdb_artwork_t a;
    CHECK(pdb_get_artwork(pdb, 0, &a) == ESP_OK);
    CHECK(a.id == 1 && strcmp(a.path, "/PIONEER/Artwork/00001/a1.jpg") == 0);
    CHECK(pdb_get_artwork(pdb, 1, &a) == ESP_OK && a.id == 2);
    CHECK(pdb_get_artwork(pdb, 2, &a) == ESP_OK && a.id == 3);
    CHECK(pdb_get_artwork(pdb, 3, &a) == ESP_ERR_INVALID_ARG);
    CHECK(pdb_get_artwork(pdb, -1, &a) == ESP_ERR_INVALID_ARG);

    const char *p = pdb_artwork_path(pdb, 3);
    CHECK(p && strcmp(p, "/PIONEER/Artwork/00001/a3.jpg") == 0);
    CHECK(pdb_artwork_path(pdb, 0) == NULL);
    CHECK(pdb_artwork_path(pdb, 4) == NULL);
    CHECK(pdb_artwork_path(pdb, 5) == NULL);
    CHECK(pdb_artwork_path(pdb, 9) == NULL);
    CHECK(pdb_artwork_path(pdb, 77) == NULL);

    CHECK(pdb_track_count(pdb) == 5);
    static const uint32_t want[5][2] = { {101, 3}, {102, 0}, {103, 1}, {104, 3}, {105, 77} };
    for (int i = 0; i < 5; i++) {
        pdb_track_t t;
        CHECK(pdb_get_track(pdb, i, &t) == ESP_OK);
        CHECK(t.track_id == want[i][0] && t.artwork_id == want[i][1]);
    }

    pdb_import_stats_t st;
    pdb_get_import_stats(pdb, &st);
    CHECK(!st.artwork_truncated);
    pdb_close(pdb);
}

static void test_no_artwork_table(void)
{
    write_pdb(0);
    pdb_t *pdb = NULL;
    CHECK(pdb_open(PDB_PATH, &pdb) == ESP_OK);
    if (!pdb) return;
    CHECK(pdb_artwork_count(pdb) == 0);
    CHECK(pdb_artwork_path(pdb, 3) == NULL);
    pdb_track_t t;
    CHECK(pdb_get_track(pdb, 0, &t) == ESP_OK && t.artwork_id == 3);
    pdb_close(pdb);
    CHECK(pdb_artwork_count(NULL) == 0);
    CHECK(pdb_artwork_path(NULL, 3) == NULL);
}

/* Real export.pdb: every nonzero artwork_id resolves to a .jpg path. */
static void test_real(const char *path)
{
    pdb_t *pdb = NULL;
    CHECK(pdb_open(path, &pdb) == ESP_OK);
    if (!pdb) return;
    int with_art = 0, unresolved = 0;
    for (int i = 0; i < pdb_track_count(pdb); i++) {
        pdb_track_t t;
        if (pdb_get_track(pdb, i, &t) != ESP_OK || t.artwork_id == 0u) continue;
        const char *p = pdb_artwork_path(pdb, t.artwork_id);
        if (!p) { unresolved++; continue; }
        size_t n = strlen(p);
        CHECK(n > 4 && strcmp(p + n - 4, ".jpg") == 0);
        if (with_art++ == 0) printf("  track %u -> %s\n", (unsigned)t.track_id, p);
    }
    printf("  real PDB: %d tracks, %d artworks, %d tracks with artwork, %d unresolved\n",
           pdb_track_count(pdb), pdb_artwork_count(pdb), with_art, unresolved);
    CHECK(unresolved == 0);
    pdb_close(pdb);
}

int main(int argc, char **argv)
{
    test_parse();
    test_no_artwork_table();
    if (argc > 1 && argv[1][0]) test_real(argv[1]);
    remove(PDB_PATH);
    if (s_failures) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("rekordbox_pdb_artwork_jc1060: all tests passed\n");
    return 0;
}
