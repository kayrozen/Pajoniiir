#pragma once
//
// dj_link_state — pure peer table for the Pioneer DJ Link observer (v246).
//
// No FreeRTOS, no lwIP, no heap: the dj_link task owns one table, feeds it the
// packets the transport copied out of lwIP, and publishes a summary. Host
// tested in test/host. Observation only - nothing here ever builds a packet.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "djlink/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_MAX_PEERS       6u
#define DJ_LINK_PEER_TIMEOUT_MS 5000u   /* players send status every ~200 ms */

/* Keep-alive byte 0x21 (djlink/claim.h has the mixer and CDJ values). */
#define DJ_LINK_DEVICE_TYPE_REKORDBOX 0x03u

typedef struct {
    bool     in_use;
    uint8_t  device_number;             /* player number (1..6), mixer 33 */
    uint8_t  device_type;               /* v297: keep-alive 0x21, 0 = none heard */
    char     name[DJLINK_NAME_LEN + 1]; /* "CDJ-3000" */
    uint32_t last_seen_ms;
    uint32_t ip;                        /* v248: source IPv4 (host order), 0 = unknown */

    /* CDJ status 0x0a (port 50002). Normally only sent to devices that
     * announce themselves with keep-alives, so a passive observer may never
     * get it: has_status stays false and master/on-air are unknown. */
    bool     has_status;
    bool     master;
    bool     on_air;
    bool     playing;
    bool     synced;
    uint8_t  master_handoff;            /* v305: status 0x9f (Mh), 0xff = none */

    /* Effective BPM (track BPM adjusted by pitch), 0 = unknown. */
    float    bpm;
    float    pitch_pct;

    /* Beat 0x28 (port 50001): rx time of the last beat, 1..4 in bar, and the
     * sender's own "next beat in" interval, used for the beat phase. */
    bool     has_beat;
    uint32_t beat_rx_ms;
    uint8_t  beat_in_bar;
    uint32_t beat_interval_ms;
    float    beat_bpm;      /* v304: effective BPM of the last beat packet (0.01 BPM
                             * track tempo, not the 0.1 BPM of 0x0b), 0 = unknown */

    /* Absolute position 0x0b (port 50001, CDJ-3000 and later). */
    bool     has_position;
    uint32_t position_rx_ms;
    uint32_t playhead_ms;
    uint32_t track_length_s;
} dj_link_peer_t;

typedef struct {
    dj_link_peer_t peers[DJ_LINK_MAX_PEERS];
    uint32_t       rx_accepted;
    uint32_t       rx_dropped;
} dj_link_table_t;

#define DJ_LINK_MAX_PLAYER_NUMBER 6u    /* players 1..6; mixers use 33+ */

/* v248: a player whose media the Library can browse over the dbserver.
 * v297: or a rekordbox library source (collection, no playback traffic). */
typedef struct {
    uint8_t  number;
    char     name[DJLINK_NAME_LEN + 1];
    uint32_t ip;                        /* host order, never 0 here */
    bool     collection;                /* rekordbox: browse the collection slot */
} dj_link_player_t;

typedef enum {
    DJ_LINK_RX_ACCEPTED = 0,
    DJ_LINK_RX_DROPPED,     /* no magic / wrong port / unknown type / bad layout */
} dj_link_rx_t;

typedef enum {
    DJ_LINK_STATE_OFF = 0,
    DJ_LINK_STATE_WAIT_IP,
    DJ_LINK_STATE_LISTENING,
    DJ_LINK_STATE_ERROR,
} dj_link_state_t;

/* What the UI shows. The master is the status-flagged tempo master when one
 * is known (master_confirmed); otherwise the peer that sent the most recent
 * beat, since a passive observer usually only hears beat/position traffic. */
typedef struct {
    dj_link_state_t state;
    uint8_t         peer_count;
    bool            has_master;
    bool            master_confirmed;
    dj_link_peer_t  master;
    uint32_t        rx_accepted;
    uint32_t        rx_dropped;
    uint8_t         our_number;      /* v247: claimed player number, 0 = joining */
    uint8_t         our_number_d2;   /* v298: deck 2's player number, 0 = joining */
    /* v248: players with a known IP, by ascending number. */
    uint8_t          player_count;
    dj_link_player_t players[DJ_LINK_MAX_PEERS];
} dj_link_summary_t;

void dj_link_table_reset(dj_link_table_t *table);

/* Decode one datagram received on `port` (50000, 50001 or 50002). Anything
 * without the DJ Link magic header, arriving on the wrong port, of a type
 * other than keep-alive 0x06 / beat 0x28 / absolute position 0x0b / CDJ
 * status 0x0a, or failing the codec layout checks is dropped without touching
 * the table. v297: keep-alives are taken from players and rekordbox only
 * (mixers are not peers here); a rekordbox source sends nothing else, so they
 * are the only way it ever reaches the table. The caller drops our own. */
dj_link_rx_t dj_link_table_ingest(dj_link_table_t *table, uint16_t port,
                                  const uint8_t *buf, size_t len,
                                  uint32_t now_ms);
/* Same, and remember src_ip (host order) as the sender's address (v248). */
dj_link_rx_t dj_link_table_ingest_from(dj_link_table_t *table, uint16_t port,
                                       const uint8_t *buf, size_t len,
                                       uint32_t src_ip, uint32_t now_ms);

/* Forget peers silent for DJ_LINK_PEER_TIMEOUT_MS. Returns true if any went. */
bool dj_link_table_expire(dj_link_table_t *table, uint32_t now_ms);

void dj_link_table_summarize(const dj_link_table_t *table,
                             dj_link_state_t state,
                             dj_link_summary_t *out);

/* v304: the beat clock network sync follows. anchor_ms is the rx time of the
 * player's last beat packet, period_us its effective beat length. */
typedef struct {
    bool     valid;
    uint8_t  player;
    uint8_t  beat_in_bar;   /* 1..4 at anchor_ms, 0 = unknown */
    uint32_t anchor_ms;
    uint32_t period_us;
} dj_link_beat_clock_t;

#define DJ_LINK_BEAT_CLOCK_STALE_BEATS 2.5f

/* Pick the player to follow: the status-confirmed tempo master, else
 * `prefer` (the one followed so far) while its beats keep coming, else the
 * newest beat. Only players with a fresh beat (within
 * DJ_LINK_BEAT_CLOCK_STALE_BEATS) and not reported stopped qualify, so two
 * unconfirmed players never make the clock hop back and forth. */
void dj_link_table_beat_clock(const dj_link_table_t *table, uint8_t prefer,
                              uint32_t now_ms, dj_link_beat_clock_t *out);

/* Fraction of the current beat elapsed at now_ms, 0..1; -1 if unknown. */
float dj_link_peer_beat_phase(const dj_link_peer_t *peer, uint32_t now_ms);

/* "DJ LINK: OFF", "DJ LINK: ON - CDJ-3000 #1 174.2 BPM", ... */
void dj_link_format_status(const dj_link_summary_t *summary,
                           char *out, size_t cap);

#ifdef __cplusplus
}
#endif
