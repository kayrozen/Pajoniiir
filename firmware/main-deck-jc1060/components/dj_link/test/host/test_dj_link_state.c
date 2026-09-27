/* v246: host tests for the DJ Link observer peer table.
 * v247: virtual-CDJ session (claim, keep-alive, media reply, load-track). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dj_link_session.h"
#include "dj_link_state.h"
#include "djlink/claim.h"
#include "djlink/media.h"
#include "djlink/mixer.h"
#include "djlink/beat.h"
#include "djlink/status.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

#define PITCH_ZERO 0x100000

static size_t make_beat(uint8_t *buf, uint8_t dev, const char *name,
                        uint16_t bpm100, int32_t pitch_raw, uint8_t in_bar,
                        uint32_t next_beat_ms)
{
    djlink_beat_t b;
    memset(&b, 0, sizeof(b));
    djlink_name_from_str(name, b.name);
    b.device_number = dev;
    b.bpm100 = bpm100;
    b.pitch_raw = pitch_raw;
    b.beat_in_bar = in_bar;
    b.next_beat_ms = next_beat_ms;
    return (size_t)djlink_beat_build(&b, buf, DJLINK_MAX_PACKET);
}

static size_t make_position(uint8_t *buf, uint8_t dev, uint32_t playhead_ms,
                            int32_t bpm10)
{
    djlink_position_t p;
    memset(&p, 0, sizeof(p));
    djlink_name_from_str("CDJ-3000", p.name);
    p.device_number = dev;
    p.track_length_s = 300;
    p.playhead_ms = playhead_ms;
    p.pitch_x100 = 250;
    p.bpm10 = bpm10;
    return (size_t)djlink_position_build(&p, buf, DJLINK_MAX_PACKET);
}

/* Hand-built CDJ status (type 0x0a), 0xd4 bytes like a nexus player. */
static size_t make_status(uint8_t *buf, uint8_t dev, uint8_t flags,
                          uint16_t bpm100, int32_t pitch_raw)
{
    const size_t len = 0xd4;
    memset(buf, 0, len);
    memcpy(buf, DJLINK_MAGIC, DJLINK_MAGIC_LEN);
    buf[0x0a] = DJLINK_TYPE_CDJ_STATUS;
    djlink_name_from_str("CDJ-3000", &buf[0x0b]);
    buf[0x20] = 3;
    buf[0x21] = dev;
    buf[0x22] = 0;
    buf[0x23] = (uint8_t)(len - 0x24);
    buf[0x89] = flags;
    djlink_wr32(&buf[0x8c], (uint32_t)pitch_raw);
    buf[0x90] = 0x80;
    buf[0x92] = (uint8_t)(bpm100 >> 8);
    buf[0x93] = (uint8_t)bpm100;
    buf[0xa6] = 2;
    return len;
}

static void test_drop_to_null(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_table_reset(&t);

    size_t n = make_beat(buf, 1, "CDJ-3000", 17420, PITCH_ZERO, 1, 344);
    buf[0] ^= 0xff;   /* broken magic */
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 10) == DJ_LINK_RX_DROPPED);

    n = make_beat(buf, 1, "CDJ-3000", 17420, PITCH_ZERO, 1, 344);
    /* Beat on the status port, truncated beat, unknown type. */
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_STATUS, buf, n, 10) == DJ_LINK_RX_DROPPED);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n - 1, 10) == DJ_LINK_RX_DROPPED);
    buf[0x0a] = 0x02;   /* fader-start command: must never be acted on */
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 10) == DJ_LINK_RX_DROPPED);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, NULL, 0, 10) == DJ_LINK_RX_DROPPED);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, DJLINK_MAX_PACKET + 1u, 10) ==
          DJ_LINK_RX_DROPPED);

    dj_link_summary_t s;
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == 0);
    CHECK(s.rx_accepted == 0);
    CHECK(s.rx_dropped == 6);
}

static void test_beat_master_inferred(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_table_reset(&t);

    size_t n = make_beat(buf, 1, "CDJ-3000", 17420, PITCH_ZERO, 3, 344);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 1000) == DJ_LINK_RX_ACCEPTED);

    dj_link_summary_t s;
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == 1);
    CHECK(s.has_master);
    CHECK(!s.master_confirmed);
    CHECK(s.master.device_number == 1);
    CHECK(strcmp(s.master.name, "CDJ-3000") == 0);
    CHECK(fabsf(s.master.bpm - 174.2f) < 0.01f);
    CHECK(s.master.beat_in_bar == 3);

    CHECK(fabsf(dj_link_peer_beat_phase(&s.master, 1000) - 0.0f) < 1e-6f);
    CHECK(fabsf(dj_link_peer_beat_phase(&s.master, 1172) - 0.5f) < 1e-6f);
    CHECK(dj_link_peer_beat_phase(&s.master, 1344) < 0.0f);

    char text[64];
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON joining - CDJ-3000 #1? 174.2 BPM") == 0);

    /* A later beat from player 2 moves the inferred master. */
    n = make_beat(buf, 2, "CDJ-3000", 12800, PITCH_ZERO + PITCH_ZERO / 50, 1, 469);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 1100) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == 2);
    CHECK(s.master.device_number == 2);
    CHECK(fabsf(s.master.bpm - 130.56f) < 0.01f);   /* 128 * 1.02 */
    CHECK(fabsf(s.master.pitch_pct - 2.0f) < 0.01f);
}

static void test_status_master_wins(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_table_reset(&t);

    size_t n = make_status(buf, 2, DJLINK_FLAG_MASTER | DJLINK_FLAG_ON_AIR | DJLINK_FLAG_PLAYING,
                           17420, PITCH_ZERO);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_STATUS, buf, n, 500) == DJ_LINK_RX_ACCEPTED);
    /* Status on the beat port is dropped. */
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 500) == DJ_LINK_RX_DROPPED);
    n = make_beat(buf, 1, "CDJ-3000", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 600) == DJ_LINK_RX_ACCEPTED);

    dj_link_summary_t s;
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.has_master);
    CHECK(s.master_confirmed);
    CHECK(s.master.device_number == 2);
    CHECK(s.master.on_air);
    CHECK(s.master.playing);

    char text[64];
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON joining - CDJ-3000 #2 174.2 BPM ON AIR") == 0);

    /* No track loaded: BPM forgotten. */
    n = make_status(buf, 2, DJLINK_FLAG_MASTER, 0xffff, PITCH_ZERO);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_STATUS, buf, n, 700) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON joining - CDJ-3000 #2 --- BPM") == 0);
}

static void test_position(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_table_reset(&t);

    size_t n = make_position(buf, 3, 61234, 1742);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 42) == DJ_LINK_RX_ACCEPTED);
    const dj_link_peer_t *p = &t.peers[0];
    CHECK(p->in_use && p->device_number == 3);
    CHECK(p->has_position && p->playhead_ms == 61234 && p->track_length_s == 300);
    CHECK(fabsf(p->bpm - 174.2f) < 0.01f);
    CHECK(fabsf(p->pitch_pct - 2.5f) < 0.01f);

    /* Unknown BPM (-1) keeps the previous value. */
    n = make_position(buf, 3, 61300, -1);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 72) == DJ_LINK_RX_ACCEPTED);
    CHECK(p->playhead_ms == 61300);
    CHECK(fabsf(p->bpm - 174.2f) < 0.01f);

    /* Position alone never makes a master. */
    dj_link_summary_t s;
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(!s.has_master);
    char text[64];
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON joining - 1 player, no master") == 0);
}

/* v248: senders' IPs and the browsable player list, by number. */
static void test_players_ip(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_summary_t s;
    dj_link_table_reset(&t);

    size_t n = make_beat(buf, 3, "CDJ-3000", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_BEAT, buf, n, 0xc0a80103u, 10) ==
          DJ_LINK_RX_ACCEPTED);
    n = make_status(buf, 1, 0, 12000, PITCH_ZERO);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_STATUS, buf, n, 0xc0a80101u, 11) ==
          DJ_LINK_RX_ACCEPTED);
    /* No IP known (plain ingest): listed as a peer, not browsable. */
    n = make_position(buf, 2, 1000, 1200);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 12) == DJ_LINK_RX_ACCEPTED);
    /* Not a player number. */
    n = make_beat(buf, 17, "rekordbox", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_BEAT, buf, n, 0xc0a80111u, 13) ==
          DJ_LINK_RX_ACCEPTED);
    /* Dropped packets never touch an IP. */
    buf[0] ^= 0xffu;
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_BEAT, buf, n, 0xc0a80199u, 14) ==
          DJ_LINK_RX_DROPPED);

    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == 4);
    CHECK(s.player_count == 2);
    CHECK(s.players[0].number == 1 && s.players[0].ip == 0xc0a80101u);
    CHECK(s.players[1].number == 3 && s.players[1].ip == 0xc0a80103u);
    CHECK(strcmp(s.players[1].name, "CDJ-3000") == 0);

    /* A later packet without an IP keeps the known one; a new IP replaces it. */
    n = make_beat(buf, 3, "CDJ-3000", 12000, PITCH_ZERO, 2, 500);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 20) == DJ_LINK_RX_ACCEPTED);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_BEAT, buf, n, 0xc0a80133u, 21) ==
          DJ_LINK_RX_ACCEPTED);
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.players[1].ip == 0xc0a80133u);
}

static void test_capacity_and_expiry(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_table_reset(&t);

    for (uint8_t dev = 1; dev <= DJ_LINK_MAX_PEERS; dev++) {
        size_t n = make_beat(buf, dev, "CDJ-3000", 12000, PITCH_ZERO, 1, 500);
        CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 100u * dev) ==
              DJ_LINK_RX_ACCEPTED);
    }
    /* A seventh device evicts the stalest (device 1). */
    size_t n = make_beat(buf, 17, "rekordbox", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 1000) == DJ_LINK_RX_ACCEPTED);
    dj_link_summary_t s;
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == DJ_LINK_MAX_PEERS);
    bool has1 = false, has17 = false;
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        has1 |= t.peers[i].in_use && t.peers[i].device_number == 1;
        has17 |= t.peers[i].in_use && t.peers[i].device_number == 17;
    }
    CHECK(!has1 && has17);

    /* Device number 0 is not a player. */
    n = make_beat(buf, 0, "BAD", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 1000) == DJ_LINK_RX_DROPPED);

    /* Silence past the timeout clears everyone; wrap-safe. */
    CHECK(!dj_link_table_expire(&t, 200 + DJ_LINK_PEER_TIMEOUT_MS));
    CHECK(dj_link_table_expire(&t, 1000 + DJ_LINK_PEER_TIMEOUT_MS + 1));
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == 0 && !s.has_master);

    dj_link_table_reset(&t);
    n = make_beat(buf, 1, "CDJ-3000", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 0xfffffff0u) == DJ_LINK_RX_ACCEPTED);
    CHECK(!dj_link_table_expire(&t, 0x100u));
    CHECK(dj_link_table_expire(&t, DJ_LINK_PEER_TIMEOUT_MS));
}

static void test_format_states(void)
{
    char text[64];
    dj_link_summary_t s;
    memset(&s, 0, sizeof(s));
    dj_link_format_status(NULL, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: OFF") == 0);
    s.state = DJ_LINK_STATE_OFF;
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: OFF") == 0);
    s.state = DJ_LINK_STATE_WAIT_IP;
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON - waiting for Ethernet IP") == 0);
    s.state = DJ_LINK_STATE_LISTENING;
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON joining - no players") == 0);
    s.our_number = 4;
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON P4 - no players") == 0);
    /* Truncation stays NUL-terminated. */
    char tiny[8];
    dj_link_format_status(&s, tiny, sizeof(tiny));
    CHECK(strlen(tiny) == sizeof(tiny) - 1u);
}

/* ---- v247 virtual-CDJ session ------------------------------------------ */

static const uint8_t k_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
#define OUR_IP  0xc0a80a0au   /* 192.168.10.10 */
#define PEER_IP 0xc0a80a0bu   /* 192.168.10.11 */

/* Poll every 100 ms from *t until the session is ACTIVE; returns the number
 * of packets sent and records their types. */
static int join(dj_link_session_t *s, uint32_t *t, uint8_t *types, int max_types)
{
    uint8_t buf[DJLINK_MAX_PACKET];
    int sent = 0;
    for (int i = 0; i < 200 && !dj_link_session_active(s); i++) {
        int n = dj_link_session_poll(s, *t, buf, sizeof(buf));
        if (n > 0) {
            if (sent < max_types) {
                types[sent] = buf[0x0a];
            }
            sent++;
        }
        *t += 100u;
    }
    return sent;
}

static size_t make_keepalive(uint8_t *buf, uint8_t dev, uint8_t mac_last)
{
    djlink_keepalive_t ka;
    memset(&ka, 0, sizeof(ka));
    ka.name = "CDJ-3000";
    ka.device_number = dev;
    ka.ip = PEER_IP;
    ka.mac[5] = mac_last;
    return (size_t)djlink_keepalive_build(&ka, buf, DJLINK_MAX_PACKET);
}

static void test_session_join_alone(void)
{
    dj_link_session_t s;
    uint8_t buf[DJLINK_MAX_PACKET];
    uint8_t types[16];
    uint32_t t = 1000;
    dj_link_session_reset(&s);
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) == 0);   /* IDLE: silent */

    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, t);
    CHECK(dj_link_session_number(&s) == 0);
    int sent = join(&s, &t, types, 16);
    CHECK(sent == 12);
    static const uint8_t expect[12] = { 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00,
                                        0x02, 0x02, 0x02, 0x04, 0x04, 0x04 };
    CHECK(memcmp(types, expect, sizeof(expect)) == 0);
    CHECK(dj_link_session_number(&s) == 4);

    /* First keep-alive immediately, then every 2 s, nothing in between. */
    int n = dj_link_session_poll(&s, t, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_KEEPALIVE_PACKET_LEN);
    CHECK(buf[0x0a] == 0x06 && buf[0x24] == 4);
    CHECK(buf[0x25] == 0x02);                       /* booted alone */
    CHECK(memcmp(&buf[0x26], k_mac, 6) == 0);
    CHECK(djlink_rd32(&buf[0x2c]) == OUR_IP);
    CHECK(buf[0x30] == 1);                          /* only us */
    CHECK(buf[0x35] == 0x00);
    CHECK(dj_link_session_poll(&s, t + 1999u, buf, sizeof(buf)) == 0);
    CHECK(dj_link_session_poll(&s, t + 2000u, buf, sizeof(buf)) > 0);
}

static void test_session_claim_packets(void)
{
    dj_link_session_t s;
    uint8_t buf[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    dj_link_session_reset(&s);
    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, t);
    /* Players 4 and 3 heard while announcing: we take 2. */
    dj_link_session_note_device(&s, 4, 100);
    dj_link_session_note_device(&s, 3, 150);
    for (int i = 0; i < 6; i++, t += 300u) {
        CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) > 0);
    }
    int n = dj_link_session_poll(&s, t, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_CLAIM_IP_PACKET_LEN);
    CHECK(buf[0x0a] == 0x02);
    CHECK(buf[0x21] == DJLINK_DEVICE_TYPE_CDJ);
    CHECK(djlink_rd32(&buf[0x24]) == OUR_IP);
    CHECK(memcmp(&buf[0x28], k_mac, 6) == 0);
    CHECK(buf[0x2e] == 2 && buf[0x2f] == 1 && buf[0x31] == 0x01);
}

static void test_session_conflicts(void)
{
    dj_link_session_t s;
    uint8_t buf[DJLINK_MAX_PACKET];
    uint8_t types[16];
    uint32_t t = 0;
    dj_link_session_reset(&s);
    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, t);
    join(&s, &t, types, 16);
    CHECK(dj_link_session_number(&s) == 4);

    /* Our own keep-alive (same MAC) or anything from our IP is ignored. */
    djlink_keepalive_t own = { .name = "PAJONIIIR", .device_number = 4, .ip = OUR_IP };
    memcpy(own.mac, k_mac, 6);
    size_t n = (size_t)djlink_keepalive_build(&own, buf, sizeof(buf));
    CHECK(!dj_link_session_on_discovery(&s, buf, n, PEER_IP, t));
    n = make_keepalive(buf, 4, 0x99);
    CHECK(!dj_link_session_on_discovery(&s, buf, n, OUR_IP, t));
    CHECK(dj_link_session_number(&s) == 4);

    /* 0x08 defending player 4 -> re-claim on another number. */
    memset(buf, 0, 0x29);
    memcpy(buf, DJLINK_MAGIC, DJLINK_MAGIC_LEN);
    buf[0x0a] = 0x08;
    buf[0x24] = 4;
    CHECK(dj_link_session_on_discovery(&s, buf, 0x29, PEER_IP, t));
    CHECK(!dj_link_session_active(&s));
    CHECK(s.phase == DJ_LINK_CLAIM_STAGE2 && s.device_number == 3);
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) > 0 && buf[0x2e] == 3);

    /* Another newcomer claiming 3 while we claim it: step aside to 2. */
    djlink_claim_ip_t other = { .name = "CDJ-3000", .device_type = 2, .ip = PEER_IP,
                                .device_number = 3, .iteration = 1, .auto_assign = 1 };
    other.mac[5] = 0x77;
    n = (size_t)djlink_claim_ip_build(&other, buf, sizeof(buf));
    CHECK(dj_link_session_on_discovery(&s, buf, n, PEER_IP, t));
    CHECK(s.device_number == 2);

    /* 0x05 "assignment finished" during the final stage -> ACTIVE now. */
    t += 300u;
    for (int i = 0; i < 3; i++, t += 300u) {
        dj_link_session_poll(&s, t, buf, sizeof(buf));
    }
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) > 0 && buf[0x0a] == 0x04);
    djlink_assign_finished_t done = { .name = "DJM-900NXS2", .device_number = 33 };
    n = (size_t)djlink_assign_finished_build(&done, buf, sizeof(buf));
    CHECK(dj_link_session_on_discovery(&s, buf, n, PEER_IP, t));
    CHECK(dj_link_session_number(&s) == 2);
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) > 0);
    CHECK(buf[0x0a] == 0x06 && buf[0x24] == 2 && buf[0x25] == 0x01);
    CHECK(buf[0x30] == 4);    /* us + 4, 3 and the mixer */

    /* A keep-alive from someone else carrying our number moves us. */
    n = make_keepalive(buf, 2, 0x42);
    CHECK(dj_link_session_on_discovery(&s, buf, n, PEER_IP, t));
    CHECK(!dj_link_session_active(&s) && s.device_number == 1);
    CHECK(s.reclaims == 3);
}

static void test_session_numbers_exhausted(void)
{
    dj_link_session_t s;
    uint8_t buf[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    dj_link_session_reset(&s);
    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, t);
    for (uint8_t d = 1; d <= 6; d++) {
        dj_link_session_note_device(&s, d, 0);
    }
    for (int i = 0; i < 6; i++, t += 300u) {
        CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) > 0);
    }
    CHECK(s.phase == DJ_LINK_CLAIM_NO_NUMBER);
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) == 0);   /* silent */
    /* Players 1..4 age out, 5 and 6 are refreshed: we get 4. */
    dj_link_session_note_device(&s, 5, 5500);
    dj_link_session_note_device(&s, 6, 5500);
    t = 6000;
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) == 0);
    CHECK(s.phase == DJ_LINK_CLAIM_STAGE2 && s.device_number == 4);

    /* Only 5 free: keep-alive carries the CDJ-3000 0x64 marker. */
    dj_link_session_reset(&s);
    t = 0;
    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, t);
    for (uint8_t d = 1; d <= 4; d++) {
        dj_link_session_note_device(&s, d, 0);
    }
    uint8_t types[16];
    join(&s, &t, types, 16);
    CHECK(dj_link_session_number(&s) == 5);
    CHECK(dj_link_session_poll(&s, t, buf, sizeof(buf)) > 0 && buf[0x35] == 0x64);
}

static dj_link_session_t make_active(uint32_t *t)
{
    dj_link_session_t s;
    uint8_t types[16];
    dj_link_session_reset(&s);
    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, *t);
    join(&s, t, types, 16);
    return s;
}

static size_t make_media_query(uint8_t *buf, uint32_t reply_ip, uint8_t dr, uint8_t slot)
{
    djlink_media_query_t q = { .name = "CDJ-3000", .sender_number = 1,
                               .reply_ip = reply_ip, .source_device = dr, .slot = slot };
    return (size_t)djlink_media_query_build(&q, buf, DJLINK_MAX_PACKET);
}

static void test_session_media_reply(void)
{
    uint8_t q[DJLINK_MAX_PACKET];
    uint8_t out[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    dj_link_session_t idle;
    dj_link_session_reset(&idle);
    size_t n = make_media_query(q, PEER_IP, 4, DJLINK_SLOT_USB);
    CHECK(dj_link_session_media_reply(&idle, q, n, PEER_IP, 10, t, out, sizeof(out)) == 0);

    dj_link_session_t s = make_active(&t);
    int r = dj_link_session_media_reply(&s, q, n, PEER_IP, 1234, t, out, sizeof(out));
    CHECK(r == (int)DJLINK_MEDIA_RESP_PACKET_LEN);
    djlink_media_resp_t resp;
    CHECK(djlink_media_resp_parse(out, (size_t)r, &resp) == DJLINK_OK);
    CHECK(resp.device_number == 4 && resp.source_device == 4);
    CHECK(resp.slot == DJLINK_SLOT_USB);
    CHECK(resp.track_count == 1234);
    CHECK(resp.track_type == 1);
    CHECK(resp.color == DJLINK_MEDIA_COLOR_AQUA);
    CHECK(strcmp(resp.media_name, "PAJONIIIR LIBRARY") == 0);

    /* Rate limit per querier, other queriers unaffected. */
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 1234, t + 499u, out, sizeof(out)) == 0);
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 1234, t + 500u, out, sizeof(out)) > 0);
    size_t n2 = make_media_query(q, PEER_IP + 1u, 4, DJLINK_SLOT_USB);
    CHECK(dj_link_session_media_reply(&s, q, n2, PEER_IP + 1u, 1234, t + 501u, out, sizeof(out)) > 0);
    t += 2000u;

    /* Ignored: reply IP not the sender, other slot, other player, no
     * library, bad magic, huge counts clamp. */
    n = make_media_query(q, PEER_IP + 7u, 4, DJLINK_SLOT_USB);
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 10, t, out, sizeof(out)) == 0);
    n = make_media_query(q, PEER_IP, 4, DJLINK_SLOT_SD);
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 10, t, out, sizeof(out)) == 0);
    n = make_media_query(q, PEER_IP, 2, DJLINK_SLOT_USB);
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 10, t, out, sizeof(out)) == 0);
    n = make_media_query(q, PEER_IP, 4, DJLINK_SLOT_USB);
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 0, t, out, sizeof(out)) == 0);
    q[1] ^= 0xff;
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 10, t, out, sizeof(out)) == 0);
    q[1] ^= 0xff;
    CHECK(dj_link_session_media_reply(&s, q, n - 1u, PEER_IP, 10, t, out, sizeof(out)) == 0);
    CHECK(dj_link_session_media_reply(&s, q, n, PEER_IP, 70000, t, out, sizeof(out)) > 0);
    CHECK(djlink_media_resp_parse(out, DJLINK_MEDIA_RESP_PACKET_LEN, &resp) == DJLINK_OK &&
          resp.track_count == 0xffff);
}

static size_t make_load(uint8_t *buf, uint8_t sender, uint8_t source, uint8_t slot,
                        uint32_t id)
{
    djlink_load_track_t lt = { .name = "PAJONIIIR-B", .sender_number = sender,
                               .target_player = source, .slot = slot,
                               .rekordbox_id = id };
    size_t n = (size_t)djlink_load_track_build(&lt, buf, DJLINK_MAX_PACKET);
    buf[0x40] = 3;   /* zero-based destination: informative only */
    return n;
}

static void test_session_load_track(void)
{
    uint8_t buf[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    dj_link_load_cmd_t cmd;

    dj_link_session_t idle;
    dj_link_session_reset(&idle);
    size_t n = make_load(buf, 2, 4, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&idle, buf, n, true, &cmd) == DJ_LINK_LOAD_NOT_JOINED);

    dj_link_session_t s = make_active(&t);
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(cmd.sender_number == 2 && cmd.source_device == 4);
    CHECK(cmd.source_slot == DJLINK_SLOT_USB && cmd.rekordbox_id == 1234);
    CHECK(cmd.dest_zero_based == 3);

    CHECK(dj_link_session_check_load(&s, buf, n, false, &cmd) == DJ_LINK_LOAD_NOT_UNICAST);
    CHECK(dj_link_session_check_load(&s, buf, n - 1u, true, &cmd) == DJ_LINK_LOAD_BAD_PACKET);
    buf[3] ^= 0xff;
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_PACKET);
    n = make_load(buf, 4, 4, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_SENDER);
    n = make_load(buf, 0, 4, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_SENDER);
    n = make_load(buf, 2, 4, DJLINK_SLOT_USB, 0);
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_NO_TRACK);
    /* Track on the sender's own USB / our SD: needs NFS, refused for now. */
    n = make_load(buf, 2, 2, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_REMOTE_SOURCE);
    CHECK(cmd.source_device == 2);
    n = make_load(buf, 2, 4, DJLINK_SLOT_SD, 1234);
    CHECK(dj_link_session_check_load(&s, buf, n, true, &cmd) == DJ_LINK_LOAD_REMOTE_SOURCE);
    CHECK(strcmp(dj_link_load_verdict_str(DJ_LINK_LOAD_REMOTE_SOURCE),
                 "source is another player (v248+)") == 0);

    /* Ack 0x1a carries our number; none before we have one. */
    uint8_t ack[DJLINK_MAX_PACKET];
    int a = dj_link_session_load_ack(&s, ack, sizeof(ack));
    CHECK(a == (int)DJLINK_LOAD_ACK_PACKET_LEN);
    djlink_load_ack_t parsed;
    CHECK(djlink_load_ack_parse(ack, (size_t)a, &parsed) == DJLINK_OK);
    CHECK(parsed.device_number == 4);
    CHECK(dj_link_session_load_ack(&idle, ack, sizeof(ack)) == 0);
}

static void test_pick_target_deck(void)
{
    CHECK(dj_link_pick_target_deck(true, true, true, true) == -1);
    CHECK(dj_link_pick_target_deck(true, false, true, true) == 1);
    CHECK(dj_link_pick_target_deck(false, true, true, true) == 0);
    CHECK(dj_link_pick_target_deck(false, false, false, false) == 0);
    CHECK(dj_link_pick_target_deck(false, false, true, false) == 1);
    CHECK(dj_link_pick_target_deck(false, false, false, true) == 0);
    CHECK(dj_link_pick_target_deck(false, false, true, true) == 0);
}

int main(void)
{
    test_drop_to_null();
    test_beat_master_inferred();
    test_status_master_wins();
    test_position();
    test_players_ip();
    test_capacity_and_expiry();
    test_format_states();
    test_session_join_alone();
    test_session_claim_packets();
    test_session_conflicts();
    test_session_numbers_exhausted();
    test_session_media_reply();
    test_session_load_track();
    test_pick_target_deck();
    if (s_failures) {
        printf("%d dj_link_state check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_state tests passed\n");
    return 0;
}
