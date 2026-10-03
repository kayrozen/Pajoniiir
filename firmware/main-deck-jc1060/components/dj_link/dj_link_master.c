#include "dj_link_master.h"

#include <string.h>

#include "djlink/sync.h"

void dj_link_master_reset(dj_link_master_t *m)
{
    if (m) {
        memset(m, 0, sizeof(*m));
    }
}

static bool ours(const dj_link_master_deck_t decks[2], uint8_t number)
{
    return number != 0u && (number == decks[0].number || number == decks[1].number);
}

/* The peer asserting master in its status, NULL if none is known. */
static const dj_link_peer_t *peer_master(const dj_link_table_t *table,
                                         const dj_link_master_deck_t decks[2])
{
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        const dj_link_peer_t *p = &table->peers[i];
        if (p->in_use && p->has_status && p->master && !ours(decks, p->device_number)) {
            return p;
        }
    }
    return NULL;
}

static const dj_link_peer_t *peer_by_number(const dj_link_table_t *table, uint8_t number)
{
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        const dj_link_peer_t *p = &table->peers[i];
        if (p->in_use && p->device_number == number) {
            return p;
        }
    }
    return NULL;
}

static void enter(dj_link_master_t *m, dj_link_master_phase_t phase, uint8_t deck,
                  uint8_t peer, uint32_t now_ms)
{
    m->phase = phase;
    m->deck = deck;
    m->peer = peer;
    m->since_ms = now_ms;
}

static void request(dj_link_master_t *m, const dj_link_master_deck_t decks[2],
                    const dj_link_peer_t *master, uint32_t now_ms, dj_link_master_out_t *out)
{
    m->sent_ms = now_ms;
    if (master->ip == 0u) {
        return;   /* no address yet: the retry catches it */
    }
    out->send_request = true;
    out->request_number = decks[m->deck].number;
    out->request_ip = master->ip;
}

static void idle_step(dj_link_master_t *m, const dj_link_master_deck_t decks[2],
                      const dj_link_peer_t *master, uint32_t now_ms,
                      dj_link_master_out_t *out)
{
    if (master) {
        /* A master handing it to one of our decks unasked. */
        for (uint8_t d = 0; d < 2u; d++) {
            if (decks[d].number != 0u && decks[d].loaded &&
                master->master_handoff == decks[d].number) {
                enter(m, DJ_LINK_MASTER_ASSERTING, d, master->device_number, now_ms);
                if (!decks[d].want) {
                    out->cmd[d] = DJ_LINK_DECK_CMD_MASTER_TAKE;
                }
                return;
            }
        }
    }
    for (uint8_t d = 0; d < 2u; d++) {
        if (!decks[d].want || decks[d].number == 0u || m->backoff[d]) {
            continue;
        }
        if (!master) {
            enter(m, DJ_LINK_MASTER_ASSERTING, d, 0u, now_ms);
        } else {
            enter(m, DJ_LINK_MASTER_REQUESTING, d, master->device_number, now_ms);
            request(m, decks, master, now_ms, out);
        }
        return;
    }
}

void dj_link_master_update(dj_link_master_t *m, const dj_link_master_deck_t decks[2],
                           const dj_link_table_t *table, uint32_t now_ms,
                           dj_link_master_out_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!m || !decks || !table) {
        return;
    }
    for (uint8_t d = 0; d < 2u; d++) {
        if (!decks[d].want) {
            m->backoff[d] = false;
        }
    }
    if (m->phase != DJ_LINK_MASTER_IDLE && decks[m->deck].number == 0u) {
        enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);   /* lost our number */
    }
    const dj_link_peer_t *master = peer_master(table, decks);
    const uint8_t d = m->deck;
    const uint32_t age = now_ms - m->since_ms;

    switch (m->phase) {
    case DJ_LINK_MASTER_IDLE:
        break;

    case DJ_LINK_MASTER_REQUESTING:
        if (!decks[d].want) {
            enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);
        } else if (!master) {
            /* The master went away or stood down: nobody to ask. */
            enter(m, DJ_LINK_MASTER_ASSERTING, d, 0u, now_ms);
        } else if (master->master_handoff == decks[d].number) {
            enter(m, DJ_LINK_MASTER_ASSERTING, d, master->device_number, now_ms);
        } else if (master->device_number != m->peer) {
            m->peer = master->device_number;    /* another took it: ask that one */
            request(m, decks, master, now_ms, out);
        } else if (age >= DJ_LINK_MASTER_REQUEST_TIMEOUT_MS) {
            m->backoff[d] = true;
            enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);
        } else if ((uint32_t)(now_ms - m->sent_ms) >= DJ_LINK_MASTER_REQUEST_RETRY_MS) {
            request(m, decks, master, now_ms, out);
        }
        return;

    case DJ_LINK_MASTER_ASSERTING:
        if (master) {
            /* The master we took over from overlaps until it sees us. */
            const bool outgoing = master->device_number == m->peer &&
                                  master->master_handoff == decks[d].number &&
                                  age < DJ_LINK_MASTER_YIELD_TIMEOUT_MS;
            if (!outgoing) {
                /* Taken without a handoff: two masters is worse than none. */
                out->cmd[d] = DJ_LINK_DECK_CMD_MASTER_DROP;
                m->backoff[d] = true;
                enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);
                return;
            }
        }
        if (decks[d].want || age < DJ_LINK_MASTER_GRACE_MS) {
            return;
        }
        enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);   /* SYNC MASTER dropped or moved */
        break;

    case DJ_LINK_MASTER_YIELDING:
    {
        const dj_link_peer_t *next = peer_by_number(table, m->peer);
        if (!decks[d].want) {
            enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);
        } else if (next && next->has_status && next->master) {
            out->cmd[d] = DJ_LINK_DECK_CMD_MASTER_DROP;
            m->backoff[d] = true;
            enter(m, DJ_LINK_MASTER_IDLE, 0u, 0u, now_ms);
        } else if (age >= DJ_LINK_MASTER_YIELD_TIMEOUT_MS) {
            enter(m, DJ_LINK_MASTER_ASSERTING, d, 0u, now_ms);
        }
        return;
    }
    }
    idle_step(m, decks, master, now_ms, out);
}

uint8_t dj_link_master_on_request(dj_link_master_t *m, const dj_link_master_deck_t decks[2],
                                  uint8_t requester, uint32_t now_ms)
{
    if (!m || !decks || requester == 0u || ours(decks, requester)) {
        return 0u;
    }
    if (m->phase == DJ_LINK_MASTER_ASSERTING) {
        enter(m, DJ_LINK_MASTER_YIELDING, m->deck, requester, now_ms);
        return decks[m->deck].number;
    }
    if (m->phase == DJ_LINK_MASTER_YIELDING && m->peer == requester) {
        return decks[m->deck].number;   /* our 0x27 was lost: answer again */
    }
    return 0u;
}

bool dj_link_master_on_response(const dj_link_master_t *m, uint8_t responder)
{
    return m && m->phase == DJ_LINK_MASTER_REQUESTING && responder != 0u &&
           responder == m->peer;
}

bool dj_link_master_asserts(const dj_link_master_t *m, uint8_t deck)
{
    return m && m->deck == deck &&
           (m->phase == DJ_LINK_MASTER_ASSERTING || m->phase == DJ_LINK_MASTER_YIELDING);
}

uint8_t dj_link_master_handoff(const dj_link_master_t *m, uint8_t deck)
{
    return m && m->deck == deck && m->phase == DJ_LINK_MASTER_YIELDING
        ? m->peer : DJ_LINK_MH_NONE;
}

int dj_link_sync_target_deck(const dj_link_master_deck_t decks[2])
{
    if (!decks) {
        return -1;
    }
    const bool numbered[2] = { decks[0].number != 0u, decks[1].number != 0u };
    if (numbered[0] != numbered[1]) {
        return numbered[0] ? 0 : 1;
    }
    if (!numbered[0]) {
        return -1;
    }
    if (decks[0].loaded != decks[1].loaded) {
        return decks[0].loaded ? 0 : 1;
    }
    if (decks[0].loaded && decks[0].playing != decks[1].playing) {
        return decks[0].playing ? 0 : 1;
    }
    return -1;
}

dj_link_deck_cmd_t dj_link_sync_action_cmd(uint8_t action)
{
    switch (action) {
    case DJLINK_SYNC_ON:     return DJ_LINK_DECK_CMD_SYNC_ON;
    case DJLINK_SYNC_OFF:    return DJ_LINK_DECK_CMD_SYNC_OFF;
    case DJLINK_SYNC_MASTER: return DJ_LINK_DECK_CMD_MASTER_TAKE;
    default:                 return DJ_LINK_DECK_CMD_NONE;
    }
}

const char *dj_link_master_phase_str(dj_link_master_phase_t phase)
{
    switch (phase) {
    case DJ_LINK_MASTER_IDLE:       return "idle";
    case DJ_LINK_MASTER_REQUESTING: return "requesting";
    case DJ_LINK_MASTER_ASSERTING:  return "master";
    case DJ_LINK_MASTER_YIELDING:   return "yielding";
    default:                        return "?";
    }
}

const char *dj_link_deck_cmd_str(dj_link_deck_cmd_t cmd)
{
    switch (cmd) {
    case DJ_LINK_DECK_CMD_SYNC_ON:     return "sync on";
    case DJ_LINK_DECK_CMD_SYNC_OFF:    return "sync off";
    case DJ_LINK_DECK_CMD_MASTER_TAKE: return "take master";
    case DJ_LINK_DECK_CMD_MASTER_DROP: return "drop master";
    default:                           return "none";
    }
}
