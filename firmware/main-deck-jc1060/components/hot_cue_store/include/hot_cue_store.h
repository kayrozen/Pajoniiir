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

#include <stdbool.h>
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
 * loads, with no cue point. v303 takes two of the reserved bytes for the
 * origin of each cue: a set bit / flag means it came from the track's
 * analysis (Rekordbox or a DJ Link peer), clear means the user made it on
 * the deck. Older blobs read back as all-local. */
typedef struct {
    uint32_t version;
    uint32_t valid_mask;
    hot_cue_store_slot_t slots[HOT_CUE_STORE_SLOT_COUNT];
    uint32_t cue_point_ms;              /* meaningful only with has_cue */
    uint8_t has_cue;
    uint8_t source_mask;                /* v303: slots seeded from the analysis */
    uint8_t cue_from_source;            /* v303: cue point seeded from the analysis */
    uint8_t reserved[1];
} hot_cue_store_blob_t;

/* Decodes a stored blob (v1 or v2) as read back from NVS. */
esp_err_t hot_cue_store_decode(const void *raw, size_t len, hot_cue_store_blob_t *out_blob);
esp_err_t hot_cue_store_load(uint32_t track_key, hot_cue_store_blob_t *out_blob);
esp_err_t hot_cue_store_save(uint32_t track_key, const hot_cue_store_blob_t *blob);
esp_err_t hot_cue_store_clear(uint32_t track_key);

/* Blob layout version written by hot_cue_store_normalize. */
#define HOT_CUE_STORE_VERSION 2u

/* Canonical form: masks clipped, unused slots / fields zeroed, version set. */
void hot_cue_store_normalize(hot_cue_store_blob_t *blob);

/* v303: the cues a track load keeps. Local cues (pads, CUE button) always
 * win. A slot or cue point that came from the analysis follows it, so an
 * edit at the source reaches the deck on the next load; a source cue that
 * is gone from the analysis is dropped. have_source false (the analysis
 * carries no cue lists, or there is none) keeps the stored cues as they
 * are. seed holds the analysis hot cues (valid_mask + slots), src_cue /
 * src_cue_ms its memory cue; a cue point at or past duration_ms (0 =
 * unknown) is dropped. Pure: the caller loads and saves. */
void hot_cue_store_merge_source(const hot_cue_store_blob_t *stored,
                                const hot_cue_store_blob_t *seed, bool have_source,
                                bool src_cue, uint32_t src_cue_ms, uint32_t duration_ms,
                                hot_cue_store_blob_t *out);

/* v309: memory cues. The analysis memory cues (Rekordbox USB, DJ Link peer)
 * are read again on every load; NVS only keeps the edits made on the deck:
 * a cue added with MEMORY (LOCAL) and an analysis cue removed with DELETE
 * (HIDDEN, matched by start and end). An edits blob lives under its own key,
 * so the hot cue blob above keeps its layout. */
#define HOT_CUE_STORE_MEMORY_MAX       16u   /* memory cues on a deck */
#define HOT_CUE_STORE_MEMORY_EDITS_MAX 32u   /* 16 local + 16 hidden */
#define HOT_CUE_STORE_MEMORY_VERSION   1u
#define HOT_CUE_STORE_MEMORY_LOCAL     0x01u
#define HOT_CUE_STORE_MEMORY_HIDDEN    0x02u

typedef struct {
    uint32_t pos_ms;
    uint32_t end_ms;                    /* loop end; 0 = a point */
    uint8_t flags;                      /* HOT_CUE_STORE_MEMORY_* (0 = analysis) */
    uint8_t reserved[3];
} hot_cue_store_memory_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    hot_cue_store_memory_t edits[HOT_CUE_STORE_MEMORY_EDITS_MAX];
} hot_cue_store_memory_blob_t;

/* The memory cues of a deck by time: analysis cues (flags 0) and local
 * ones (LOCAL). */
typedef struct {
    uint8_t count;
    hot_cue_store_memory_t cues[HOT_CUE_STORE_MEMORY_MAX];
} hot_cue_store_memory_list_t;

typedef enum {
    HOT_CUE_STORE_MEMORY_ADDED = 0,
    HOT_CUE_STORE_MEMORY_EXISTS,        /* a cue already starts there */
    HOT_CUE_STORE_MEMORY_FULL,
} hot_cue_store_memory_add_t;

/* Decodes in place a blob read back from NVS (len = bytes read). */
esp_err_t hot_cue_store_memory_decode(hot_cue_store_memory_blob_t *blob, size_t len);
/* Not found = no edits. The caller's blob avoids a 400-byte stack copy. */
esp_err_t hot_cue_store_memory_load(uint32_t track_key, hot_cue_store_memory_blob_t *out_blob);
/* An empty blob erases the key. */
esp_err_t hot_cue_store_memory_save(uint32_t track_key, const hot_cue_store_memory_blob_t *blob);

/* Pure rules (hot_cue_store_merge.c). */

/* The deck's list for a load: the stored local cues plus the analysis cues
 * (source, by time, at most source_count) that are not hidden, by time,
 * duplicates (same start and end) once, cues at or past duration_ms (0 =
 * unknown) dropped, the earliest HOT_CUE_STORE_MEMORY_MAX kept. have_source
 * false (no cue lists in the analysis) ignores source. With have_source,
 * edits is pruned in place of hidden cues the analysis no longer has and of
 * local cues it now has too; returns true when it changed and should be
 * saved. */
bool hot_cue_store_memory_merge(hot_cue_store_memory_blob_t *edits,
                                const hot_cue_store_memory_t *source, size_t source_count,
                                bool have_source, uint32_t duration_ms,
                                hot_cue_store_memory_list_t *out);
/* MEMORY: adds [pos_ms, end_ms) to the list and records the edit. A hidden
 * analysis cue with the same start and end is un-hidden instead. */
hot_cue_store_memory_add_t hot_cue_store_memory_add(hot_cue_store_memory_blob_t *edits,
                                                    hot_cue_store_memory_list_t *list,
                                                    uint32_t pos_ms, uint32_t end_ms);
/* DELETE: removes list->cues[index]; a local cue leaves the edits, an
 * analysis cue is hidden. False (nothing changed) when index is out of
 * range or the edits are full. */
bool hot_cue_store_memory_remove(hot_cue_store_memory_blob_t *edits,
                                 hot_cue_store_memory_list_t *list, size_t index);
/* Index of the cue starting within tol_ms of pos_ms (nearest), else -1. */
int hot_cue_store_memory_find(const hot_cue_store_memory_list_t *list, uint32_t pos_ms,
                              uint32_t tol_ms);
/* CALL: the last cue starting before pos_ms - tol_ms (dir < 0) or the first
 * one starting after pos_ms + tol_ms (dir > 0); -1 when there is none. */
int hot_cue_store_memory_step(const hot_cue_store_memory_list_t *list, uint32_t pos_ms,
                              uint32_t tol_ms, int dir);
