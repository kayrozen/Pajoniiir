#pragma once
//
// dj_link_master — tempo master negotiation (v305, docs/DJ_LINK_SPEC.md
// Phase 4), pure and host tested like dj_link_state / dj_link_session.
//
// One of our two decks may hold the DJ Link tempo master. A deck "wants" it
// while its SYNC MASTER is set in deck_core (the deck report's master); the
// network flag is asserted only once it is granted (sync.html):
//   * no peer asserts master: assert right away;
//   * a peer does: send it a takeover request 0x26 and assert once its status
//     names our number in Mh (0x9f); no answer within
//     DJ_LINK_MASTER_REQUEST_TIMEOUT_MS = refused, no retry until the deck
//     drops SYNC MASTER;
//   * a peer asks us (0x26) while we assert: answer 0x27, keep asserting with
//     Mh = the requester until its status asserts master, then stop and have
//     deck_core drop SYNC MASTER;
//   * a master that names one of our loaded decks in Mh unasked hands it to
//     us: take it;
//   * a peer asserting master over us without a handoff wins: we drop.
// Sync control 0x2a carries no target (both of our players share one IP),
// see dj_link_sync_target_deck. deck_core stays authoritative: this module
// only asks it for SYNC / SYNC MASTER changes through dj_link_deck_cmd_t.
// No I/O, no RTOS, no heap.
//
#include <stdbool.h>
#include <stdint.h>

#include "dj_link_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_MASTER_REQUEST_TIMEOUT_MS 2000u /* 0x26 unanswered = refused */
#define DJ_LINK_MASTER_REQUEST_RETRY_MS   500u  /* 0x26 resent while waiting */
#define DJ_LINK_MASTER_YIELD_TIMEOUT_MS   3000u /* requester never asserted: keep it */
#define DJ_LINK_MASTER_GRACE_MS           1500u /* deck_core catching up with a take */
#define DJ_LINK_MH_NONE                   0xffu

/* One of our decks, as the dj_link task sees it. */
typedef struct {
    uint8_t number;     /* its player number, 0 = none */
    bool    loaded;
    bool    playing;
    bool    want;       /* SYNC MASTER set in deck_core */
} dj_link_master_deck_t;

typedef enum {
    DJ_LINK_MASTER_IDLE = 0,
    DJ_LINK_MASTER_REQUESTING,  /* 0x26 sent, waiting for Mh = our number */
    DJ_LINK_MASTER_ASSERTING,   /* our deck is the tempo master */
    DJ_LINK_MASTER_YIELDING,    /* 0x27 sent, Mh = requester until it asserts */
} dj_link_master_phase_t;

/* What deck_core is asked to do (never decided here). */
typedef enum {
    DJ_LINK_DECK_CMD_NONE = 0,
    DJ_LINK_DECK_CMD_SYNC_ON,
    DJ_LINK_DECK_CMD_SYNC_OFF,
    DJ_LINK_DECK_CMD_MASTER_TAKE,
    DJ_LINK_DECK_CMD_MASTER_DROP,
} dj_link_deck_cmd_t;

typedef struct {
    dj_link_master_phase_t phase;
    uint8_t  deck;          /* 0/1, meaningful when not IDLE */
    uint8_t  peer;          /* REQUESTING: the master asked; YIELDING: the
                             * requester; ASSERTING: the master we took over
                             * from, 0 = none */
    uint32_t since_ms;      /* phase entry */
    uint32_t sent_ms;       /* last 0x26 */
    bool     backoff[2];    /* refused or yielded: no request until want drops */
} dj_link_master_t;

typedef struct {
    bool     send_request;  /* 0x26 from request_number to request_ip */
    uint8_t  request_number;
    uint32_t request_ip;    /* host order */
    dj_link_deck_cmd_t cmd[2];
} dj_link_master_out_t;

void dj_link_master_reset(dj_link_master_t *m);

/* Advance with the decks and the peer table, every dj_link task tick. */
void dj_link_master_update(dj_link_master_t *m, const dj_link_master_deck_t decks[2],
                           const dj_link_table_t *table, uint32_t now_ms,
                           dj_link_master_out_t *out);

/* A takeover request 0x26 from player `requester`. Returns the number to
 * answer 0x27 from (our master deck's), 0 = not ours to give. */
uint8_t dj_link_master_on_request(dj_link_master_t *m, const dj_link_master_deck_t decks[2],
                                  uint8_t requester, uint32_t now_ms);
/* A takeover response 0x27 from `responder`: true when it answers our
 * pending request (the grant itself is the Mh in its status). */
bool dj_link_master_on_response(const dj_link_master_t *m, uint8_t responder);

/* What deck `deck`'s status shows: the master flag (F bit 5 and Mm) and Mh. */
bool dj_link_master_asserts(const dj_link_master_t *m, uint8_t deck);
uint8_t dj_link_master_handoff(const dj_link_master_t *m, uint8_t deck);

/* Sync control 0x2a target: the only deck holding a number, else the only
 * loaded one, else the only playing one among the loaded; -1 = ambiguous
 * (refused). */
int dj_link_sync_target_deck(const dj_link_master_deck_t decks[2]);
/* DJLINK_SYNC_ON / OFF / MASTER -> command; NONE for anything else. */
dj_link_deck_cmd_t dj_link_sync_action_cmd(uint8_t action);

const char *dj_link_master_phase_str(dj_link_master_phase_t phase);
const char *dj_link_deck_cmd_str(dj_link_deck_cmd_t cmd);

#ifdef __cplusplus
}
#endif
