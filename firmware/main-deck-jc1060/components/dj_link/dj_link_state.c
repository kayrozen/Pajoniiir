#include "dj_link_state.h"

#include <stdio.h>
#include <string.h>

#include "djlink/beat.h"
#include "djlink/claim.h"
#include "djlink/status.h"

#define DJ_LINK_BPM100_NO_TRACK 0xffffu

void dj_link_table_reset(dj_link_table_t *table)
{
    if (table) {
        memset(table, 0, sizeof(*table));
    }
}

/* Slot for device_number: the existing one, else a free one, else the peer
 * heard from least recently (a seventh device evicts the stalest). */
static dj_link_peer_t *peer_slot(dj_link_table_t *table,
                                 uint8_t device_number,
                                 const uint8_t name[DJLINK_NAME_LEN],
                                 uint32_t now_ms)
{
    dj_link_peer_t *free_slot = NULL;
    dj_link_peer_t *oldest = &table->peers[0];
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        dj_link_peer_t *p = &table->peers[i];
        if (p->in_use && p->device_number == device_number) {
            p->last_seen_ms = now_ms;
            return p;
        }
        if (!p->in_use && !free_slot) {
            free_slot = p;
        }
        if ((uint32_t)(now_ms - p->last_seen_ms) >
            (uint32_t)(now_ms - oldest->last_seen_ms)) {
            oldest = p;
        }
    }
    dj_link_peer_t *p = free_slot ? free_slot : oldest;
    memset(p, 0, sizeof(*p));
    p->in_use = true;
    p->device_number = device_number;
    p->master_handoff = 0xffu;
    djlink_name_to_str(name, p->name);
    p->last_seen_ms = now_ms;
    return p;
}

static float bpm_from_track(uint16_t bpm100, int32_t pitch_raw)
{
    if (bpm100 == 0u || bpm100 == DJ_LINK_BPM100_NO_TRACK || pitch_raw <= 0) {
        return 0.0f;
    }
    return djlink_effective_bpm(bpm100, pitch_raw);
}

static dj_link_rx_t ingest_beat(dj_link_table_t *table, const uint8_t *buf,
                                size_t len, uint32_t src_ip, uint32_t now_ms)
{
    djlink_beat_t beat;
    if (djlink_beat_parse(buf, len, &beat) != DJLINK_OK || beat.device_number == 0u) {
        return DJ_LINK_RX_DROPPED;
    }
    dj_link_peer_t *p = peer_slot(table, beat.device_number, beat.name, now_ms);
    if (src_ip) {
        p->ip = src_ip;
    }
    p->has_beat = true;
    p->beat_rx_ms = now_ms;
    p->beat_in_bar = beat.beat_in_bar;
    p->beat_interval_ms = beat.next_beat_ms;
    float bpm = bpm_from_track(beat.bpm100, beat.pitch_raw);
    p->beat_bpm = bpm > 0.0f ? bpm : 0.0f;
    if (bpm > 0.0f) {
        p->bpm = bpm;
        p->pitch_pct = djlink_pitch_raw_to_percent(beat.pitch_raw);
    }
    return DJ_LINK_RX_ACCEPTED;
}

static dj_link_rx_t ingest_position(dj_link_table_t *table, const uint8_t *buf,
                                    size_t len, uint32_t src_ip, uint32_t now_ms)
{
    djlink_position_t pos;
    if (djlink_position_parse(buf, len, &pos) != DJLINK_OK || pos.device_number == 0u) {
        return DJ_LINK_RX_DROPPED;
    }
    dj_link_peer_t *p = peer_slot(table, pos.device_number, pos.name, now_ms);
    if (src_ip) {
        p->ip = src_ip;
    }
    p->has_position = true;
    p->position_rx_ms = now_ms;
    p->playhead_ms = pos.playhead_ms;
    p->track_length_s = pos.track_length_s;
    if (pos.bpm10 > 0) {
        p->bpm = (float)pos.bpm10 / 10.0f;
        p->pitch_pct = (float)pos.pitch_x100 / 100.0f;
    }
    return DJ_LINK_RX_ACCEPTED;
}

static dj_link_rx_t ingest_status(dj_link_table_t *table, const uint8_t *buf,
                                  size_t len, uint32_t src_ip, uint32_t now_ms)
{
    djlink_status_t st;
    if (djlink_status_parse(buf, len, &st) != DJLINK_OK || st.device_number == 0u) {
        return DJ_LINK_RX_DROPPED;
    }
    /* esp-djlink 0.2.0 leaves djlink_status_t.name zeroed; the name field is
     * at 0x0b..0x1e like every family header, and parse has checked len. */
    dj_link_peer_t *p = peer_slot(table, st.device_number, &buf[0x0b], now_ms);
    if (src_ip) {
        p->ip = src_ip;
    }
    p->has_status = st.has_flag_bits;
    if (st.has_flag_bits) {
        p->master  = (st.flags & DJLINK_FLAG_MASTER) != 0u;
        p->on_air  = (st.flags & DJLINK_FLAG_ON_AIR) != 0u;
        p->playing = (st.flags & DJLINK_FLAG_PLAYING) != 0u;
        p->synced  = (st.flags & DJLINK_FLAG_SYNC) != 0u;
    }
    p->master_handoff = st.master_handoff;
    if (st.bpm100 == DJ_LINK_BPM100_NO_TRACK) {
        p->bpm = 0.0f;
    } else {
        float bpm = bpm_from_track(st.bpm100, st.pitch_raw);
        if (bpm > 0.0f) {
            p->bpm = bpm;
            p->pitch_pct = djlink_pitch_raw_to_percent(st.pitch_raw);
        }
    }
    return DJ_LINK_RX_ACCEPTED;
}

/* v297: keep-alive 0x06 (port 50000): number 0x24, type 0x21, IPv4 0x2c,
 * name 0x0c..0x1f like the rest of the claim family. */
static dj_link_rx_t ingest_keepalive(dj_link_table_t *table, const uint8_t *buf,
                                     size_t len, uint32_t src_ip, uint32_t now_ms)
{
    if (len < DJLINK_KEEPALIVE_PACKET_LEN || buf[0x24] == 0u ||
        (buf[0x21] != DJLINK_DEVICE_TYPE_CDJ &&
         buf[0x21] != DJ_LINK_DEVICE_TYPE_REKORDBOX)) {
        return DJ_LINK_RX_DROPPED;
    }
    dj_link_peer_t *p = peer_slot(table, buf[0x24], &buf[0x0c], now_ms);
    p->device_type = buf[0x21];
    uint32_t ip = djlink_rd32(&buf[0x2c]);
    if (src_ip) {
        p->ip = src_ip;
    } else if (ip) {
        p->ip = ip;
    }
    return DJ_LINK_RX_ACCEPTED;
}

dj_link_rx_t dj_link_table_ingest(dj_link_table_t *table, uint16_t port,
                                  const uint8_t *buf, size_t len,
                                  uint32_t now_ms)
{
    return dj_link_table_ingest_from(table, port, buf, len, 0u, now_ms);
}

dj_link_rx_t dj_link_table_ingest_from(dj_link_table_t *table, uint16_t port,
                                       const uint8_t *buf, size_t len,
                                       uint32_t src_ip, uint32_t now_ms)
{
    if (!table) {
        return DJ_LINK_RX_DROPPED;
    }
    dj_link_rx_t rx = DJ_LINK_RX_DROPPED;
    if (len <= DJLINK_MAX_PACKET && djlink_packet_is_valid(buf, len)) {
        int type = djlink_packet_type(buf, len);
        if (port == DJLINK_PORT_DISCOVERY && type == (int)DJLINK_TYPE_KEEPALIVE) {
            rx = ingest_keepalive(table, buf, len, src_ip, now_ms);
        } else if (port == DJLINK_PORT_BEAT && type == (int)DJLINK_TYPE_BEAT) {
            rx = ingest_beat(table, buf, len, src_ip, now_ms);
        } else if (port == DJLINK_PORT_BEAT && type == (int)DJLINK_TYPE_ABS_POSITION) {
            rx = ingest_position(table, buf, len, src_ip, now_ms);
        } else if (port == DJLINK_PORT_STATUS && type == (int)DJLINK_TYPE_CDJ_STATUS) {
            rx = ingest_status(table, buf, len, src_ip, now_ms);
        }
    }
    if (rx == DJ_LINK_RX_ACCEPTED) {
        table->rx_accepted++;
    } else {
        table->rx_dropped++;
    }
    return rx;
}

bool dj_link_table_expire(dj_link_table_t *table, uint32_t now_ms)
{
    bool removed = false;
    if (!table) {
        return false;
    }
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        dj_link_peer_t *p = &table->peers[i];
        if (p->in_use && (uint32_t)(now_ms - p->last_seen_ms) > DJ_LINK_PEER_TIMEOUT_MS) {
            memset(p, 0, sizeof(*p));
            removed = true;
        }
    }
    return removed;
}

void dj_link_table_summarize(const dj_link_table_t *table,
                             dj_link_state_t state,
                             dj_link_summary_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = state;
    if (!table) {
        return;
    }
    out->rx_accepted = table->rx_accepted;
    out->rx_dropped = table->rx_dropped;

    const dj_link_peer_t *confirmed = NULL;
    const dj_link_peer_t *latest_beat = NULL;
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        const dj_link_peer_t *p = &table->peers[i];
        if (!p->in_use) {
            continue;
        }
        out->peer_count++;
        bool collection = p->device_type == DJ_LINK_DEVICE_TYPE_REKORDBOX;
        if (p->ip && (collection || (p->device_number >= 1u &&
                                     p->device_number <= DJ_LINK_MAX_PLAYER_NUMBER))) {
            /* Insertion by number; at most DJ_LINK_MAX_PEERS entries. */
            uint8_t at = out->player_count;
            while (at > 0u && out->players[at - 1u].number > p->device_number) {
                out->players[at] = out->players[at - 1u];
                at--;
            }
            out->players[at].number = p->device_number;
            memcpy(out->players[at].name, p->name, sizeof(out->players[at].name));
            out->players[at].ip = p->ip;
            out->players[at].collection = collection;
            out->player_count++;
        }
        if (p->has_status && p->master &&
            (!confirmed || p->device_number < confirmed->device_number)) {
            confirmed = p;
        }
        /* Wrap-safe "newer": the later beat is less than half the counter
         * range ahead. */
        if (p->has_beat &&
            (!latest_beat ||
             (int32_t)(p->beat_rx_ms - latest_beat->beat_rx_ms) > 0)) {
            latest_beat = p;
        }
    }
    const dj_link_peer_t *master = confirmed ? confirmed : latest_beat;
    if (master) {
        out->has_master = true;
        out->master_confirmed = confirmed != NULL;
        out->master = *master;
    }
}

static bool beat_clock_candidate(const dj_link_peer_t *p, uint32_t now_ms)
{
    if (!p->in_use || !p->has_beat || !(p->beat_bpm > 0.0f) ||
        (p->has_status && !p->playing)) {
        return false;
    }
    const float since_ms = (float)(uint32_t)(now_ms - p->beat_rx_ms);
    return since_ms * p->beat_bpm <= DJ_LINK_BEAT_CLOCK_STALE_BEATS * 60000.0f;
}

void dj_link_table_beat_clock(const dj_link_table_t *table, uint8_t prefer,
                              uint32_t now_ms, dj_link_beat_clock_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!table) {
        return;
    }
    const dj_link_peer_t *confirmed = NULL;
    const dj_link_peer_t *preferred = NULL;
    const dj_link_peer_t *newest = NULL;
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        const dj_link_peer_t *p = &table->peers[i];
        if (!beat_clock_candidate(p, now_ms)) {
            continue;
        }
        if (p->has_status && p->master &&
            (!confirmed || p->device_number < confirmed->device_number)) {
            confirmed = p;
        }
        if (prefer != 0u && p->device_number == prefer) {
            preferred = p;
        }
        if (!newest || (int32_t)(p->beat_rx_ms - newest->beat_rx_ms) > 0) {
            newest = p;
        }
    }
    const dj_link_peer_t *p = confirmed ? confirmed : preferred ? preferred : newest;
    if (!p) {
        return;
    }
    out->valid = true;
    out->player = p->device_number;
    out->beat_in_bar = p->beat_in_bar >= 1u && p->beat_in_bar <= 4u ? p->beat_in_bar : 0u;
    out->anchor_ms = p->beat_rx_ms;
    out->period_us = (uint32_t)(60000000.0f / p->beat_bpm + 0.5f);
}

float dj_link_peer_beat_phase(const dj_link_peer_t *peer, uint32_t now_ms)
{
    if (!peer || !peer->has_beat || peer->beat_interval_ms == 0u) {
        return -1.0f;
    }
    uint32_t since = now_ms - peer->beat_rx_ms;
    if (since >= peer->beat_interval_ms) {
        return -1.0f;   /* next beat overdue: player stopped or packet lost */
    }
    return (float)since / (float)peer->beat_interval_ms;
}

void dj_link_format_status(const dj_link_summary_t *summary,
                           char *out, size_t cap)
{
    if (!out || cap == 0u) {
        return;
    }
    if (!summary) {
        snprintf(out, cap, "DJ LINK: OFF");
        return;
    }
    switch (summary->state) {
    case DJ_LINK_STATE_OFF:
        snprintf(out, cap, "DJ LINK: OFF");
        return;
    case DJ_LINK_STATE_WAIT_IP:
        snprintf(out, cap, "DJ LINK: ON - waiting for Ethernet IP");
        return;
    case DJ_LINK_STATE_ERROR:
        snprintf(out, cap, "DJ LINK: ON - socket error, retrying");
        return;
    case DJ_LINK_STATE_LISTENING:
    default:
        break;
    }
    /* "P4+P3" = the player numbers decks 1 and 2 hold on the network;
     * "joining" while deck 1's claim sequence is still running. */
    char us[16];
    if (summary->our_number != 0u && summary->our_number_d2 != 0u) {
        snprintf(us, sizeof(us), "ON P%u+P%u", (unsigned)summary->our_number,
                 (unsigned)summary->our_number_d2);
    } else if (summary->our_number != 0u) {
        snprintf(us, sizeof(us), "ON P%u", (unsigned)summary->our_number);
    } else {
        snprintf(us, sizeof(us), "ON joining");
    }
    if (!summary->has_master) {
        if (summary->peer_count == 0u) {
            snprintf(out, cap, "DJ LINK: %s - no players", us);
        } else {
            snprintf(out, cap, "DJ LINK: %s - %u player%s, no master", us,
                     (unsigned)summary->peer_count,
                     summary->peer_count == 1u ? "" : "s");
        }
        return;
    }
    const dj_link_peer_t *m = &summary->master;
    const char *name = m->name[0] != '\0' ? m->name : "PLAYER";
    /* "?" marks a master inferred from beats only (no status packets). */
    const char *mark = summary->master_confirmed ? "" : "?";
    if (m->bpm > 0.0f) {
        unsigned bpm10 = (unsigned)(m->bpm * 10.0f + 0.5f);
        snprintf(out, cap, "DJ LINK: %s - %s #%u%s %u.%u BPM%s",
                 us, name, (unsigned)m->device_number, mark,
                 bpm10 / 10u, bpm10 % 10u,
                 m->on_air ? " ON AIR" : "");
    } else {
        snprintf(out, cap, "DJ LINK: %s - %s #%u%s --- BPM",
                 us, name, (unsigned)m->device_number, mark);
    }
}
