#include "dj_link_session.h"

#include <string.h>

#include "djlink/claim.h"
#include "djlink/media.h"
#include "djlink/mixer.h"
#include "djlink/packet.h"
#include "djlink/status.h"

#define TYPE_ANNOUNCE      0x0au
#define TYPE_CLAIM_STAGE1  0x00u
#define TYPE_CLAIM_STAGE2  0x02u
#define TYPE_CLAIM_FINAL   0x04u
#define TYPE_ASSIGN_DONE   0x05u
#define TYPE_KEEPALIVE     0x06u
#define TYPE_CONFLICT      0x08u
#define TYPE_MEDIA_QUERY   0x05u
#define TYPE_LOAD_TRACK    0x19u

#define MEDIA_QUERY_MIN_LEN 0x30u
#define LOAD_DEST_OFFSET    0x40u

/* Players 1..4 are what every mixer/player generation understands; take the
 * highest free one so players that auto-number from 1 rarely collide with
 * us. 5/6 (CDJ-3000 networks) only when 1..4 are taken. */
static const uint8_t k_number_preference[] = { 4u, 3u, 2u, 1u, 5u, 6u };

static bool is_after(uint32_t now_ms, uint32_t at_ms)
{
    return (int32_t)(now_ms - at_ms) >= 0;
}

void dj_link_session_reset(dj_link_session_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

bool dj_link_session_active(const dj_link_session_t *s)
{
    return s && s->phase == DJ_LINK_CLAIM_ACTIVE;
}

uint8_t dj_link_session_number(const dj_link_session_t *s)
{
    return dj_link_session_active(s) ? s->device_number : 0u;
}

static bool number_in_use(const dj_link_session_t *s, uint8_t number, uint32_t now_ms)
{
    for (size_t i = 0; i < DJ_LINK_SEEN_MAX; i++) {
        const dj_link_seen_t *e = &s->seen[i];
        if (e->number == number &&
            (uint32_t)(now_ms - e->last_ms) <= DJ_LINK_DEVICE_TIMEOUT_MS) {
            return true;
        }
    }
    return false;
}

static uint8_t live_device_count(const dj_link_session_t *s, uint32_t now_ms)
{
    uint8_t n = 0;
    for (size_t i = 0; i < DJ_LINK_SEEN_MAX; i++) {
        if (s->seen[i].number != 0u &&
            (uint32_t)(now_ms - s->seen[i].last_ms) <= DJ_LINK_DEVICE_TIMEOUT_MS) {
            n++;
        }
    }
    return n;
}

void dj_link_session_note_device(dj_link_session_t *s, uint8_t number, uint32_t now_ms)
{
    if (!s || number == 0u) {
        return;
    }
    dj_link_seen_t *slot = NULL;
    for (size_t i = 0; i < DJ_LINK_SEEN_MAX; i++) {
        dj_link_seen_t *e = &s->seen[i];
        if (e->number == number) {
            slot = e;
            break;
        }
        if (!slot || e->number == 0u ||
            (slot->number != 0u &&
             (uint32_t)(now_ms - e->last_ms) > (uint32_t)(now_ms - slot->last_ms))) {
            slot = e;
        }
    }
    slot->number = number;
    slot->last_ms = now_ms;
}

static uint8_t pick_number(const dj_link_session_t *s, uint32_t now_ms)
{
    for (size_t i = 0; i < sizeof(k_number_preference); i++) {
        if (!number_in_use(s, k_number_preference[i], now_ms)) {
            return k_number_preference[i];
        }
    }
    return 0u;
}

/* Restart the numbered part of the claim with a number nobody holds. */
static void reclaim(dj_link_session_t *s, uint32_t now_ms)
{
    s->reclaims++;
    s->device_number = pick_number(s, now_ms);
    s->iteration = 1u;
    s->next_ms = now_ms;
    s->phase = s->device_number ? DJ_LINK_CLAIM_STAGE2 : DJ_LINK_CLAIM_NO_NUMBER;
}

void dj_link_session_start(dj_link_session_t *s, const char *name,
                           const uint8_t mac[6], uint32_t ip, uint32_t now_ms)
{
    if (!s) {
        return;
    }
    memset(s->name, 0, sizeof(s->name));
    if (name) {
        strncpy(s->name, name, DJ_LINK_SESSION_NAME_MAX);
    }
    if (mac) {
        memcpy(s->mac, mac, sizeof(s->mac));
    }
    s->ip = ip;
    s->phase = DJ_LINK_CLAIM_ANNOUNCE;
    s->iteration = 1u;
    s->next_ms = now_ms;
    s->device_number = 0u;
    s->startup_flags = 0u;
    memset(s->replies, 0, sizeof(s->replies));
}

static int build_keepalive(dj_link_session_t *s, uint32_t now_ms, uint8_t *out, size_t cap)
{
    djlink_keepalive_t ka = {
        .name = s->name,
        .device_number = s->device_number,
        .ip = s->ip,
        .peer_count = (uint8_t)(live_device_count(s, now_ms) + 1u),
        .startup_flags = s->startup_flags,
    };
    memcpy(ka.mac, s->mac, sizeof(ka.mac));
    int n = djlink_keepalive_build(&ka, out, cap);
    if (n > 0 && s->device_number > 4u) {
        /* CDJ-3000 variant: without 0x64 at 0x35 players 5/6 are reported
         * to kick themselves off the network (esp-djlink 0.2.0 leaves it 0). */
        out[0x35] = 0x64u;
    }
    return n;
}

int dj_link_session_poll(dj_link_session_t *s, uint32_t now_ms, uint8_t *out, size_t cap)
{
    if (!s || !out || s->phase == DJ_LINK_CLAIM_IDLE || !is_after(now_ms, s->next_ms)) {
        return 0;
    }
    int n = 0;
    uint8_t iteration = s->iteration;
    bool stage_done = iteration >= 3u;
    s->iteration = stage_done ? 1u : (uint8_t)(iteration + 1u);
    s->next_ms = now_ms + DJ_LINK_CLAIM_STEP_MS;

    switch (s->phase) {
    case DJ_LINK_CLAIM_ANNOUNCE: {
        djlink_announce_t a = { .name = s->name, .device_type = DJLINK_DEVICE_TYPE_CDJ,
                                .payload_byte = 0x01u };
        n = djlink_announce_build(&a, out, cap);
        if (stage_done) {
            s->phase = DJ_LINK_CLAIM_STAGE1;
        }
        break;
    }
    case DJ_LINK_CLAIM_STAGE1: {
        djlink_claim_mac_t c = { .name = s->name, .device_type = DJLINK_DEVICE_TYPE_CDJ,
                                 .iteration = iteration };
        memcpy(c.mac, s->mac, sizeof(c.mac));
        n = djlink_claim_mac_build(&c, out, cap);
        if (stage_done) {
            /* Everything heard so far (claims, keep-alives, beats) is taken. */
            s->device_number = pick_number(s, now_ms);
            s->phase = s->device_number ? DJ_LINK_CLAIM_STAGE2 : DJ_LINK_CLAIM_NO_NUMBER;
        }
        break;
    }
    case DJ_LINK_CLAIM_STAGE2: {
        djlink_claim_ip_t c = { .name = s->name, .device_type = DJLINK_DEVICE_TYPE_CDJ,
                                .ip = s->ip, .device_number = s->device_number,
                                .iteration = iteration, .auto_assign = 0x01u };
        memcpy(c.mac, s->mac, sizeof(c.mac));
        n = djlink_claim_ip_build(&c, out, cap);
        if (stage_done) {
            s->phase = DJ_LINK_CLAIM_FINAL;
        }
        break;
    }
    case DJ_LINK_CLAIM_FINAL: {
        djlink_claim_final_t c = { .name = s->name, .device_type = DJLINK_DEVICE_TYPE_CDJ,
                                   .device_number = s->device_number,
                                   .iteration = iteration };
        n = djlink_claim_final_build(&c, out, cap);
        if (stage_done) {
            s->phase = DJ_LINK_CLAIM_ACTIVE;
            s->startup_flags = live_device_count(s, now_ms) ? 0x01u : 0x02u;
            s->next_ms = now_ms;  /* first keep-alive right away */
        }
        break;
    }
    case DJ_LINK_CLAIM_ACTIVE:
        s->iteration = 1u;
        s->next_ms = now_ms + DJ_LINK_KEEPALIVE_MS;
        n = build_keepalive(s, now_ms, out, cap);
        break;
    case DJ_LINK_CLAIM_NO_NUMBER:
    default:
        s->iteration = 1u;
        s->next_ms = now_ms + DJ_LINK_KEEPALIVE_MS;
        s->device_number = pick_number(s, now_ms);
        if (s->device_number) {
            s->phase = DJ_LINK_CLAIM_STAGE2;
            s->next_ms = now_ms;
        }
        return 0;
    }
    return n > 0 ? n : 0;
}

static bool claiming_number(const dj_link_session_t *s)
{
    return s->phase == DJ_LINK_CLAIM_STAGE2 || s->phase == DJ_LINK_CLAIM_FINAL;
}

bool dj_link_session_on_discovery(dj_link_session_t *s, const uint8_t *buf, size_t len,
                                  uint32_t src_ip, uint32_t now_ms)
{
    if (!s || s->phase == DJ_LINK_CLAIM_IDLE || src_ip == s->ip ||
        len < 0x25u || !djlink_packet_is_valid(buf, len)) {
        return false;
    }
    uint8_t number;
    switch (buf[0x0a]) {
    case TYPE_KEEPALIVE:
        if (len < DJLINK_KEEPALIVE_PACKET_LEN || memcmp(&buf[0x26], s->mac, 6) == 0) {
            return false;
        }
        number = buf[0x24];
        dj_link_session_note_device(s, number, now_ms);
        /* Somebody is announcing our number: they own it now. */
        if ((claiming_number(s) || s->phase == DJ_LINK_CLAIM_ACTIVE) &&
            number == s->device_number) {
            reclaim(s, now_ms);
            return true;
        }
        return false;
    case TYPE_CLAIM_STAGE2:
    case TYPE_CLAIM_FINAL:
        if (buf[0x0a] == TYPE_CLAIM_STAGE2) {
            if (len < DJLINK_CLAIM_IP_PACKET_LEN || memcmp(&buf[0x28], s->mac, 6) == 0) {
                return false;
            }
            number = buf[0x2e];
        } else {
            number = buf[0x24];
        }
        /* Two newcomers wanting the same number: step aside. An already
         * ACTIVE session keeps its number; the claimant's own keep-alives
         * would move us if it takes it anyway. */
        if (claiming_number(s) && number == s->device_number) {
            dj_link_session_note_device(s, number, now_ms);
            reclaim(s, now_ms);
            return true;
        }
        return false;
    case TYPE_CONFLICT:
        number = buf[0x24];
        if ((claiming_number(s) || s->phase == DJ_LINK_CLAIM_ACTIVE) &&
            number == s->device_number) {
            dj_link_session_note_device(s, number, now_ms);
            reclaim(s, now_ms);
            return true;
        }
        return false;
    case TYPE_ASSIGN_DONE:
        dj_link_session_note_device(s, buf[0x24], now_ms);
        if (s->phase == DJ_LINK_CLAIM_FINAL) {
            s->phase = DJ_LINK_CLAIM_ACTIVE;
            s->startup_flags = 0x01u;
            s->iteration = 1u;
            s->next_ms = now_ms;
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* Per-querier-IP limiter; a full table of fresh entries drops the reply. */
static bool reply_allowed(dj_link_session_t *s, uint32_t ip, uint32_t now_ms)
{
    dj_link_reply_slot_t *slot = NULL;
    for (size_t i = 0; i < DJ_LINK_REPLY_LIMIT_SLOTS; i++) {
        dj_link_reply_slot_t *e = &s->replies[i];
        if (e->ip == ip) {
            slot = e;
            break;
        }
        if (!slot || e->ip == 0u ||
            (slot->ip != 0u &&
             (uint32_t)(now_ms - e->last_ms) > (uint32_t)(now_ms - slot->last_ms))) {
            slot = e;
        }
    }
    if (slot->ip != 0u &&
        (uint32_t)(now_ms - slot->last_ms) < DJ_LINK_MEDIA_REPLY_MIN_MS) {
        return false;
    }
    slot->ip = ip;
    slot->last_ms = now_ms;
    return true;
}

int dj_link_session_media_reply(dj_link_session_t *s, const uint8_t *buf, size_t len,
                                uint32_t src_ip, uint32_t track_count, uint32_t now_ms,
                                uint8_t *out, size_t cap)
{
    if (!s || !out || !dj_link_session_active(s) || len < MEDIA_QUERY_MIN_LEN ||
        !djlink_packet_is_valid(buf, len) || buf[0x0a] != TYPE_MEDIA_QUERY) {
        return 0;
    }
    /* The reply goes where the sender says (0x24); only honour its own
     * address, so we can never be used to reflect packets elsewhere. */
    if (djlink_rd32(&buf[0x24]) != src_ip || src_ip == 0u ||
        buf[0x2b] != s->device_number || buf[0x2f] != DJLINK_SLOT_USB ||
        track_count == 0u || !reply_allowed(s, src_ip, now_ms)) {
        return 0;
    }
    djlink_media_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    djlink_name_from_str(s->name, resp.name);
    resp.device_number = s->device_number;
    resp.source_device = s->device_number;
    resp.slot = DJLINK_SLOT_USB;
    strncpy(resp.media_name, "PAJONIIIR LIBRARY", sizeof(resp.media_name) - 1u);
    resp.track_count = track_count > 0xffffu ? 0xffffu : (uint16_t)track_count;
    resp.color = DJLINK_MEDIA_COLOR_AQUA;
    resp.track_type = 0x01u;  /* rekordbox database present */
    int n = djlink_media_resp_build(&resp, out, cap);
    return n > 0 ? n : 0;
}

dj_link_load_verdict_t dj_link_session_check_load(const dj_link_session_t *s,
                                                  const uint8_t *buf, size_t len,
                                                  bool unicast, dj_link_load_cmd_t *out)
{
    dj_link_load_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    djlink_load_track_t lt;
    dj_link_load_verdict_t verdict = DJ_LINK_LOAD_OK;
    if (djlink_load_track_parse(buf, len, &lt) != DJLINK_OK) {
        verdict = DJ_LINK_LOAD_BAD_PACKET;
    } else {
        cmd.sender_number = lt.sender_number;
        cmd.source_device = lt.target_player;  /* misnamed in esp-djlink 0.2.0 */
        cmd.source_slot = lt.slot;
        cmd.rekordbox_id = lt.rekordbox_id;
        cmd.dest_zero_based = buf[LOAD_DEST_OFFSET];
        uint8_t ours = dj_link_session_number(s);
        if (ours == 0u) {
            verdict = DJ_LINK_LOAD_NOT_JOINED;
        } else if (!unicast) {
            verdict = DJ_LINK_LOAD_NOT_UNICAST;
        } else if (cmd.sender_number == 0u || cmd.sender_number == ours) {
            verdict = DJ_LINK_LOAD_BAD_SENDER;
        } else if (cmd.rekordbox_id == 0u) {
            verdict = DJ_LINK_LOAD_NO_TRACK;
        } else if (cmd.source_device != ours || cmd.source_slot != DJLINK_SLOT_USB) {
            verdict = DJ_LINK_LOAD_REMOTE_SOURCE;
        }
    }
    if (out) {
        *out = cmd;
    }
    return verdict;
}

const char *dj_link_load_verdict_str(dj_link_load_verdict_t verdict)
{
    switch (verdict) {
    case DJ_LINK_LOAD_OK:            return "ok";
    case DJ_LINK_LOAD_BAD_PACKET:    return "malformed";
    case DJ_LINK_LOAD_NOT_JOINED:    return "no player number yet";
    case DJ_LINK_LOAD_NOT_UNICAST:   return "not unicast to us";
    case DJ_LINK_LOAD_BAD_SENDER:    return "bad sender number";
    case DJ_LINK_LOAD_REMOTE_SOURCE: return "source is another player (v248+)";
    case DJ_LINK_LOAD_NO_TRACK:      return "no track id";
    default:                         return "?";
    }
}

int dj_link_session_load_ack(const dj_link_session_t *s, uint8_t *out, size_t cap)
{
    uint8_t ours = dj_link_session_number(s);
    if (ours == 0u) {
        return 0;
    }
    int n = djlink_load_ack_build(ours, s->name, out, cap);
    return n > 0 ? n : 0;
}

int dj_link_pick_target_deck(bool deck0_playing, bool deck1_playing,
                             bool deck0_loaded, bool deck1_loaded)
{
    if (deck0_playing && deck1_playing) {
        return -1;
    }
    if (deck0_playing) {
        return 1;
    }
    if (deck1_playing) {
        return 0;
    }
    if (deck0_loaded && !deck1_loaded) {
        return 1;
    }
    return 0;
}
