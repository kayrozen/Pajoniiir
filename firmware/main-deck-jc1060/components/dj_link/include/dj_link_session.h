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
//     the NFS/dbserver client lands in v248+) and the 0x1a ack.
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
    uint8_t  startup_flags;      /* keep-alive 0x25: 0x02 alone, 0x01 joined */
    uint32_t reclaims;
    dj_link_seen_t seen[DJ_LINK_SEEN_MAX];
    dj_link_reply_slot_t replies[DJ_LINK_REPLY_LIMIT_SLOTS];
} dj_link_session_t;

/* Begin (or restart) the join sequence. Numbers already seen are kept. */
void dj_link_session_start(dj_link_session_t *s, const char *name,
                           const uint8_t mac[6], uint32_t ip, uint32_t now_ms);
/* Forget everything; nothing is sent until the next start. */
void dj_link_session_reset(dj_link_session_t *s);
bool dj_link_session_active(const dj_link_session_t *s);
/* Our player number once ACTIVE, else 0. */
uint8_t dj_link_session_number(const dj_link_session_t *s);

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
    DJ_LINK_LOAD_REMOTE_SOURCE,  /* track on another player's slot: v248+ */
    DJ_LINK_LOAD_NO_TRACK,       /* rekordbox id 0 */
} dj_link_load_verdict_t;

typedef struct {
    uint8_t  sender_number;   /* 0x21 */
    uint8_t  source_device;   /* 0x28 (esp-djlink: target_player) */
    uint8_t  source_slot;     /* 0x29 */
    uint32_t rekordbox_id;    /* 0x2c */
    uint8_t  dest_zero_based; /* 0x40, informative only */
} dj_link_load_cmd_t;

dj_link_load_verdict_t dj_link_session_check_load(const dj_link_session_t *s,
                                                  const uint8_t *buf, size_t len,
                                                  bool unicast, dj_link_load_cmd_t *out);
const char *dj_link_load_verdict_str(dj_link_load_verdict_t verdict);

/* 0x1a ack, sent to the commander's port 50002 once the load is accepted. */
int dj_link_session_load_ack(const dj_link_session_t *s, uint8_t *out, size_t cap);

/* Deck for a network load: the one not playing; when neither plays, an empty
 * deck first, else deck 0. -1 when both decks play (refused). */
int dj_link_pick_target_deck(bool deck0_playing, bool deck1_playing,
                             bool deck0_loaded, bool deck1_loaded);

#ifdef __cplusplus
}
#endif
