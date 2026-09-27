#pragma once
//
// dj_link_db — pure (host-testable) client for a peer's rekordbox database
// server (TCP), v248: list the tracks of a peer's USB and fetch per-track
// metadata on demand, so the Library can browse another player's media.
//
// Sans-I/O: the client never touches a socket. The owner supplies connect /
// send / close hooks and feeds back connection events and received bytes;
// the client builds every request with the esp-djlink dbserver codec and
// reports tracks through callbacks. dj_link.c drives it from the dj_link task
// over a raw lwIP TCP PCB; test/host drives it over real localhost sockets
// against a mock peer.
//
// Flow (djl-analysis track_metadata.html, "Database Server"):
//   connect peer:12523 -> "RemoteDBServer" port query -> u16 port -> close
//   connect peer:port  -> greeting 11 00 00 00 01 (echoed)
//   context setup (type 0, our player number) -> 0x4000
//   all-tracks menu 0x1004 (DMST, sort) -> 0x4000 [.., item count]
//   render 0x3000 in batches -> 0x4001 header, 0x4101 items, 0x4201 footer
//   per track on demand: metadata 0x2002 (DMST, id) -> 0x4000 -> render
// One connection, one request in flight. The dbserver only serves metadata;
// v249 fetches the audio over NFS (esp-djlink nfs.h, dj_link_fetch_*), so
// the owner marks listed tracks DJ_LINK_PEER_AUDIO_NFS when that path exists.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_DB_TITLE_MAX        80u   /* UTF-8 bytes incl. NUL */
#define DJ_LINK_DB_ARTIST_MAX       64u
#define DJ_LINK_DB_STEP_TIMEOUT_MS  3000u /* per connect / reply */
#define DJ_LINK_DB_IDLE_CLOSE_MS    20000u
#define DJ_LINK_DB_RENDER_BATCH     64u
#define DJ_LINK_DB_RX_MAX           4096u /* largest single reply buffered */
#define DJ_LINK_DB_TX_MAX           128u

/* Menu item types (arg 6) used here. */
#define DJ_LINK_DB_ITEM_TITLE       0x0004u
#define DJ_LINK_DB_ITEM_ARTIST      0x0007u
#define DJ_LINK_DB_ITEM_DURATION    0x000bu
#define DJ_LINK_DB_ITEM_TEMPO       0x000du
#define DJ_LINK_DB_ITEM_TITLE_ARTIST 0x0704u

#define DJ_LINK_DB_TYPE_TRACK_MENU  0x1004u /* all tracks, arg: sort order */

typedef enum {
    /* dbserver metadata only: no way to reach the audio. */
    DJ_LINK_PEER_AUDIO_METADATA_ONLY = 0,
    /* v249: downloadable from the peer's NFS export to the SD cache. */
    DJ_LINK_PEER_AUDIO_NFS,
} dj_link_peer_audio_t;

typedef struct {
    uint32_t rekordbox_id;
    char     title[DJ_LINK_DB_TITLE_MAX];
    char     artist[DJ_LINK_DB_ARTIST_MAX];
    uint16_t duration_s;    /* 0 = unknown (detail not fetched) */
    uint16_t bpm100;        /* 0 = unknown */
    bool     has_detail;    /* metadata request done (even if it failed) */
    dj_link_peer_audio_t audio;
} dj_link_peer_track_t;

typedef enum {
    DJ_LINK_DB_IDLE = 0,        /* no connection */
    DJ_LINK_DB_DISC_CONNECTING,
    DJ_LINK_DB_DISC_WAIT_PORT,
    DJ_LINK_DB_SRV_CONNECTING,
    DJ_LINK_DB_SRV_WAIT_GREETING,
    DJ_LINK_DB_SRV_WAIT_SETUP,
    DJ_LINK_DB_READY,           /* session open, nothing in flight */
    DJ_LINK_DB_LIST_WAIT_AVAIL,
    DJ_LINK_DB_LIST_WAIT_RENDER,
    DJ_LINK_DB_DETAIL_WAIT_AVAIL,
    DJ_LINK_DB_DETAIL_WAIT_RENDER,
    DJ_LINK_DB_FAILED,          /* closed; error() says why; restart to retry */
} dj_link_db_phase_t;

typedef struct {
    /* Transport. connect starts an asynchronous connect (0 = started) and
     * the owner later calls dj_link_db_on_connected() or _on_closed(). */
    int  (*connect)(void *ctx, uint32_t ip, uint16_t port);
    int  (*send)(void *ctx, const uint8_t *buf, size_t len);   /* 0 = queued */
    void (*close)(void *ctx);
    /* Results. list_begin: item count the peer reported (before capping);
     * track: row `index` listed (detail false) or its metadata (true). */
    void (*list_begin)(void *ctx, uint32_t total);
    void (*track)(void *ctx, uint32_t index, const dj_link_peer_track_t *t, bool detail);
    /* Next row wanting metadata: its index and rekordbox id, false if none. */
    bool (*next_detail)(void *ctx, uint32_t *index, uint32_t *rekordbox_id);
    void *ctx;
} dj_link_db_io_t;

typedef struct {
    dj_link_db_io_t io;
    uint16_t discovery_port;    /* 12523; host tests use an ephemeral port */
    uint32_t step_timeout_ms;
    uint32_t max_tracks;        /* list cap (the owner's buffer size) */
    dj_link_peer_audio_t audio; /* stamped on every listed track */

    uint32_t peer_ip;           /* host order */
    uint8_t  peer_number;
    uint8_t  slot;              /* DJLINK_SLOT_USB */
    uint8_t  our_number;

    dj_link_db_phase_t phase;
    uint32_t deadline_ms;
    uint32_t idle_since_ms;
    uint32_t txid;
    bool     list_wanted;
    bool     list_done;
    uint32_t list_total;        /* reported by the peer */
    uint32_t list_target;       /* min(total, max_tracks) */
    uint32_t render_offset;
    uint32_t render_count;
    uint32_t render_seen;
    uint32_t detail_index;
    uint32_t detail_count;
    dj_link_peer_track_t detail;
    char     error[32];

    size_t   rx_len;
    uint8_t  rx[DJ_LINK_DB_RX_MAX];
    uint8_t  tx[DJ_LINK_DB_TX_MAX];
} dj_link_db_t;

/* Reset to IDLE with the given hooks (no I/O). */
void dj_link_db_init(dj_link_db_t *c, const dj_link_db_io_t *io, uint32_t max_tracks);

/* Browse `slot` of player `peer_number` at peer_ip (host order), querying as
 * player our_number. Closes any previous session, then connects and lists. */
void dj_link_db_start(dj_link_db_t *c, uint32_t peer_ip, uint8_t peer_number,
                      uint8_t slot, uint8_t our_number, uint32_t now_ms);
/* Close the connection (if any) and go IDLE. */
void dj_link_db_stop(dj_link_db_t *c);

void dj_link_db_on_connected(dj_link_db_t *c, uint32_t now_ms);
void dj_link_db_on_data(dj_link_db_t *c, const uint8_t *buf, size_t len, uint32_t now_ms);
/* Remote close or connection error. */
void dj_link_db_on_closed(dj_link_db_t *c, uint32_t now_ms);
/* Timeouts, idle close, and starting the next detail request. */
void dj_link_db_poll(dj_link_db_t *c, uint32_t now_ms);

dj_link_db_phase_t dj_link_db_phase(const dj_link_db_t *c);
bool dj_link_db_list_done(const dj_link_db_t *c);
const char *dj_link_db_error(const dj_link_db_t *c);

/* Byte length of the complete dbserver message at buf, 0 if more bytes are
 * needed, -1 if it can never parse (bad magic / field / too large). */
int dj_link_db_msg_size(const uint8_t *buf, size_t len);

/* UTF-16BE (NUL-terminated or not) to UTF-8, always NUL-terminated;
 * characters outside the BMP become '?'. */
void dj_link_db_utf16be_to_utf8(const uint8_t *in, size_t in_len, char *out, size_t cap);

typedef enum {
    DJ_LINK_PEER_LOAD_OK = 0,           /* audio reachable (NFS fetch) */
    DJ_LINK_PEER_LOAD_METADATA_ONLY,    /* no audio path for peer media */
    DJ_LINK_PEER_LOAD_NO_TRACK,
} dj_link_peer_load_t;

/* Can this peer track be loaded into a deck? Fills a one-line reason. */
dj_link_peer_load_t dj_link_peer_load_check(const dj_link_peer_track_t *t,
                                            char *reason, size_t cap);

/* Stable nonzero key of a peer track (FNV-1a of ip, player number and
 * rekordbox id): names its cache file and marks it loaded in the Library. */
uint32_t dj_link_peer_track_key(uint32_t peer_ip, uint8_t peer_number, uint32_t rekordbox_id);

#ifdef __cplusplus
}
#endif
