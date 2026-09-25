#include "ui_hot_cue_view.h"

#include <stdbool.h>
#include <stddef.h>

_Static_assert(DECK_CORE_HOT_CUE_SLOT_COUNT == ANLZ_MAX_CUES,
               "hot cue store and ANLZ slot counts must match");

static bool anlz_cue_for_slot(const anlz_metadata_t *meta,
                              uint8_t slot,
                              anlz_cue_t *out)
{
    if (!meta) {
        return false;
    }
    uint8_t count = meta->cue_count <= ANLZ_MAX_CUES ? meta->cue_count
                                                     : (uint8_t)ANLZ_MAX_CUES;
    for (uint8_t j = 0; j < count; j++) {
        if (meta->cues[j].index == slot) {
            *out = meta->cues[j];
            return true;
        }
    }
    return false;
}

uint8_t ui_hot_cue_view_merge(const deck_core_hot_cues_t *store,
                              const anlz_metadata_t *meta,
                              anlz_cue_t out[ANLZ_MAX_CUES])
{
    uint8_t count = 0;
    if (!out) {
        return 0;
    }
    for (uint8_t slot = 0; slot < ANLZ_MAX_CUES; slot++) {
        anlz_cue_t cue;
        if (store && store->known && (store->valid_mask & (1u << slot)) != 0u) {
            const deck_core_hot_cue_slot_t *s = &store->slots[slot];
            cue.index = slot;
            cue.start_ms = s->pos_ms;
            if (s->loop && s->end_ms > s->pos_ms) {
                cue.type = ANLZ_CUE_LOOP;
                cue.end_ms = s->end_ms;
            } else {
                cue.type = ANLZ_CUE_SINGLE;
                cue.end_ms = 0;
            }
        } else if (!anlz_cue_for_slot(meta, slot, &cue)) {
            continue;
        }
        cue.index = slot;
        out[count++] = cue;
    }
    return count;
}
