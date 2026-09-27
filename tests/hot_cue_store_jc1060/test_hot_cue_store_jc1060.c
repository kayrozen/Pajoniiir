/* JC1060 hot_cue_store v2: hot cues plus the manual cue point per track, and
 * the v1 -> v2 migration of blobs written by earlier firmware. */
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

int main(void)
{
    assert(sizeof(hot_cue_store_blob_t) == sizeof(blob_v1_t) + 8u);
    test_v2_roundtrip_with_cue();
    test_cue_only_and_forget();
    test_v1_migration();
    test_decode_rejects_bad_blobs();
    test_rejects_invalid_track_key();
    puts("hot_cue_store_jc1060 tests passed");
    return 0;
}
