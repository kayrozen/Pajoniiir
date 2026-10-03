#pragma once
//
// dj_link_session — pure (host-testable) virtual-CDJ logic, v247:
//   * port 50000 join: announcement 0x0a, claims 0x00/0x02/0x04 (3x each at
//     ~300 ms), auto device number avoiding numbers in use, re-claim on a
//     0x08 conflict or a keep-alive carrying our number, then keep-alive 0x06
//     every 2.0 s;
//   * media query 0x05 -> media response 0x06 announcing our USB slot
//     (reply only, per-querier rate limit);
//   * incoming load-track 0x19 validation (source must be our own USB until
//     the NFS/dbserver client lands in v248+) and the 0x1a ack;
//   * v298: one session per deck. The deck-2 session joins once deck 1 holds
//     a number and never takes it (sibling); both send keep-alives from the
//     same MAC/IP, like the two players of an XDJ-XZ. Each one broadcasts a
//     CDJ status 0x0a for its deck (dj_link_session_status), which is what
//     rekordbox-side tools (vynull, beat-link) list as players.
//   * v308: the two decks' numbers follow the deck order (deck 1 below
//     deck 2), as vynull / beat-link list players by number: deck 1 claims
//     the low number of the highest free pair (3/4, else 2/3, 1/2, 5/6, 4/5)
//     and deck 2 the next free one above it (dj_link_session_set_pair).
//   * v301: each deck with a track also sends the absolute position 0x0b
//     every DJ_LINK_POSITION_MS and a beat 0x28 when its playhead crosses a
//     beat of its grid, both on port 50001 (dj_link_session_position /
//     dj_link_session_beat). The playhead is extrapolated from the UI's last
//     report (dj_link_deck_playhead).
// No I/O here: dj_link.c owns the PCBs and sends what these functions build.
//
// Byte layouts follow the Deep Symmetry DJ Link analysis (startup.html,
// media.html, loading_tracks.html). Note for esp-djlink 0.2.0: the load-track
// field it names target_player (byte 0x28) is the SOURCE player of the track;
// the target is the player the packet is unicast to.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_CLAIM_STEP_MS        300u
#define DJ_LINK_KEEPALIVE_MS         2000u
#define DJ_LINK_DEVICE_TIMEOUT_MS    5000u  /* a number unheard this long is free */
#define DJ_LINK_MEDIA_REPLY_MIN_MS   500u   /* per querier IP */
#define DJ_LINK_SEEN_MAX             8u
#define DJ_LINK_REPLY_LIMIT_SLOTS    4u
#define DJ_LINK_SESSION_NAME_MAX     20u
#define DJ_LINK_STATUS_MS            200u   /* CDJ status period */
#define DJ_LINK_POSITION_MS          30u    /* absolute position period (v301) */
#define DJ_LINK_REPORT_BEATS         16u    /* grid window in a deck report */
#define DJ_LINK_REPORT_HOLD_MS       1000u  /* longest extrapolation of a report */
#define DJ_LINK_BEAT_SEEK_MS         1000u  /* a larger playhead jump is a seek */

typedef enum {
    DJ_LINK_CLAIM_IDLE = 0,
    DJ_LINK_CLAIM_ANNOUNCE,   /* 0x0a x3 */
    DJ_LINK_CLAIM_STAGE1,     /* 0x00 x3 (MAC) */
    DJ_LINK_CLAIM_STAGE2,     /* 0x02 x3 (IP + wanted number) */
    DJ_LINK_CLAIM_FINAL,      /* 0x04 x3, cut short by a 0x05 */
    DJ_LINK_CLAIM_ACTIVE,     /* keep-alives */
    DJ_LINK_CLAIM_NO_NUMBER,  /* players 1..6 all taken; retried */
} dj_link_claim_phase_t;

typedef struct {
    uint8_t  number;
    uint32_t last_ms;
} dj_link_seen_t;

typedef struct {
    uint32_t ip;
    uint32_t last_ms;
} dj_link_reply_slot_t;

typedef struct {
    char     name[DJ_LINK_SESSION_NAME_MAX + 1];
    uint8_t  mac[6];
    uint32_t ip;                 /* host order */
    dj_link_claim_phase_t phase;
    uint8_t  iteration;          /* 1..3 within a stage */
    uint32_t next_ms;
    uint8_t  device_number;      /* candidate while claiming, ours when ACTIVE */
    uint8_t  sibling_number;     /* v298: our other deck's number, never taken */
    uint8_t  pair;               /* v308: DJ_LINK_PAIR_*; kept across start */
    uint8_t  startup_flags;      /* keep-alive 0x25: 0x02 alone, 0x01 joined */
    uint32_t reclaims;
    dj_link_seen_t seen[DJ_LINK_SEEN_MAX];
    dj_link_reply_slot_t replies[DJ_LINK_REPLY_LIMIT_SLOTS];
} dj_link_session_t;

/* v308: a session's place in our pair of decks. */
#define DJ_LINK_PAIR_NONE   0u   /* one player: highest free number */
#define DJ_LINK_PAIR_LOW    1u   /* deck 1: below its sibling */
#define DJ_LINK_PAIR_HIGH   2u   /* deck 2: above its sibling */

/* Begin (or restart) the join sequence. Numbers already seen are kept. */
void dj_link_session_start(dj_link_session_t *s, const char *name,
                           const uint8_t mac[6], uint32_t ip, uint32_t now_ms);
/* Forget everything; nothing is sent until the next start. */
void dj_link_session_reset(dj_link_session_t *s);
bool dj_link_session_active(const dj_link_session_t *s);
/* Our player number once ACTIVE, else 0. */
uint8_t dj_link_session_number(const dj_link_session_t *s);

/* v298: the number our other deck holds or claims (0 = none). It is never
 * picked here and counts as a device in the keep-alive. */
void dj_link_session_set_sibling(dj_link_session_t *s, uint8_t number);

/* v308: DJ_LINK_PAIR_* for the next number picked; reset clears it. */
void dj_link_session_set_pair(dj_link_session_t *s, uint8_t pair);

/* Record that device `number` is on the network (beat/status/keep-alive). */
void dj_link_session_note_device(dj_link_session_t *s, uint8_t number, uint32_t now_ms);

/* Next broadcast (port 50000) due at now_ms, built into out: returns its
 * length, or 0 when nothing is due. At most one packet per call. */
int dj_link_session_poll(dj_link_session_t *s, uint32_t now_ms, uint8_t *out, size_t cap);

/* A port-50000 packet from src_ip (host order). Returns true when it changed
 * our claim (conflict -> new number, or 0x05 -> ACTIVE). */
bool dj_link_session_on_discovery(dj_link_session_t *s, const uint8_t *buf, size_t len,
                                  uint32_t src_ip, uint32_t now_ms);

/* Media query 0x05 from src_ip. Returns the 0x06 response length built into
 * out (to be unicast to src_ip:50002), or 0 when it must be ignored: not
 * ACTIVE, not for our USB slot, reply IP != sender, no library, or rate
 * limited. */
int dj_link_session_media_reply(dj_link_session_t *s, const uint8_t *buf, size_t len,
                                uint32_t src_ip, uint32_t track_count, uint32_t now_ms,
                                uint8_t *out, size_t cap);

typedef enum {
    DJ_LINK_LOAD_OK = 0,
    DJ_LINK_LOAD_BAD_PACKET,
    DJ_LINK_LOAD_NOT_JOINED,     /* we have no player number yet */
    DJ_LINK_LOAD_NOT_UNICAST,    /* broadcast/multicast: not addressed to us */
    DJ_LINK_LOAD_BAD_SENDER,     /* number 0 or our own */
    DJ_LINK_LOAD_REMOTE_SOURCE,  /* none, deck 2, or not deck 1's USB */
    DJ_LINK_LOAD_NO_TRACK,       /* rekordbox id 0 */
} dj_link_load_verdict_t;

typedef struct {
    uint8_t  sender_number;   /* 0x21 */
    uint8_t  source_device;   /* 0x28 (esp-djlink: target_player) */
    uint8_t  source_slot;     /* 0x29 */
    uint32_t rekordbox_id;    /* 0x2c */
    uint8_t  dest_zero_based; /* 0x40, informative only */
} dj_link_load_cmd_t;

/* `s` is the target deck's session. Of our media, only the USB announced by
 * player `library_number` (deck 1, the one that answers media queries)
 * holds tracks; v298: any other player's media is accepted too (the UI
 * downloads it). A sender holding either of our numbers is refused. */
dj_link_load_verdict_t dj_link_session_check_load(const dj_link_session_t *s,
                                                  uint8_t library_number,
                                                  const uint8_t *buf, size_t len,
                                                  bool unicast, dj_link_load_cmd_t *out);
/* v298: the deck a load-track names (0x40 = target player - 1), from our two
 * numbers; -1 when it names neither (the UI then picks a deck). */
int dj_link_load_target_deck(const uint8_t numbers[2], const dj_link_load_cmd_t *cmd);
const char *dj_link_load_verdict_str(dj_link_load_verdict_t verdict);

/* 0x1a ack, sent to the commander's port 50002 once the load is accepted. */
int dj_link_session_load_ack(const dj_link_session_t *s, uint8_t *out, size_t cap);

/* v301: one beat of the deck's grid. */
typedef struct {
    uint32_t time_ms;            /* track time */
    uint8_t  beat_in_bar;        /* 1..4, 0 = unknown */
} dj_link_report_beat_t;

/* v298: what the UI reports for one deck; the CDJ status is built from it. */
typedef struct {
    bool     loaded;
    bool     playing;
    uint8_t  source_number;      /* player whose media holds it, 0 = our library */
    uint8_t  source_slot;        /* DJLINK_SLOT_*, used when source_number != 0 */
    uint32_t rekordbox_id;       /* 0 = unknown */
    uint16_t bpm100;             /* track BPM x100, 0 = unknown */
    int16_t  pitch_centipercent; /* tempo adjust, 0.01 % units */
    bool     sync;
    bool     master;             /* v305: set by dj_link while LINK SYNC is on */
    uint8_t  master_handoff;     /* v305: Mh, set by dj_link; 0 = none */
    /* v301: playhead, for the position 0x0b and beat 0x28 packets. */
    uint32_t position_ms;        /* playhead when reported */
    uint32_t duration_ms;        /* 0 = unknown */
    uint16_t speed_permille;     /* playback speed, 0 = frozen (scratch) */
    uint32_t stamp_ms;           /* dj_link clock at report, set by dj_link */
    uint8_t  beat_count;         /* valid entries in beats[] */
    /* The last grid beat at or before position_ms (if any), then the next
     * ones (dj_link_report_beats). */
    dj_link_report_beat_t beats[DJ_LINK_REPORT_BEATS];
} dj_link_deck_report_t;

/* Beat i of a grid of `count` beats in time order; false past the end. */
typedef bool (*dj_link_grid_get_fn)(const void *grid, size_t i, dj_link_report_beat_t *out);
/* Fill r->beats / r->beat_count with the window around r->position_ms. */
void dj_link_report_beats(dj_link_deck_report_t *r, const void *grid, size_t count,
                          dj_link_grid_get_fn get);

/* Playhead at now_ms: the reported one, advanced at speed while playing (for
 * at most DJ_LINK_REPORT_HOLD_MS) and clamped to the duration. */
uint32_t dj_link_deck_playhead(const dj_link_deck_report_t *r, uint32_t now_ms);
/* Wall-clock ms until the playhead reaches the next beat of the window;
 * UINT32_MAX when not playing or none is ahead. */
uint32_t dj_link_deck_next_beat_in_ms(const dj_link_deck_report_t *r, uint32_t now_ms);

/* Absolute position 0x0b for this session's deck (track loaded, playing or
 * not): returns its length, or 0. */
int dj_link_session_position(const dj_link_session_t *s, const dj_link_deck_report_t *deck,
                             uint32_t now_ms, uint8_t *out, size_t cap);

/* Last beat sent for one deck, owned by the caller. */
typedef struct {
    bool     valid;
    uint32_t playhead_ms;        /* where the last check stopped */
    uint32_t beat_ms;            /* last beat sent, track time */
} dj_link_beat_tracker_t;

/* Beat 0x28 when the playhead crossed a grid beat since the last call
 * (playing only; a seek re-arms without sending): returns its length, or
 * 0. At most one per call, the latest beat crossed. */
int dj_link_session_beat(const dj_link_session_t *s, const dj_link_deck_report_t *deck,
                         dj_link_beat_tracker_t *t, uint32_t now_ms, uint8_t *out, size_t cap);

/* CDJ status 0x0a for this session's deck, built into out: returns its
 * length, or 0 when the session holds no number. A track of our library is
 * reported on player `library_number`'s USB. `counter` increments per
 * packet. */
int dj_link_session_status(const dj_link_session_t *s, uint8_t library_number,
                           const dj_link_deck_report_t *deck, uint32_t counter,
                           uint8_t *out, size_t cap);

/* Deck for a network load: the one not playing; when neither plays, an empty
 * deck first, else deck 0. -1 when both decks play (refused). */
int dj_link_pick_target_deck(bool deck0_playing, bool deck1_playing,
                             bool deck0_loaded, bool deck1_loaded);

#ifdef __cplusplus
}
#endif
