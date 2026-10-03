/* v309: in-RAM memory cue edits for the jc1060 deck_core host tests, beside
 * deck_core_dual/hot_cue_store_stub.c (hot cues). */
#include "hot_cue_store.h"

#include <stdbool.h>
#include <string.h>

esp_err_t hot_cue_memory_stub_save_result;
int hot_cue_memory_stub_save_count;

static struct {
    uint32_t key;
    hot_cue_store_memory_blob_t blob;
    bool valid;
} s_memory[4];

void hot_cue_memory_stub_reset(void)
{
    memset(s_memory, 0, sizeof(s_memory));
    hot_cue_memory_stub_save_result = ESP_OK;
    hot_cue_memory_stub_save_count = 0;
}

esp_err_t hot_cue_store_memory_load(uint32_t track_key, hot_cue_store_memory_blob_t *out_blob)
{
    if (track_key == 0 || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_blob, 0, sizeof(*out_blob));
    for (size_t i = 0; i < sizeof(s_memory) / sizeof(s_memory[0]); i++) {
        if (s_memory[i].valid && s_memory[i].key == track_key) {
            *out_blob = s_memory[i].blob;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t hot_cue_store_memory_save(uint32_t track_key, const hot_cue_store_memory_blob_t *blob)
{
    if (track_key == 0 || !blob) {
        return ESP_ERR_INVALID_ARG;
    }
    hot_cue_memory_stub_save_count++;
    if (hot_cue_memory_stub_save_result != ESP_OK) {
        return hot_cue_memory_stub_save_result;
    }
    for (size_t i = 0; i < sizeof(s_memory) / sizeof(s_memory[0]); i++) {
        if (s_memory[i].valid && s_memory[i].key == track_key) {
            s_memory[i].valid = false;
        }
    }
    if (blob->count == 0) {
        return ESP_OK;
    }
    for (size_t i = 0; i < sizeof(s_memory) / sizeof(s_memory[0]); i++) {
        if (!s_memory[i].valid) {
            s_memory[i].key = track_key;
            s_memory[i].blob = *blob;
            s_memory[i].valid = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
