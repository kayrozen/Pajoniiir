#include "dj_link_neighbor.h"

#include <stdio.h>
#include <string.h>

void dj_link_nb_reset(dj_link_nb_cache_t *c)
{
    memset(c, 0, sizeof(*c));
}

/* Players whose track a dbserver can describe: USB, SD, or a rekordbox
 * library. A CD has nothing to ask. */
static bool nb_wanted(const dj_link_peer_t *p)
{
    return p->device_number >= 1u && p->device_number <= DJ_LINK_MAX_PLAYER_NUMBER &&
           p->has_status && p->rekordbox_id != 0u && p->source_device != 0u &&
           (p->source_slot == 2u /* SD */ || p->source_slot == 3u /* USB */ ||
            p->source_slot == 4u /* rekordbox */);
}

static void nb_restart(dj_link_nb_slot_t *s, const dj_link_peer_t *p)
{
    const uint32_t gen = s->gen + 1u;
    memset(s, 0, sizeof(*s));
    s->gen = gen;
    s->number = p->device_number;
    s->source_device = p->source_device;
    s->source_slot = p->source_slot;
    s->rekordbox_id = p->rekordbox_id;
    s->state = DJ_LINK_NB_WANT_META;
}

bool dj_link_nb_sync(dj_link_nb_cache_t *c, const dj_link_table_t *table)
{
    bool changed = false;
    /* Drop what the table no longer backs. */
    for (size_t i = 0; i < DJ_LINK_NEIGHBOR_MAX; i++) {
        dj_link_nb_slot_t *s = &c->slot[i];
        if (s->number == 0u) {
            continue;
        }
        bool keep = false;
        for (size_t k = 0; k < DJ_LINK_MAX_PEERS; k++) {
            const dj_link_peer_t *p = &table->peers[k];
            if (p->in_use && p->device_number == s->number && nb_wanted(p)) {
                keep = true;
                break;
            }
        }
        if (!keep) {
            const uint32_t gen = s->gen + 1u;
            memset(s, 0, sizeof(*s));
            s->gen = gen;
            changed = true;
        }
    }
    /* Follow by ascending player number. */
    for (unsigned n = 1; n <= DJ_LINK_MAX_PLAYER_NUMBER; n++) {
        const dj_link_peer_t *p = NULL;
        for (size_t k = 0; k < DJ_LINK_MAX_PEERS; k++) {
            if (table->peers[k].in_use && table->peers[k].device_number == n) {
                p = &table->peers[k];
                break;
            }
        }
        if (!p || !nb_wanted(p)) {
            continue;
        }
        dj_link_nb_slot_t *own = NULL, *free_slot = NULL;
        for (size_t i = 0; i < DJ_LINK_NEIGHBOR_MAX; i++) {
            if (c->slot[i].number == n) {
                own = &c->slot[i];
            } else if (c->slot[i].number == 0u && !free_slot) {
                free_slot = &c->slot[i];
            }
        }
        if (own) {
            if (own->source_device != p->source_device || own->source_slot != p->source_slot ||
                own->rekordbox_id != p->rekordbox_id) {
                nb_restart(own, p);
                changed = true;
            }
        } else if (free_slot) {
            nb_restart(free_slot, p);
            changed = true;
        }
    }
    return changed;
}

bool dj_link_nb_next(const dj_link_nb_cache_t *c, uint32_t now_ms, dj_link_nb_req_t *out)
{
    const dj_link_nb_slot_t *best = NULL;
    for (size_t i = 0; i < DJ_LINK_NEIGHBOR_MAX; i++) {
        const dj_link_nb_slot_t *s = &c->slot[i];
        if ((s->state != DJ_LINK_NB_WANT_META && s->state != DJ_LINK_NB_WANT_WAVE) ||
            (int32_t)(now_ms - s->retry_at_ms) < 0) {
            continue;
        }
        if (!best || s->number < best->number) {
            best = s;
        }
    }
    if (!best) {
        return false;
    }
    out->number = best->number;
    out->device = best->source_device;
    out->slot = best->source_slot;
    out->rekordbox_id = best->rekordbox_id;
    out->want_wave = best->state == DJ_LINK_NB_WANT_WAVE;
    return true;
}

/* The slot an answer is for, NULL if the player moved on. */
static dj_link_nb_slot_t *nb_match(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req)
{
    for (size_t i = 0; i < DJ_LINK_NEIGHBOR_MAX; i++) {
        dj_link_nb_slot_t *s = &c->slot[i];
        if (s->number == req->number && s->source_device == req->device &&
            s->source_slot == req->slot && s->rekordbox_id == req->rekordbox_id) {
            return s;
        }
    }
    return NULL;
}

void dj_link_nb_on_meta(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req,
                        const dj_link_peer_track_t *meta)
{
    dj_link_nb_slot_t *s = nb_match(c, req);
    if (!s || s->state != DJ_LINK_NB_WANT_META) {
        return;
    }
    if (!meta) {
        dj_link_nb_on_fail(c, req, 0u);
        return;
    }
    snprintf(s->title, sizeof(s->title), "%s", meta->title);
    snprintf(s->artist, sizeof(s->artist), "%s", meta->artist);
    s->duration_s = meta->duration_s;
    s->bpm100 = meta->bpm100;
    s->has_meta = true;
    s->state = DJ_LINK_NB_WANT_WAVE;
    s->tries = 0u;
    s->retry_at_ms = 0u;
    s->gen++;
}

void dj_link_nb_on_wave(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req,
                        const uint8_t *detail, size_t len)
{
    dj_link_nb_slot_t *s = nb_match(c, req);
    if (!s || s->state != DJ_LINK_NB_WANT_WAVE) {
        return;
    }
    s->has_wave = len > 0u && detail && dj_link_anlz_preview(detail, len, s->wave);
    if (!s->has_wave) {
        memset(s->wave, 0, sizeof(s->wave));
    }
    s->state = DJ_LINK_NB_DONE;
    s->gen++;
}

void dj_link_nb_on_fail(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req, uint32_t now_ms)
{
    dj_link_nb_slot_t *s = nb_match(c, req);
    if (!s || (s->state != DJ_LINK_NB_WANT_META && s->state != DJ_LINK_NB_WANT_WAVE)) {
        return;
    }
    if (++s->tries >= DJ_LINK_NEIGHBOR_TRIES) {
        /* Metadata is the point; a waveform that never comes leaves a done
         * slot with the text only. */
        s->state = s->state == DJ_LINK_NB_WANT_WAVE ? DJ_LINK_NB_DONE : DJ_LINK_NB_GAVE_UP;
        s->gen++;
        return;
    }
    s->retry_at_ms = now_ms + DJ_LINK_NEIGHBOR_RETRY_MS;
}

const dj_link_nb_slot_t *dj_link_nb_find(const dj_link_nb_cache_t *c, uint8_t number)
{
    for (size_t i = 0; i < DJ_LINK_NEIGHBOR_MAX; i++) {
        if (number != 0u && c->slot[i].number == number) {
            return &c->slot[i];
        }
    }
    return NULL;
}

bool dj_link_nb_position_ms(const dj_link_peer_t *p, uint32_t now_ms, uint32_t *out_ms,
                            bool *estimated)
{
    *estimated = false;
    if (p->has_position) {
        uint32_t ms = p->playhead_ms;
        if (p->playing) {
            ms += now_ms - p->position_rx_ms;   /* the packet is ~30 ms apart: carry on */
        }
        *out_ms = ms;
        return true;
    }
    /* The beat counter counts beats since the first one of the grid: close to
     * the elapsed time at the effective BPM, within a first-beat offset. */
    if (p->beat_count > 0u && p->bpm > 0.0f) {
        *out_ms = (uint32_t)(((float)(p->beat_count - 1u) * 60000.0f) / p->bpm);
        *estimated = true;
        return true;
    }
    return false;
}
