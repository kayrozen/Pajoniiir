#include "hot_cue_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOT_CUE_STORE_VERSION_V1 1u

/* v1 layout, before the cue point. */
typedef struct {
    uint32_t version;
    uint32_t valid_mask;
    hot_cue_store_slot_t slots[HOT_CUE_STORE_SLOT_COUNT];
} hot_cue_store_blob_v1_t;

#if !defined(HOT_CUE_STORE_STANDALONE_TEST)
static esp_err_t make_key_prefixed(const char *prefix, uint32_t track_key, char out[16])
{
    if (track_key == 0 || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, 16, "%s%08lx", prefix, (unsigned long)track_key);
    return ESP_OK;
}

static esp_err_t make_key(uint32_t track_key, char out[16])
{
    return make_key_prefixed("hc", track_key, out);
}
#endif

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
    hot_cue_store_normalize(&blob);
    *out_blob = blob;
    return ESP_OK;
}

esp_err_t hot_cue_store_memory_decode(hot_cue_store_memory_blob_t *blob, size_t len)
{
    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != sizeof(*blob) || blob->version != HOT_CUE_STORE_MEMORY_VERSION ||
        blob->count > HOT_CUE_STORE_MEMORY_EDITS_MAX) {
        memset(blob, 0, sizeof(*blob));
        return ESP_ERR_INVALID_SIZE;
    }
    /* Keep only well-formed edits, in order. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < blob->count; i++) {
        hot_cue_store_memory_t e = blob->edits[i];
        if (e.flags != HOT_CUE_STORE_MEMORY_LOCAL && e.flags != HOT_CUE_STORE_MEMORY_HIDDEN) {
            continue;
        }
        if (e.end_ms <= e.pos_ms) {
            e.end_ms = 0;
        }
        memset(e.reserved, 0, sizeof(e.reserved));
        blob->edits[kept++] = e;
    }
    memset(&blob->edits[kept], 0, (HOT_CUE_STORE_MEMORY_EDITS_MAX - kept) * sizeof(blob->edits[0]));
    blob->count = kept;
    return ESP_OK;
}

static hot_cue_store_pin_fn s_pin_fn;

void hot_cue_store_set_pin_provider(hot_cue_store_pin_fn fn)
{
    s_pin_fn = fn;
}

#define HOT_CUE_STORE_PIN_MAX 4u

/* Keys a prune must keep: the provider's loaded tracks and the key being
 * saved. Returns the count. */
static size_t collect_pins(uint32_t keep_key, uint32_t pins[HOT_CUE_STORE_PIN_MAX])
{
    size_t n = s_pin_fn ? s_pin_fn(pins, HOT_CUE_STORE_PIN_MAX - 1u) : 0u;
    if (n > HOT_CUE_STORE_PIN_MAX - 1u) {
        n = HOT_CUE_STORE_PIN_MAX - 1u;
    }
    pins[n++] = keep_key;
    return n;
}

static bool key_is_pinned(const uint32_t *pins, size_t n, uint32_t key)
{
    for (size_t i = 0; i < n; i++) {
        if (pins[i] == key) {
            return true;
        }
    }
    return false;
}

/* Erases the seed-only hot cue blobs whose key is not pinned; the count. */
static size_t prune_seed_only(const uint32_t *pins, size_t pin_n);

/* A save ran out of space: free what a load rebuilds. The count erased; the
 * caller retries once when it is not 0. */
static size_t make_room(uint32_t keep_key)
{
    uint32_t pins[HOT_CUE_STORE_PIN_MAX];
    const size_t pin_n = collect_pins(keep_key, pins);
    return prune_seed_only(pins, pin_n);
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
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}

/* Test hook: an empty store. */
void hot_cue_store_test_reset(void);
void hot_cue_store_test_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_pin_fn = NULL;
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
    hot_cue_store_normalize(&normalized);
    esp_err_t rc = hot_cue_store_test_put_raw(track_key, &normalized, sizeof(normalized));
    if (rc == ESP_ERR_NVS_NOT_ENOUGH_SPACE && make_room(track_key) > 0u) {
        rc = hot_cue_store_test_put_raw(track_key, &normalized, sizeof(normalized));
    }
    return rc;
}

static size_t prune_seed_only(const uint32_t *pins, size_t pin_n)
{
    size_t erased = 0;
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        hot_cue_store_blob_t blob;
        if (s_entries[i].valid && !key_is_pinned(pins, pin_n, s_entries[i].key) &&
            hot_cue_store_decode(s_entries[i].raw, s_entries[i].len, &blob) == ESP_OK &&
            hot_cue_store_blob_is_seed_only(&blob)) {
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
            erased++;
        }
    }
    return erased;
}

void hot_cue_store_log_usage(const char *why)
{
    (void)why;
}

static struct {
    uint32_t key;
    hot_cue_store_memory_blob_t blob;
    int valid;
} s_memory_entries[8];

esp_err_t hot_cue_store_memory_load(uint32_t track_key, hot_cue_store_memory_blob_t *out_blob)
{
    if (track_key == 0 || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_blob, 0, sizeof(*out_blob));
    for (size_t i = 0; i < sizeof(s_memory_entries) / sizeof(s_memory_entries[0]); i++) {
        if (s_memory_entries[i].valid && s_memory_entries[i].key == track_key) {
            *out_blob = s_memory_entries[i].blob;
            return hot_cue_store_memory_decode(out_blob, sizeof(*out_blob));
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t hot_cue_store_memory_save(uint32_t track_key, const hot_cue_store_memory_blob_t *blob)
{
    if (track_key == 0 || !blob || blob->count > HOT_CUE_STORE_MEMORY_EDITS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(s_memory_entries) / sizeof(s_memory_entries[0]); i++) {
        if (s_memory_entries[i].valid && s_memory_entries[i].key == track_key) {
            memset(&s_memory_entries[i], 0, sizeof(s_memory_entries[i]));
        }
    }
    if (blob->count == 0) {
        return ESP_OK;
    }
    for (size_t i = 0; i < sizeof(s_memory_entries) / sizeof(s_memory_entries[0]); i++) {
        if (!s_memory_entries[i].valid) {
            s_memory_entries[i].key = track_key;
            s_memory_entries[i].blob = *blob;
            s_memory_entries[i].blob.version = HOT_CUE_STORE_MEMORY_VERSION;
            s_memory_entries[i].valid = 1;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
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

/* One prune erases at most this many blobs (6 NVS entries each). */
#define HOT_CUE_STORE_PRUNE_BATCH 24u

static size_t prune_seed_only(const uint32_t *pins, size_t pin_n)
{
    char names[HOT_CUE_STORE_PRUNE_BATCH][16];
    size_t found = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    nvs_iterator_t it = NULL;
    esp_err_t rc = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS, NVS_TYPE_BLOB, &it);
    while (rc == ESP_OK && found < HOT_CUE_STORE_PRUNE_BATCH) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) == ESP_OK && info.key[0] == 'h' && info.key[1] == 'c') {
            hot_cue_store_blob_t raw;
            hot_cue_store_blob_t blob;
            size_t len = sizeof(raw);
            const uint32_t key = (uint32_t)strtoul(info.key + 2, NULL, 16);
            if (!key_is_pinned(pins, pin_n, key) &&
                nvs_get_blob(h, info.key, &raw, &len) == ESP_OK &&
                hot_cue_store_decode(&raw, len, &blob) == ESP_OK &&
                hot_cue_store_blob_is_seed_only(&blob)) {
                memcpy(names[found++], info.key, sizeof(names[0]));
            }
        }
        rc = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    size_t erased = 0;
    for (size_t i = 0; i < found; i++) {
        if (nvs_erase_key(h, names[i]) == ESP_OK) {
            erased++;
        }
    }
    if (erased > 0u) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
    return erased;
}

void hot_cue_store_log_usage(const char *why)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        ESP_LOGW(TAG, "NVS (%s): %u used, %u free, %u total entries, %u namespaces", why,
                 (unsigned)st.used_entries, (unsigned)st.free_entries,
                 (unsigned)st.total_entries, (unsigned)st.namespace_count);
    }
}

/* Writes one blob; on NOT_ENOUGH_SPACE makes room and writes it once more. */
static esp_err_t put_blob(uint32_t track_key, const char *key, const void *data, size_t len,
                          const char *what)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        nvs_handle_t h;
        esp_err_t rc = nvs_open(NS, NVS_READWRITE, &h);
        if (rc != ESP_OK) {
            return rc;
        }
        rc = nvs_set_blob(h, key, data, len);
        if (rc == ESP_OK) {
            rc = nvs_commit(h);
        }
        nvs_close(h);
        if (rc != ESP_ERR_NVS_NOT_ENOUGH_SPACE || attempt > 0) {
            return rc;
        }
        const size_t erased = make_room(track_key);
        hot_cue_store_log_usage(what);
        ESP_LOGW(TAG, "%s: NVS full, pruned %u seed-only hot cue blobs", what,
                 (unsigned)erased);
        if (erased == 0u) {
            return rc;
        }
    }
    return ESP_FAIL;
}

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
    hot_cue_store_normalize(&normalized);

    return put_blob(track_key, key, &normalized, sizeof(normalized), "hot cue save");
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

esp_err_t hot_cue_store_memory_load(uint32_t track_key, hot_cue_store_memory_blob_t *out_blob)
{
    if (track_key == 0 || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_blob, 0, sizeof(*out_blob));
    char key[16];
    ESP_RETURN_ON_ERROR(make_key_prefixed("mc", track_key, key), TAG, "key");

    nvs_handle_t h;
    esp_err_t rc = nvs_open(NS, NVS_READONLY, &h);
    if (rc != ESP_OK) {
        return rc == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : rc;
    }
    size_t len = sizeof(*out_blob);
    rc = nvs_get_blob(h, key, out_blob, &len);
    nvs_close(h);
    if (rc != ESP_OK) {
        memset(out_blob, 0, sizeof(*out_blob));
        return rc == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : rc;
    }
    return hot_cue_store_memory_decode(out_blob, len);
}

esp_err_t hot_cue_store_memory_save(uint32_t track_key, const hot_cue_store_memory_blob_t *blob)
{
    if (track_key == 0 || !blob || blob->count > HOT_CUE_STORE_MEMORY_EDITS_MAX ||
        blob->version != HOT_CUE_STORE_MEMORY_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    ESP_RETURN_ON_ERROR(make_key_prefixed("mc", track_key, key), TAG, "key");

    if (blob->count != 0) {
        return put_blob(track_key, key, blob, sizeof(*blob), "memory cue save");
    }
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
