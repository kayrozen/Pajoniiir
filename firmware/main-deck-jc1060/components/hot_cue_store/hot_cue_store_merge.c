/* v303: the blob rules without NVS (hot_cue_store.c keeps the storage), so
 * host tests of deck_core can link the real merge. */
#include "hot_cue_store.h"

#include <string.h>

void hot_cue_store_normalize(hot_cue_store_blob_t *blob)
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
    blob->source_mask &= (uint8_t)blob->valid_mask;
    blob->has_cue = blob->has_cue ? 1u : 0u;
    blob->cue_from_source = blob->has_cue && blob->cue_from_source ? 1u : 0u;
    if (!blob->has_cue) {
        blob->cue_point_ms = 0;
    }
    memset(blob->reserved, 0, sizeof(blob->reserved));
}

void hot_cue_store_merge_source(const hot_cue_store_blob_t *stored,
                                const hot_cue_store_blob_t *seed, bool have_source,
                                bool src_cue, uint32_t src_cue_ms, uint32_t duration_ms,
                                hot_cue_store_blob_t *out)
{
    hot_cue_store_blob_t next = *stored;
    hot_cue_store_normalize(&next);
    if (have_source) {
        const uint8_t local = (uint8_t)(next.valid_mask & ~next.source_mask);
        const uint8_t from = (uint8_t)(seed->valid_mask & ~local);
        for (uint32_t i = 0; i < HOT_CUE_STORE_SLOT_COUNT; i++) {
            const uint8_t bit = (uint8_t)(1u << i);
            if ((local & bit) != 0u) {
                continue;
            }
            if ((from & bit) != 0u) {
                next.slots[i] = seed->slots[i];
            } else {
                memset(&next.slots[i], 0, sizeof(next.slots[i]));
            }
        }
        next.valid_mask = (uint32_t)(local | from);
        next.source_mask = from;
    }
    const bool past_end = next.has_cue && duration_ms != 0u &&
                          next.cue_point_ms >= duration_ms;
    if (past_end || (have_source && next.cue_from_source)) {
        next.has_cue = 0u;
        next.cue_from_source = 0u;
    }
    if (!next.has_cue && have_source && src_cue &&
        (duration_ms == 0u || src_cue_ms < duration_ms)) {
        next.has_cue = 1u;
        next.cue_point_ms = src_cue_ms;
        next.cue_from_source = 1u;
    }
    hot_cue_store_normalize(&next);
    *out = next;
}

/* ── v309: memory cues ─────────────────────────────────────────────────── */

static bool memory_same(const hot_cue_store_memory_t *a, uint32_t pos_ms, uint32_t end_ms)
{
    return a->pos_ms == pos_ms && a->end_ms == end_ms;
}

/* Inserts by time (stable: an equal start goes after); a full list keeps the
 * earliest. Duplicates (same start and end) are not inserted. */
static void memory_insert(hot_cue_store_memory_list_t *list, uint32_t pos_ms, uint32_t end_ms,
                          uint8_t flags)
{
    size_t at = 0;
    while (at < list->count && list->cues[at].pos_ms <= pos_ms) {
        if (memory_same(&list->cues[at], pos_ms, end_ms)) {
            return;
        }
        at++;
    }
    if (at >= HOT_CUE_STORE_MEMORY_MAX) {
        return;
    }
    const size_t n = list->count < HOT_CUE_STORE_MEMORY_MAX ? list->count
                                                           : HOT_CUE_STORE_MEMORY_MAX - 1u;
    memmove(&list->cues[at + 1u], &list->cues[at], (n - at) * sizeof(list->cues[0]));
    list->cues[at] = (hot_cue_store_memory_t) { .pos_ms = pos_ms, .end_ms = end_ms,
                                                .flags = flags };
    list->count = (uint8_t)(n + 1u);
}

static int memory_edit_index(const hot_cue_store_memory_blob_t *edits, uint8_t flag,
                             uint32_t pos_ms, uint32_t end_ms)
{
    for (uint32_t i = 0; i < edits->count; i++) {
        if (edits->edits[i].flags == flag && memory_same(&edits->edits[i], pos_ms, end_ms)) {
            return (int)i;
        }
    }
    return -1;
}

static void memory_edit_drop(hot_cue_store_memory_blob_t *edits, uint32_t index)
{
    memmove(&edits->edits[index], &edits->edits[index + 1u],
            (edits->count - index - 1u) * sizeof(edits->edits[0]));
    edits->count--;
    memset(&edits->edits[edits->count], 0, sizeof(edits->edits[0]));
}

static bool memory_edit_append(hot_cue_store_memory_blob_t *edits, uint8_t flag,
                               uint32_t pos_ms, uint32_t end_ms)
{
    if (edits->count >= HOT_CUE_STORE_MEMORY_EDITS_MAX) {
        return false;
    }
    edits->edits[edits->count++] = (hot_cue_store_memory_t) { .pos_ms = pos_ms,
                                                              .end_ms = end_ms, .flags = flag };
    return true;
}

bool hot_cue_store_memory_merge(hot_cue_store_memory_blob_t *edits,
                                const hot_cue_store_memory_t *source, size_t source_count,
                                bool have_source, uint32_t duration_ms,
                                hot_cue_store_memory_list_t *out)
{
    memset(out, 0, sizeof(*out));
    edits->version = HOT_CUE_STORE_MEMORY_VERSION;
    bool changed = false;
    if (have_source) {
        /* A hidden cue the analysis dropped, or a local one it now has
         * too, is no longer an edit. */
        for (uint32_t i = 0; i < edits->count;) {
            const hot_cue_store_memory_t *e = &edits->edits[i];
            bool in_source = false;
            for (size_t k = 0; source && !in_source && k < source_count; k++) {
                in_source = memory_same(&source[k], e->pos_ms, e->end_ms);
            }
            const bool keep = e->flags == HOT_CUE_STORE_MEMORY_HIDDEN ? in_source
                            : e->flags == HOT_CUE_STORE_MEMORY_LOCAL && !in_source;
            if (keep) {
                i++;
            } else {
                memory_edit_drop(edits, i);
                changed = true;
            }
        }
    }
    const bool past_end_ok = duration_ms == 0u;
    for (uint32_t i = 0; i < edits->count; i++) {
        const hot_cue_store_memory_t *e = &edits->edits[i];
        if (e->flags == HOT_CUE_STORE_MEMORY_LOCAL && (past_end_ok || e->pos_ms < duration_ms)) {
            memory_insert(out, e->pos_ms, e->end_ms, HOT_CUE_STORE_MEMORY_LOCAL);
        }
    }
    for (size_t k = 0; have_source && source && k < source_count; k++) {
        if ((past_end_ok || source[k].pos_ms < duration_ms) &&
            memory_edit_index(edits, HOT_CUE_STORE_MEMORY_HIDDEN, source[k].pos_ms,
                              source[k].end_ms) < 0) {
            memory_insert(out, source[k].pos_ms, source[k].end_ms, 0u);
        }
    }
    return changed;
}

hot_cue_store_memory_add_t hot_cue_store_memory_add(hot_cue_store_memory_blob_t *edits,
                                                    hot_cue_store_memory_list_t *list,
                                                    uint32_t pos_ms, uint32_t end_ms)
{
    for (uint8_t i = 0; i < list->count; i++) {
        if (list->cues[i].pos_ms == pos_ms) {
            return HOT_CUE_STORE_MEMORY_EXISTS;
        }
    }
    if (list->count >= HOT_CUE_STORE_MEMORY_MAX) {
        return HOT_CUE_STORE_MEMORY_FULL;
    }
    edits->version = HOT_CUE_STORE_MEMORY_VERSION;
    const int hidden = memory_edit_index(edits, HOT_CUE_STORE_MEMORY_HIDDEN, pos_ms, end_ms);
    if (hidden >= 0) {
        memory_edit_drop(edits, (uint32_t)hidden);
        memory_insert(list, pos_ms, end_ms, 0u);
        return HOT_CUE_STORE_MEMORY_ADDED;
    }
    if (!memory_edit_append(edits, HOT_CUE_STORE_MEMORY_LOCAL, pos_ms, end_ms)) {
        return HOT_CUE_STORE_MEMORY_FULL;
    }
    memory_insert(list, pos_ms, end_ms, HOT_CUE_STORE_MEMORY_LOCAL);
    return HOT_CUE_STORE_MEMORY_ADDED;
}

bool hot_cue_store_memory_remove(hot_cue_store_memory_blob_t *edits,
                                 hot_cue_store_memory_list_t *list, size_t index)
{
    if (index >= list->count) {
        return false;
    }
    const hot_cue_store_memory_t cue = list->cues[index];
    edits->version = HOT_CUE_STORE_MEMORY_VERSION;
    if (cue.flags == HOT_CUE_STORE_MEMORY_LOCAL) {
        const int local = memory_edit_index(edits, HOT_CUE_STORE_MEMORY_LOCAL, cue.pos_ms,
                                            cue.end_ms);
        if (local >= 0) {
            memory_edit_drop(edits, (uint32_t)local);
        }
    } else if (!memory_edit_append(edits, HOT_CUE_STORE_MEMORY_HIDDEN, cue.pos_ms,
                                   cue.end_ms)) {
        return false;
    }
    memmove(&list->cues[index], &list->cues[index + 1u],
            (list->count - index - 1u) * sizeof(list->cues[0]));
    list->count--;
    memset(&list->cues[list->count], 0, sizeof(list->cues[0]));
    return true;
}

int hot_cue_store_memory_find(const hot_cue_store_memory_list_t *list, uint32_t pos_ms,
                              uint32_t tol_ms)
{
    int best = -1;
    uint32_t best_diff = 0;
    for (uint8_t i = 0; i < list->count; i++) {
        const uint32_t p = list->cues[i].pos_ms;
        const uint32_t diff = p > pos_ms ? p - pos_ms : pos_ms - p;
        if (diff <= tol_ms && (best < 0 || diff < best_diff)) {
            best = i;
            best_diff = diff;
        }
    }
    return best;
}

int hot_cue_store_memory_step(const hot_cue_store_memory_list_t *list, uint32_t pos_ms,
                              uint32_t tol_ms, int dir)
{
    if (dir < 0) {
        for (int i = (int)list->count - 1; i >= 0; i--) {
            if ((uint64_t)list->cues[i].pos_ms + tol_ms < pos_ms) {
                return i;
            }
        }
    } else {
        for (uint8_t i = 0; i < list->count; i++) {
            if (list->cues[i].pos_ms > (uint64_t)pos_ms + tol_ms) {
                return i;
            }
        }
    }
    return -1;
}
