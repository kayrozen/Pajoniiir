/* JC1060 hot_cue_store v2: hot cues plus the manual cue point per track, and
 * the v1 -> v2 migration of blobs written by earlier firmware. v303: cue
 * origin (analysis vs local) and the per-load merge with the analysis. v309:
 * memory cues, the edits made on the deck kept beside the analysis list. */
#include "hot_cue_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

esp_err_t hot_cue_store_test_put_raw(uint32_t track_key, const void *raw, size_t len);

/* v1 layout as written up to v252. */
typedef struct {
    uint32_t version;
    uint32_t valid_mask;
    hot_cue_store_slot_t slots[HOT_CUE_STORE_SLOT_COUNT];
} blob_v1_t;

static void test_v2_roundtrip_with_cue(void)
{
    hot_cue_store_blob_t blob = {0};
    blob.valid_mask = 0x05;
    blob.slots[0].pos_ms = 1000;
    blob.slots[0].type = HOT_CUE_STORE_TYPE_SINGLE;
    blob.slots[2].pos_ms = 2000;
    blob.slots[2].end_ms = 4000;
    blob.slots[2].type = HOT_CUE_STORE_TYPE_LOOP;
    blob.has_cue = 1;
    blob.cue_point_ms = 34567;
    assert(hot_cue_store_save(1234, &blob) == ESP_OK);

    hot_cue_store_blob_t loaded;
    memset(&loaded, 0xAA, sizeof(loaded));
    assert(hot_cue_store_load(1234, &loaded) == ESP_OK);
    assert(loaded.version == 2u);
    assert(loaded.valid_mask == 0x05);
    assert(loaded.slots[0].pos_ms == 1000);
    assert(loaded.slots[2].end_ms == 4000);
    assert(loaded.slots[2].type == HOT_CUE_STORE_TYPE_LOOP);
    assert(loaded.has_cue == 1u && loaded.cue_point_ms == 34567u);

    assert(hot_cue_store_clear(1234) == ESP_OK);
    assert(hot_cue_store_load(1234, &loaded) == ESP_ERR_NOT_FOUND);
}

/* Cue point without any hot cue, then forgotten (SHIFT+CUE). */
static void test_cue_only_and_forget(void)
{
    hot_cue_store_blob_t blob = {0};
    blob.has_cue = 1;
    blob.cue_point_ms = 0;              /* a cue at 0 is still a saved cue */
    assert(hot_cue_store_save(77, &blob) == ESP_OK);
    hot_cue_store_blob_t loaded;
    assert(hot_cue_store_load(77, &loaded) == ESP_OK);
    assert(loaded.valid_mask == 0u && loaded.has_cue == 1u && loaded.cue_point_ms == 0u);

    loaded.has_cue = 0;
    loaded.cue_point_ms = 9999;         /* stale value is not kept */
    assert(hot_cue_store_save(77, &loaded) == ESP_OK);
    assert(hot_cue_store_load(77, &loaded) == ESP_OK);
    assert(loaded.has_cue == 0u && loaded.cue_point_ms == 0u);

    /* has_cue is a flag: any non-zero byte reads back as 1. */
    blob.has_cue = 0x80;
    blob.cue_point_ms = 5;
    assert(hot_cue_store_save(77, &blob) == ESP_OK);
    assert(hot_cue_store_load(77, &loaded) == ESP_OK);
    assert(loaded.has_cue == 1u && loaded.cue_point_ms == 5u);
    assert(hot_cue_store_clear(77) == ESP_OK);
}

/* A v1 blob keeps its hot cues and loads with no cue point; the next save
 * writes v2 without losing them. */
static void test_v1_migration(void)
{
    blob_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1u;
    v1.valid_mask = 0x81;
    v1.slots[0].pos_ms = 111;
    v1.slots[0].type = HOT_CUE_STORE_TYPE_SINGLE;
    v1.slots[7].pos_ms = 5000;
    v1.slots[7].end_ms = 6000;
    v1.slots[7].type = HOT_CUE_STORE_TYPE_LOOP;
    assert(hot_cue_store_test_put_raw(4242, &v1, sizeof(v1)) == ESP_OK);

    hot_cue_store_blob_t loaded;
    memset(&loaded, 0xAA, sizeof(loaded));
    assert(hot_cue_store_load(4242, &loaded) == ESP_OK);
    assert(loaded.version == 2u);
    assert(loaded.valid_mask == 0x81);
    assert(loaded.slots[0].pos_ms == 111);
    assert(loaded.slots[7].pos_ms == 5000 && loaded.slots[7].end_ms == 6000);
    assert(loaded.slots[7].type == HOT_CUE_STORE_TYPE_LOOP);
    assert(loaded.has_cue == 0u && loaded.cue_point_ms == 0u);
    for (int i = 1; i < 7; i++) {
        assert(loaded.slots[i].pos_ms == 0u && loaded.slots[i].type == 0u);
    }

    loaded.has_cue = 1;
    loaded.cue_point_ms = 2222;
    assert(hot_cue_store_save(4242, &loaded) == ESP_OK);
    hot_cue_store_blob_t again;
    assert(hot_cue_store_load(4242, &again) == ESP_OK);
    assert(again.valid_mask == 0x81 && again.slots[7].end_ms == 6000);
    assert(again.has_cue == 1u && again.cue_point_ms == 2222u);
    assert(hot_cue_store_clear(4242) == ESP_OK);
}

static void test_decode_rejects_bad_blobs(void)
{
    hot_cue_store_blob_t out;
    blob_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 2u;                    /* v1 size with a v2 header */
    assert(hot_cue_store_decode(&v1, sizeof(v1), &out) == ESP_ERR_INVALID_SIZE);

    hot_cue_store_blob_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.version = 1u;                    /* v2 size with a v1 header */
    assert(hot_cue_store_decode(&v2, sizeof(v2), &out) == ESP_ERR_INVALID_SIZE);
    v2.version = 3u;
    assert(hot_cue_store_decode(&v2, sizeof(v2), &out) == ESP_ERR_INVALID_SIZE);
    v2.version = 2u;
    assert(hot_cue_store_decode(&v2, sizeof(v2) - 1u, &out) == ESP_ERR_INVALID_SIZE);
    assert(hot_cue_store_decode(&v2, 0u, &out) == ESP_ERR_INVALID_SIZE);
    assert(hot_cue_store_decode(NULL, sizeof(v2), &out) == ESP_ERR_INVALID_ARG);

    /* A corrupt stored blob is reported, never silently read as empty. */
    assert(hot_cue_store_test_put_raw(99, &v2, 10u) == ESP_OK);
    assert(hot_cue_store_load(99, &out) == ESP_ERR_INVALID_SIZE);
    assert(hot_cue_store_clear(99) == ESP_OK);
}

static void test_rejects_invalid_track_key(void)
{
    hot_cue_store_blob_t blob = {0};
    assert(hot_cue_store_save(0, &blob) == ESP_ERR_INVALID_ARG);
    assert(hot_cue_store_load(0, &blob) == ESP_ERR_INVALID_ARG);
    assert(hot_cue_store_clear(0) == ESP_ERR_INVALID_ARG);
}

static hot_cue_store_slot_t slot(uint32_t pos, uint32_t end)
{
    hot_cue_store_slot_t s = { .pos_ms = pos, .end_ms = end,
                               .type = end ? HOT_CUE_STORE_TYPE_LOOP
                                           : HOT_CUE_STORE_TYPE_SINGLE };
    return s;
}

/* v303: the origin survives a save, and only for cues that exist. */
static void test_origin_roundtrip(void)
{
    hot_cue_store_blob_t blob = {0};
    blob.valid_mask = 0x03;
    blob.slots[0] = slot(1000, 0);
    blob.slots[1] = slot(2000, 0);
    blob.source_mask = 0x86;            /* bit 7 and 2: no such slot */
    blob.has_cue = 1;
    blob.cue_point_ms = 500;
    blob.cue_from_source = 1;
    assert(hot_cue_store_save(55, &blob) == ESP_OK);
    hot_cue_store_blob_t loaded;
    assert(hot_cue_store_load(55, &loaded) == ESP_OK);
    assert(loaded.source_mask == 0x02 && loaded.cue_from_source == 1u);

    loaded.has_cue = 0;                 /* a forgotten cue has no origin */
    assert(hot_cue_store_save(55, &loaded) == ESP_OK);
    assert(hot_cue_store_load(55, &loaded) == ESP_OK);
    assert(loaded.cue_from_source == 0u && loaded.reserved[0] == 0u);
    assert(hot_cue_store_clear(55) == ESP_OK);
}

/* First load: the analysis seeds everything and marks it as such. */
static void test_merge_first_seed(void)
{
    hot_cue_store_blob_t stored = {0};
    hot_cue_store_blob_t seed = {0};
    seed.valid_mask = 0x05;
    seed.slots[0] = slot(1000, 0);
    seed.slots[2] = slot(3000, 5000);
    hot_cue_store_blob_t out;
    hot_cue_store_merge_source(&stored, &seed, true, true, 800, 0, &out);
    assert(out.valid_mask == 0x05 && out.source_mask == 0x05);
    assert(out.slots[2].type == HOT_CUE_STORE_TYPE_LOOP && out.slots[2].end_ms == 5000);
    assert(out.has_cue == 1u && out.cue_point_ms == 800u && out.cue_from_source == 1u);
}

/* The reported bug: cues edited at the source after the first seed. Source
 * slots and the source cue point follow; a source cue gone from the analysis
 * goes away; local pads and a local cue are never touched. */
static void test_merge_follows_source_edits(void)
{
    hot_cue_store_blob_t stored = {0};
    stored.valid_mask = 0x0b;           /* A, B from the source; D local */
    stored.source_mask = 0x03;
    stored.slots[0] = slot(1000, 0);
    stored.slots[1] = slot(2000, 0);
    stored.slots[3] = slot(4444, 0);
    stored.has_cue = 1;
    stored.cue_point_ms = 800;
    stored.cue_from_source = 1;

    hot_cue_store_blob_t seed = {0};    /* A moved, B deleted, C and D new */
    seed.valid_mask = 0x0d;
    seed.slots[0] = slot(1500, 0);
    seed.slots[2] = slot(3000, 0);
    seed.slots[3] = slot(9999, 0);
    hot_cue_store_blob_t out;
    hot_cue_store_merge_source(&stored, &seed, true, true, 900, 0, &out);
    assert(out.valid_mask == 0x0d && out.source_mask == 0x05);
    assert(out.slots[0].pos_ms == 1500 && out.slots[2].pos_ms == 3000);
    assert(out.slots[1].pos_ms == 0 && out.slots[1].type == 0);
    assert(out.slots[3].pos_ms == 4444);              /* local D wins */
    assert(out.has_cue == 1u && out.cue_point_ms == 900u && out.cue_from_source == 1u);

    /* Memory cue removed at the source: back to track start. */
    hot_cue_store_merge_source(&out, &seed, true, false, 0, 0, &out);
    assert(out.has_cue == 0u && out.cue_from_source == 0u);

    /* A local cue point stays, whatever the source says. */
    stored.cue_from_source = 0;
    hot_cue_store_merge_source(&stored, &seed, true, true, 900, 0, &out);
    assert(out.has_cue == 1u && out.cue_point_ms == 800u && out.cue_from_source == 0u);
}

/* No cue lists in the analysis (none at all, or a DJ Link DAT whose cue
 * request went unanswered): the stored cues stay as they are. Blobs from
 * before v303 carry no origin, so they read as local. */
static void test_merge_without_source(void)
{
    hot_cue_store_blob_t stored = {0};
    stored.valid_mask = 0x01;
    stored.source_mask = 0x01;
    stored.slots[0] = slot(1000, 0);
    stored.has_cue = 1;
    stored.cue_point_ms = 700;
    stored.cue_from_source = 1;
    hot_cue_store_blob_t seed = {0};
    hot_cue_store_blob_t out;
    hot_cue_store_merge_source(&stored, &seed, false, false, 0, 0, &out);
    assert(out.valid_mask == 0x01 && out.source_mask == 0x01);
    assert(out.has_cue == 1u && out.cue_point_ms == 700u && out.cue_from_source == 1u);

    hot_cue_store_blob_t legacy = {0};
    legacy.valid_mask = 0x01;
    legacy.slots[0] = slot(1000, 0);
    legacy.has_cue = 1;
    legacy.cue_point_ms = 700;
    seed.valid_mask = 0x03;
    seed.slots[0] = slot(1200, 0);
    seed.slots[1] = slot(2200, 0);
    hot_cue_store_merge_source(&legacy, &seed, true, true, 900, 0, &out);
    assert(out.valid_mask == 0x03 && out.source_mask == 0x02);
    assert(out.slots[0].pos_ms == 1000 && out.slots[1].pos_ms == 2200);
    assert(out.has_cue == 1u && out.cue_point_ms == 700u && out.cue_from_source == 0u);
}

/* A cue point at or past the track end is dropped; the source one is used
 * only when it fits. */
static void test_merge_duration(void)
{
    hot_cue_store_blob_t stored = {0};
    stored.has_cue = 1;
    stored.cue_point_ms = 60000;
    hot_cue_store_blob_t seed = {0};
    hot_cue_store_blob_t out;
    hot_cue_store_merge_source(&stored, &seed, true, true, 1000, 30000, &out);
    assert(out.has_cue == 1u && out.cue_point_ms == 1000u && out.cue_from_source == 1u);
    hot_cue_store_merge_source(&stored, &seed, true, true, 30000, 30000, &out);
    assert(out.has_cue == 0u);
    hot_cue_store_merge_source(&stored, &seed, false, false, 0, 30000, &out);
    assert(out.has_cue == 0u);
}

/* ── v309: memory cues ─────────────────────────────────────────────────── */

static const hot_cue_store_memory_t k_source[] = {
    { .pos_ms = 5000 }, { .pos_ms = 1000 }, { .pos_ms = 9000, .end_ms = 11000 },
};

static void test_memory_merge_sorts_and_caps(void)
{
    hot_cue_store_memory_blob_t edits = {0};
    hot_cue_store_memory_list_t list;
    assert(!hot_cue_store_memory_merge(&edits, k_source, 3, true, 0, &list));
    assert(list.count == 3u && edits.version == HOT_CUE_STORE_MEMORY_VERSION);
    assert(list.cues[0].pos_ms == 1000u && list.cues[1].pos_ms == 5000u);
    assert(list.cues[2].pos_ms == 9000u && list.cues[2].end_ms == 11000u);
    for (int i = 0; i < 3; i++) assert(list.cues[i].flags == 0u);

    /* Without cue lists the analysis is ignored; past the end is dropped. */
    assert(!hot_cue_store_memory_merge(&edits, k_source, 3, false, 0, &list));
    assert(list.count == 0u);
    assert(!hot_cue_store_memory_merge(&edits, k_source, 3, true, 9000, &list));
    assert(list.count == 2u && list.cues[1].pos_ms == 5000u);

    /* 20 cues (and a duplicate): the earliest 16, once each. */
    hot_cue_store_memory_t many[21];
    for (uint32_t i = 0; i < 20; i++) many[i] = (hot_cue_store_memory_t) { .pos_ms = (20 - i) * 100 };
    many[20] = many[0];
    assert(!hot_cue_store_memory_merge(&edits, many, 21, true, 0, &list));
    assert(list.count == HOT_CUE_STORE_MEMORY_MAX);
    for (uint32_t i = 0; i < HOT_CUE_STORE_MEMORY_MAX; i++) {
        assert(list.cues[i].pos_ms == (i + 1) * 100);
    }
}

static void test_memory_add_and_delete(void)
{
    hot_cue_store_memory_blob_t edits = {0};
    hot_cue_store_memory_list_t list;
    (void)hot_cue_store_memory_merge(&edits, k_source, 3, true, 0, &list);

    /* MEMORY adds a local cue in time order; the same start again is refused. */
    assert(hot_cue_store_memory_add(&edits, &list, 3000, 0) == HOT_CUE_STORE_MEMORY_ADDED);
    assert(list.count == 4u && list.cues[1].pos_ms == 3000u);
    assert(list.cues[1].flags == HOT_CUE_STORE_MEMORY_LOCAL);
    assert(edits.count == 1u && edits.edits[0].flags == HOT_CUE_STORE_MEMORY_LOCAL);
    assert(hot_cue_store_memory_add(&edits, &list, 3000, 4000) == HOT_CUE_STORE_MEMORY_EXISTS);

    /* DELETE of an analysis cue hides it; of a local one drops the edit. */
    assert(hot_cue_store_memory_remove(&edits, &list, 0));
    assert(list.count == 3u && list.cues[0].pos_ms == 3000u);
    assert(edits.count == 2u && edits.edits[1].flags == HOT_CUE_STORE_MEMORY_HIDDEN &&
           edits.edits[1].pos_ms == 1000u);
    assert(hot_cue_store_memory_remove(&edits, &list, 0));
    assert(edits.count == 1u && edits.edits[0].pos_ms == 1000u);
    assert(!hot_cue_store_memory_remove(&edits, &list, 9));

    /* Saved and loaded again, the edits rebuild the same list. */
    assert(hot_cue_store_memory_save(4242, &edits) == ESP_OK);
    hot_cue_store_memory_blob_t back;
    assert(hot_cue_store_memory_load(4242, &back) == ESP_OK);
    assert(back.count == 1u && back.edits[0].flags == HOT_CUE_STORE_MEMORY_HIDDEN);
    assert(!hot_cue_store_memory_merge(&back, k_source, 3, true, 0, &list));
    assert(list.count == 2u && list.cues[0].pos_ms == 5000u);

    /* MEMORY on a hidden analysis cue un-hides it: no edit left. */
    assert(hot_cue_store_memory_add(&back, &list, 1000, 0) == HOT_CUE_STORE_MEMORY_ADDED);
    assert(back.count == 0u && list.count == 3u && list.cues[0].flags == 0u);
    assert(hot_cue_store_memory_save(4242, &back) == ESP_OK);
    assert(hot_cue_store_memory_load(4242, &back) == ESP_ERR_NOT_FOUND && back.count == 0u);
}

static void test_memory_merge_prunes_edits(void)
{
    hot_cue_store_memory_blob_t edits = {0};
    hot_cue_store_memory_list_t list;
    edits.count = 3;
    edits.edits[0] = (hot_cue_store_memory_t) { .pos_ms = 7000, .flags = HOT_CUE_STORE_MEMORY_HIDDEN };
    edits.edits[1] = (hot_cue_store_memory_t) { .pos_ms = 5000, .flags = HOT_CUE_STORE_MEMORY_LOCAL };
    edits.edits[2] = (hot_cue_store_memory_t) { .pos_ms = 2000, .flags = HOT_CUE_STORE_MEMORY_LOCAL };
    /* 7000 is gone from the analysis, 5000 is now in it: both edits go. */
    assert(hot_cue_store_memory_merge(&edits, k_source, 3, true, 0, &list));
    assert(edits.count == 1u && edits.edits[0].pos_ms == 2000u);
    assert(list.count == 4u && list.cues[1].pos_ms == 2000u &&
           list.cues[1].flags == HOT_CUE_STORE_MEMORY_LOCAL && list.cues[2].flags == 0u);
    /* Without cue lists the edits stay as they are. */
    edits.edits[1] = (hot_cue_store_memory_t) { .pos_ms = 7000, .flags = HOT_CUE_STORE_MEMORY_HIDDEN };
    edits.count = 2;
    assert(!hot_cue_store_memory_merge(&edits, k_source, 3, false, 0, &list));
    assert(edits.count == 2u && list.count == 1u);
}

static void test_memory_full(void)
{
    hot_cue_store_memory_blob_t edits = {0};
    hot_cue_store_memory_list_t list;
    (void)hot_cue_store_memory_merge(&edits, NULL, 0, true, 0, &list);
    for (uint32_t i = 0; i < HOT_CUE_STORE_MEMORY_MAX; i++) {
        assert(hot_cue_store_memory_add(&edits, &list, i * 10, 0) == HOT_CUE_STORE_MEMORY_ADDED);
    }
    assert(hot_cue_store_memory_add(&edits, &list, 999, 0) == HOT_CUE_STORE_MEMORY_FULL);
    assert(list.count == HOT_CUE_STORE_MEMORY_MAX && edits.count == HOT_CUE_STORE_MEMORY_MAX);
}

static void test_memory_find_and_step(void)
{
    hot_cue_store_memory_blob_t edits = {0};
    hot_cue_store_memory_list_t list;
    (void)hot_cue_store_memory_merge(&edits, k_source, 3, true, 0, &list);
    /* 1000, 5000, 9000 */
    assert(hot_cue_store_memory_find(&list, 5020, 50) == 1);
    assert(hot_cue_store_memory_find(&list, 5100, 50) == -1);
    /* Parked on 5000: back to 1000, on to 9000. */
    assert(hot_cue_store_memory_step(&list, 5000, 50, -1) == 0);
    assert(hot_cue_store_memory_step(&list, 5000, 50, +1) == 2);
    assert(hot_cue_store_memory_step(&list, 6000, 50, -1) == 1);
    assert(hot_cue_store_memory_step(&list, 1000, 50, -1) == -1);
    assert(hot_cue_store_memory_step(&list, 9000, 50, +1) == -1);
    assert(hot_cue_store_memory_step(&list, 0, 50, +1) == 0);
}

static void test_memory_decode(void)
{
    hot_cue_store_memory_blob_t blob = {0};
    blob.version = HOT_CUE_STORE_MEMORY_VERSION;
    blob.count = 3;
    blob.edits[0] = (hot_cue_store_memory_t) { .pos_ms = 10, .end_ms = 5, .flags = HOT_CUE_STORE_MEMORY_LOCAL };
    blob.edits[1] = (hot_cue_store_memory_t) { .pos_ms = 20, .flags = 0x7f };
    blob.edits[2] = (hot_cue_store_memory_t) { .pos_ms = 30, .flags = HOT_CUE_STORE_MEMORY_HIDDEN };
    assert(hot_cue_store_memory_decode(&blob, sizeof(blob)) == ESP_OK);
    assert(blob.count == 2u && blob.edits[0].end_ms == 0u && blob.edits[1].pos_ms == 30u);
    assert(hot_cue_store_memory_decode(&blob, sizeof(blob) - 1) == ESP_ERR_INVALID_SIZE);
    assert(blob.count == 0u);
    blob.version = 9;
    assert(hot_cue_store_memory_decode(&blob, sizeof(blob)) == ESP_ERR_INVALID_SIZE);
    assert(hot_cue_store_memory_load(0, &blob) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    assert(sizeof(hot_cue_store_blob_t) == sizeof(blob_v1_t) + 8u);
    test_v2_roundtrip_with_cue();
    test_cue_only_and_forget();
    test_v1_migration();
    test_decode_rejects_bad_blobs();
    test_rejects_invalid_track_key();
    test_origin_roundtrip();
    test_merge_first_seed();
    test_merge_follows_source_edits();
    test_merge_without_source();
    test_merge_duration();
    test_memory_merge_sorts_and_caps();
    test_memory_add_and_delete();
    test_memory_merge_prunes_edits();
    test_memory_full();
    test_memory_find_and_step();
    test_memory_decode();
    puts("hot_cue_store_jc1060 tests passed");
    return 0;
}
