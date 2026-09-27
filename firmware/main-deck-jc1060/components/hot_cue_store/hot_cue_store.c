#include "hot_cue_store.h"

#include <stdio.h>
#include <string.h>

#define HOT_CUE_STORE_VERSION    2u
#define HOT_CUE_STORE_VERSION_V1 1u

/* v1 layout, before the cue point. */
typedef struct {
    uint32_t version;
    uint32_t valid_mask;
    hot_cue_store_slot_t slots[HOT_CUE_STORE_SLOT_COUNT];
} hot_cue_store_blob_v1_t;

#if !defined(HOT_CUE_STORE_STANDALONE_TEST)
static esp_err_t make_key(uint32_t track_key, char out[16])
{
    if (track_key == 0 || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, 16, "hc%08lx", (unsigned long)track_key);
    return ESP_OK;
}
#endif

static void normalize_blob(hot_cue_store_blob_t *blob)
{
    blob->version = HOT_CUE_STORE_VERSION;
    blob->valid_mask &= 0xFFu;
    for (uint32_t i = 0; i < HOT_CUE_STORE_SLOT_COUNT; i++) {
        if ((blob->valid_mask & (1u << i)) == 0) {
            memset(&blob->slots[i], 0, sizeof(blob->slots[i]));
            continue;
        }
        if (blob->slots[i].type != HOT_CUE_STORE_TYPE_LOOP) {
            blob->slots[i].type = HOT_CUE_STORE_TYPE_SINGLE;
            blob->slots[i].end_ms = 0;
        }
        memset(blob->slots[i].reserved, 0, sizeof(blob->slots[i].reserved));
    }
    blob->has_cue = blob->has_cue ? 1u : 0u;
    if (!blob->has_cue) {
        blob->cue_point_ms = 0;
    }
    memset(blob->reserved, 0, sizeof(blob->reserved));
}

esp_err_t hot_cue_store_decode(const void *raw, size_t len, hot_cue_store_blob_t *out_blob)
{
    if (!raw || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    hot_cue_store_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    if (len == sizeof(hot_cue_store_blob_t)) {
        memcpy(&blob, raw, sizeof(blob));
        if (blob.version != HOT_CUE_STORE_VERSION) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else if (len == sizeof(hot_cue_store_blob_v1_t)) {
        /* Hot cues only: the cue point stays absent. */
        memcpy(&blob, raw, sizeof(hot_cue_store_blob_v1_t));
        if (blob.version != HOT_CUE_STORE_VERSION_V1) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        return ESP_ERR_INVALID_SIZE;
    }
    normalize_blob(&blob);
    *out_blob = blob;
    return ESP_OK;
}

#if defined(HOT_CUE_STORE_STANDALONE_TEST)

typedef struct {
    uint32_t key;
    uint8_t raw[sizeof(hot_cue_store_blob_t)];
    size_t len;
    int valid;
} test_entry_t;

static test_entry_t s_entries[16];

/* Test hook: store raw bytes as an older firmware would have. */
esp_err_t hot_cue_store_test_put_raw(uint32_t track_key, const void *raw, size_t len);
esp_err_t hot_cue_store_test_put_raw(uint32_t track_key, const void *raw, size_t len)
{
    if (track_key == 0 || !raw || len > sizeof(s_entries[0].raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        if (!s_entries[i].valid || s_entries[i].key == track_key) {
            s_entries[i].key = track_key;
            memcpy(s_entries[i].raw, raw, len);
            s_entries[i].len = len;
            s_entries[i].valid = 1;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t hot_cue_store_load(uint32_t track_key, hot_cue_store_blob_t *out_blob)
{
    if (track_key == 0 || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        if (s_entries[i].valid && s_entries[i].key == track_key) {
            return hot_cue_store_decode(s_entries[i].raw, s_entries[i].len, out_blob);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t hot_cue_store_save(uint32_t track_key, const hot_cue_store_blob_t *blob)
{
    if (track_key == 0 || !blob) {
        return ESP_ERR_INVALID_ARG;
    }
    hot_cue_store_blob_t normalized = *blob;
    normalize_blob(&normalized);
    return hot_cue_store_test_put_raw(track_key, &normalized, sizeof(normalized));
}

esp_err_t hot_cue_store_clear(uint32_t track_key)
{
    if (track_key == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        if (s_entries[i].valid && s_entries[i].key == track_key) {
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
            return ESP_OK;
        }
    }
    return ESP_OK;
}

#else

#include "nvs.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "hot_cue_store";
static const char *NS = "hotcue";

esp_err_t hot_cue_store_load(uint32_t track_key, hot_cue_store_blob_t *out_blob)
{
    if (track_key == 0 || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    ESP_RETURN_ON_ERROR(make_key(track_key, key), TAG, "key");

    nvs_handle_t h;
    esp_err_t rc = nvs_open(NS, NVS_READONLY, &h);
    if (rc != ESP_OK) {
        return rc == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : rc;
    }
    /* A v1 blob is shorter: NVS reports its real length. */
    hot_cue_store_blob_t raw;
    size_t len = sizeof(raw);
    rc = nvs_get_blob(h, key, &raw, &len);
    nvs_close(h);
    if (rc != ESP_OK) {
        return rc == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : rc;
    }
    return hot_cue_store_decode(&raw, len, out_blob);
}

esp_err_t hot_cue_store_save(uint32_t track_key, const hot_cue_store_blob_t *blob)
{
    if (track_key == 0 || !blob) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    ESP_RETURN_ON_ERROR(make_key(track_key, key), TAG, "key");

    hot_cue_store_blob_t normalized = *blob;
    normalize_blob(&normalized);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t rc = nvs_set_blob(h, key, &normalized, sizeof(normalized));
    if (rc == ESP_OK) {
        rc = nvs_commit(h);
    }
    nvs_close(h);
    return rc;
}

esp_err_t hot_cue_store_clear(uint32_t track_key)
{
    if (track_key == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    ESP_RETURN_ON_ERROR(make_key(track_key, key), TAG, "key");

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t rc = nvs_erase_key(h, key);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        rc = ESP_OK;
    }
    if (rc == ESP_OK) {
        rc = nvs_commit(h);
    }
    nvs_close(h);
    return rc;
}

#endif
