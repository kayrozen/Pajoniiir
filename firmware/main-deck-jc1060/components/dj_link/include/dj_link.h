#pragma once
//
// dj_link — Pioneer Pro DJ Link endpoint, Ethernet only (v246 observer,
// v247 virtual CDJ, v248 peer library browse; docs/DJ_LINK_SPEC.md).
//
// A low-priority task on core 1 owns raw lwIP PCBs bound to the Ethernet
// netif on UDP 50000 (join/keep-alive), 50001 (beat 0x28, position 0x0b) and
// 50002 (CDJ status 0x0a, media query 0x05, load-track 0x19). It decodes with
// the vendored esp-djlink codec, keeps a table of up to DJ_LINK_MAX_PEERS
// players, claims a player number, answers media queries for our USB library
// and hands validated load-track requests to ui_update(). On request it
// browses one peer's USB through that player's TCP dbserver (dj_link_db),
// one connection at a time, into a PSRAM cache the Library reads. v249
// downloads a chosen peer track over NFS into an SD cache file. It never
// touches the audio path, and sends nothing while disabled.
//
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "dj_link_db.h"
#include "dj_link_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ethernet lives in main (eth_bringup), so the network is reached through
 * hooks rather than a component dependency. */
typedef struct {
    bool (*ip_ready)(void);          /* Ethernet has a DHCP lease */
    esp_netif_t *(*eth_netif)(void); /* Ethernet esp_netif, NULL if absent */
} dj_link_config_t;

/* Store the hooks. No task, no PCB, no allocation. Call once before
 * dj_link_set_enabled(). */
esp_err_t dj_link_init(const dj_link_config_t *config);

/* Start (true) or stop (false) the observer. Non-blocking: start creates the
 * task, which opens its PCBs only once ip_ready() holds; stop is a request -
 * the task closes the PCBs, clears the table and deletes itself within
 * ~200 ms. Safe to call from the LVGL task. */
esp_err_t dj_link_set_enabled(bool enable);

/* Last published summary; state OFF when the task is not running. */
void dj_link_get_summary(dj_link_summary_t *out);

/* Library size announced in media responses (0 = no reply). Called by the
 * LVGL task, which may block on the library; the dj_link task never does. */
void dj_link_set_local_track_count(uint32_t count);

/* A validated network load-track (0x19) for a track of our own USB. */
typedef struct {
    uint32_t id;            /* handoff token for dj_link_finish_load_request() */
    uint32_t rekordbox_id;
    uint8_t  from_number;   /* player that sent the command */
} dj_link_load_request_t;

/* ui_update() only. Takes the pending request, if any; the caller must then
 * report the outcome with dj_link_finish_load_request(). The 0x1a ack is sent
 * for accepted loads only; an untaken request is dropped after ~3 s. */
bool dj_link_take_load_request(dj_link_load_request_t *out);
void dj_link_finish_load_request(uint32_t id, bool accepted);

/* v248 peer library browse. The LVGL task selects a player and reads a cache
 * the dj_link task fills from the peer's dbserver; no call here blocks on the
 * network. Rows are identified by (generation, index): a new selection or a
 * restart bumps the generation and invalidates every index. */
#define DJ_LINK_BROWSE_MAX_TRACKS 2000u

typedef enum {
    DJ_LINK_BROWSE_OFF = 0,     /* nothing selected */
    DJ_LINK_BROWSE_WAITING,     /* selected; waiting for our number / its IP */
    DJ_LINK_BROWSE_LOADING,     /* listing */
    DJ_LINK_BROWSE_LISTED,      /* list complete; details on demand */
    DJ_LINK_BROWSE_FAILED,      /* error says why; select again to retry */
} dj_link_browse_state_t;

typedef struct {
    dj_link_browse_state_t state;
    uint8_t  peer;              /* selected player number, 0 = none */
    char     peer_name[DJLINK_NAME_LEN + 1];
    uint32_t total;             /* items the peer reported */
    uint32_t count;             /* rows readable now (<= DJ_LINK_BROWSE_MAX_TRACKS) */
    uint32_t generation;
    uint32_t detail_seq;        /* bumps whenever a row gains metadata */
    char     error[32];
} dj_link_browse_status_t;

/* Browse the USB of player `peer` (0 stops and frees the cache). Selecting
 * the current peer again restarts it (retry after FAILED). */
void dj_link_browse_select(uint8_t peer);
void dj_link_browse_get_status(dj_link_browse_status_t *out);
/* Copy row `index` of `generation`; false if stale or not listed yet. */
bool dj_link_browse_get_track(uint32_t generation, uint32_t index,
                              dj_link_peer_track_t *out);
/* Rows [first, first + count) want duration / BPM (the visible page). */
void dj_link_browse_want_details(uint32_t generation, uint32_t first, uint32_t count);

/* v249 peer track download. The dj_link task fetches the peer's export.pdb
 * over NFS (to find the file path of the rekordbox id), then the audio file,
 * into /sd/djlcache; the UI then loads that local file through its usual
 * path. Nothing is ever streamed to the audio engine from the network. One
 * fetch at a time; the status keeps the last outcome until the next start. */
#define DJ_LINK_FETCH_PATH_MAX 64u

typedef enum {
    DJ_LINK_FETCH_IDLE = 0,
    DJ_LINK_FETCH_PDB,          /* downloading / searching the peer's export.pdb */
    DJ_LINK_FETCH_AUDIO,        /* downloading the audio file */
    DJ_LINK_FETCH_DONE,         /* path holds the cached file */
    DJ_LINK_FETCH_FAILED,       /* error says why */
    DJ_LINK_FETCH_CANCELLED,
} dj_link_fetch_state_t;

typedef struct {
    uint32_t id;                /* from dj_link_fetch_start(), 0 = none yet */
    dj_link_fetch_state_t state;
    uint8_t  peer;
    uint32_t rekordbox_id;
    uint32_t track_key;         /* dj_link_peer_track_key() of the file */
    uint32_t done;              /* bytes of the current file */
    uint32_t total;
    uint8_t  percent;           /* of the current file */
    bool     cache_hit;         /* DONE without downloading */
    char     path[DJ_LINK_FETCH_PATH_MAX];
    char     error[48];
} dj_link_fetch_status_t;

/* Fetch track `rekordbox_id` from the USB of player `peer`. keep_keys (NULL
 * or two entries, 0 = none) name cached files that must survive the cache
 * pruning, e.g. the ones loaded on the decks. Returns the fetch id, or 0 if
 * dj_link is off or a fetch is already running. */
uint32_t dj_link_fetch_start(uint8_t peer, uint32_t rekordbox_id, const uint32_t *keep_keys);
/* Stop fetch `id` (no-op once it is over); the partial file is deleted. */
void dj_link_fetch_cancel(uint32_t id);
void dj_link_fetch_get_status(dj_link_fetch_status_t *out);

#ifdef __cplusplus
}
#endif
