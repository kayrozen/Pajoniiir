#pragma once

#include <stdint.h>

#if defined(HOT_CUE_STORE_STANDALONE_TEST)
typedef int esp_err_t;
#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_INVALID_ARG  0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND    0x105
#else
#include "esp_err.h"
#endif

#include <stddef.h>

#define HOT_CUE_STORE_SLOT_COUNT 8u
#define HOT_CUE_STORE_TYPE_SINGLE 1u
#define HOT_CUE_STORE_TYPE_LOOP   2u

typedef struct {
    uint32_t pos_ms;
    uint32_t end_ms;
    uint8_t type;
    uint8_t reserved[3];
} hot_cue_store_slot_t;

/* v2 appends the manual cue point to the v1 hot cue blob. A v1 blob still
 * loads, with no cue point. */
typedef struct {
    uint32_t version;
    uint32_t valid_mask;
    hot_cue_store_slot_t slots[HOT_CUE_STORE_SLOT_COUNT];
    uint32_t cue_point_ms;              /* meaningful only with has_cue */
    uint8_t has_cue;
    uint8_t reserved[3];
} hot_cue_store_blob_t;

/* Decodes a stored blob (v1 or v2) as read back from NVS. */
esp_err_t hot_cue_store_decode(const void *raw, size_t len, hot_cue_store_blob_t *out_blob);
esp_err_t hot_cue_store_load(uint32_t track_key, hot_cue_store_blob_t *out_blob);
esp_err_t hot_cue_store_save(uint32_t track_key, const hot_cue_store_blob_t *blob);
esp_err_t hot_cue_store_clear(uint32_t track_key);
