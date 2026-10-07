#pragma once
//
// dj_link_neighbor (v338) - what the DJ Link players on the network have
// loaded: title, artist, duration and a 400-column waveform preview, fetched
// from the dbserver of the device the track came from, and kept per player.
// Pure bookkeeping (host tested): dj_link.c owns the session that does the
// asking and feeds the answers in. The player's own status packet gives
// BPM, play state and position (dj_link_peer_t); this cache holds only what
// needs a dbserver round trip.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dj_link_anlz.h"
#include "dj_link_db.h"
#include "dj_link_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_NEIGHBOR_MAX       4u      /* players followed (= DJ_LINK_ROWS) */
#define DJ_LINK_NEIGHBOR_TRIES     3u      /* a request failing this often is dropped */
#define DJ_LINK_NEIGHBOR_RETRY_MS  4000u

typedef enum {
    DJ_LINK_NB_EMPTY = 0,
    DJ_LINK_NB_WANT_META,
    DJ_LINK_NB_WANT_WAVE,
    DJ_LINK_NB_DONE,
    DJ_LINK_NB_GAVE_UP,         /* until the player loads another track */
} dj_link_nb_state_t;

typedef struct {
    uint8_t  number;            /* player, 0 = free slot */
    /* The key: the track as the player reports it. */
    uint8_t  source_device;
    uint8_t  source_slot;
    uint32_t rekordbox_id;

    dj_link_nb_state_t state;
    uint8_t  tries;
    uint32_t retry_at_ms;
    uint32_t gen;               /* bumps whenever title / artist / wave change */

    bool     has_meta;
    bool     has_wave;
    char     title[DJ_LINK_DB_TITLE_MAX];
    char     artist[DJ_LINK_DB_ARTIST_MAX];
    uint16_t duration_s;        /* 0 = unknown */
    uint16_t bpm100;            /* the track's own BPM, 0 = unknown */
    uint8_t  wave[DJ_LINK_ANLZ_PREVIEW_LEN];
} dj_link_nb_slot_t;

typedef struct {
    dj_link_nb_slot_t slot[DJ_LINK_NEIGHBOR_MAX];
} dj_link_nb_cache_t;

/* One request for the session: ask `device`'s `slot` about `rekordbox_id`
 * for player `number`. want_wave false = metadata, true = the waveform. */
typedef struct {
    uint8_t  number;
    uint8_t  device;
    uint8_t  slot;
    uint32_t rekordbox_id;
    bool     want_wave;
} dj_link_nb_req_t;

void dj_link_nb_reset(dj_link_nb_cache_t *c);

/* Follow the table: a player with a track on a dbserver source (USB, SD or a
 * rekordbox library) gets a slot, a changed track restarts its slot, a
 * player that went away or unloaded frees it. At most DJ_LINK_NEIGHBOR_MAX,
 * lowest numbers first. True if any slot changed. */
bool dj_link_nb_sync(dj_link_nb_cache_t *c, const dj_link_table_t *table);

/* The next request that may run at now_ms (retries wait), lowest player
 * first, metadata before waveform. False if there is none. */
bool dj_link_nb_next(const dj_link_nb_cache_t *c, uint32_t now_ms, dj_link_nb_req_t *out);

/* Answers. A stale one (the player moved on to another track) is ignored.
 * meta NULL / on_fail count as a failure; the waveform answer with len 0 is
 * "the source has none": the slot is done without a preview. detail is the
 * PWV3 blob of the waveform request. */
void dj_link_nb_on_meta(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req,
                        const dj_link_peer_track_t *meta);
void dj_link_nb_on_wave(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req,
                        const uint8_t *detail, size_t len);
void dj_link_nb_on_fail(dj_link_nb_cache_t *c, const dj_link_nb_req_t *req, uint32_t now_ms);

const dj_link_nb_slot_t *dj_link_nb_find(const dj_link_nb_cache_t *c, uint8_t number);

/* Elapsed time of a player's track: the absolute position (0x0b) when it
 * sends one, else estimated from the beat counter and the effective BPM
 * (*estimated set); false if neither is known. */
bool dj_link_nb_position_ms(const dj_link_peer_t *p, uint32_t now_ms, uint32_t *out_ms,
                            bool *estimated);

#ifdef __cplusplus
}
#endif
