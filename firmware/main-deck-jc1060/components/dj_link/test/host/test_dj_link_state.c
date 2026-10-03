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

/* v297: a keep-alive 0x06 as vynull / rekordbox send it (layout of
 * djlink_keepalive_build, device type at 0x21 patched). */
static size_t make_typed_keepalive(uint8_t *buf, uint8_t dev, uint8_t type,
                                   const char *name, uint32_t ip)
{
    djlink_keepalive_t ka;
    memset(&ka, 0, sizeof(ka));
    ka.name = name;
    ka.device_number = dev;
    ka.ip = ip;
    size_t n = (size_t)djlink_keepalive_build(&ka, buf, DJLINK_MAX_PACKET);
    buf[0x21] = type;
    return n;
}

/* v297: a rekordbox source (vynull #17) only ever sends keep-alives: it must
 * still be listed, browsable as a collection. Mixers are not peers. */
static void test_keepalive_sources(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_summary_t s;
    char text[64];
    dj_link_table_reset(&t);

    size_t n = make_typed_keepalive(buf, 17, DJ_LINK_DEVICE_TYPE_REKORDBOX, "Vynull",
                                    0xc0a86493u);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_DISCOVERY, buf, n, 0xc0a86493u, 10) ==
          DJ_LINK_RX_ACCEPTED);
    /* A player's keep-alive without a source address: the IP at 0x2c. */
    n = make_typed_keepalive(buf, 2, DJLINK_DEVICE_TYPE_CDJ, "XDJ-XZ", 0xc0a86402u);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_DISCOVERY, buf, n, 11) == DJ_LINK_RX_ACCEPTED);

    /* Dropped: mixer, number 0, short, wrong port. */
    n = make_typed_keepalive(buf, 33, DJLINK_DEVICE_TYPE_MIXER, "DJM-900NXS2", 0xc0a86421u);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_DISCOVERY, buf, n, 0xc0a86421u, 12) ==
          DJ_LINK_RX_DROPPED);
    n = make_typed_keepalive(buf, 0, DJ_LINK_DEVICE_TYPE_REKORDBOX, "rekordbox", 0xc0a86494u);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_DISCOVERY, buf, n, 12) == DJ_LINK_RX_DROPPED);
    n = make_typed_keepalive(buf, 18, DJ_LINK_DEVICE_TYPE_REKORDBOX, "rekordbox", 0xc0a86494u);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_DISCOVERY, buf, n - 1u, 12) ==
          DJ_LINK_RX_DROPPED);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_STATUS, buf, n, 12) == DJ_LINK_RX_DROPPED);

    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.rx_accepted == 2 && s.rx_dropped == 4);
    CHECK(s.peer_count == 2);
    CHECK(s.player_count == 2);
    CHECK(s.players[0].number == 2 && s.players[0].ip == 0xc0a86402u);
    CHECK(!s.players[0].collection);
    CHECK(s.players[1].number == 17 && s.players[1].ip == 0xc0a86493u);
    CHECK(s.players[1].collection);
    CHECK(strcmp(s.players[1].name, "Vynull") == 0);
    CHECK(!s.has_master);
    s.our_number = 3;
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON P3 - 2 players, no master") == 0);
    s.our_number_d2 = 4;
    dj_link_format_status(&s, text, sizeof(text));
    CHECK(strcmp(text, "DJ LINK: ON P3+P4 - 2 players, no master") == 0);
    s.our_number_d2 = 0;

    /* A keep-alive refreshes a player without forgetting its status. */
    n = make_status(buf, 2, DJLINK_FLAG_MASTER | DJLINK_FLAG_PLAYING, 12800, PITCH_ZERO);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_STATUS, buf, n, 0xc0a86402u, 20) ==
          DJ_LINK_RX_ACCEPTED);
    n = make_typed_keepalive(buf, 2, DJLINK_DEVICE_TYPE_CDJ, "XDJ-XZ", 0xc0a86402u);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_DISCOVERY, buf, n, 0xc0a86402u, 21) ==
          DJ_LINK_RX_ACCEPTED);
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.has_master && s.master_confirmed && s.master.device_number == 2);
    CHECK(s.master.playing && fabsf(s.master.bpm - 128.0f) < 0.01f);

    /* Keep-alives every 1.5 s keep the source; silence expires it. */
    uint32_t now = 21;
    for (int i = 0; i < 6; i++) {
        now += 1500u;
        n = make_typed_keepalive(buf, 17, DJ_LINK_DEVICE_TYPE_REKORDBOX, "Vynull",
                                 0xc0a86493u);
        dj_link_table_ingest_from(&t, DJLINK_PORT_DISCOVERY, buf, n, 0xc0a86493u, now);
        dj_link_table_expire(&t, now);
    }
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.player_count == 1 && s.players[0].number == 17 && s.players[0].collection);
    CHECK(dj_link_table_expire(&t, now + DJ_LINK_PEER_TIMEOUT_MS + 1u));
    dj_link_table_summarize(&t, DJ_LINK_STATE_LISTENING, &s);
    CHECK(s.peer_count == 0 && s.player_count == 0);
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
    CHECK(dj_link_session_check_load(&idle, 0, buf, n, true, &cmd) == DJ_LINK_LOAD_NOT_JOINED);

    dj_link_session_t s = make_active(&t);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(cmd.sender_number == 2 && cmd.source_device == 4);
    CHECK(cmd.source_slot == DJLINK_SLOT_USB && cmd.rekordbox_id == 1234);
    CHECK(cmd.dest_zero_based == 3);

    CHECK(dj_link_session_check_load(&s, 4, buf, n, false, &cmd) == DJ_LINK_LOAD_NOT_UNICAST);
    CHECK(dj_link_session_check_load(&s, 4, buf, n - 1u, true, &cmd) == DJ_LINK_LOAD_BAD_PACKET);
    buf[3] ^= 0xff;
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_PACKET);
    n = make_load(buf, 4, 4, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_SENDER);
    n = make_load(buf, 0, 4, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_SENDER);
    n = make_load(buf, 2, 4, DJLINK_SLOT_USB, 0);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_NO_TRACK);
    /* v298: a track on the sender's own media (vynull: #17, collection)
     * is accepted, the UI downloads it; our SD or no source is refused. */
    n = make_load(buf, 2, 2, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(cmd.source_device == 2);
    n = make_load(buf, 17, 17, DJLINK_SLOT_LAPTOP, 9);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(cmd.source_device == 17 && cmd.source_slot == DJLINK_SLOT_LAPTOP);
    n = make_load(buf, 2, 4, DJLINK_SLOT_SD, 1234);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_REMOTE_SOURCE);
    n = make_load(buf, 2, 0, DJLINK_SLOT_USB, 1234);
    CHECK(dj_link_session_check_load(&s, 4, buf, n, true, &cmd) == DJ_LINK_LOAD_REMOTE_SOURCE);
    CHECK(strcmp(dj_link_load_verdict_str(DJ_LINK_LOAD_REMOTE_SOURCE),
                 "no loadable media at the source") == 0);

    /* Ack 0x1a carries our number; none before we have one. */
    uint8_t ack[DJLINK_MAX_PACKET];
    int a = dj_link_session_load_ack(&s, ack, sizeof(ack));
    CHECK(a == (int)DJLINK_LOAD_ACK_PACKET_LEN);
    djlink_load_ack_t parsed;
    CHECK(djlink_load_ack_parse(ack, (size_t)a, &parsed) == DJLINK_OK);
    CHECK(parsed.device_number == 4);
    CHECK(dj_link_session_load_ack(&idle, ack, sizeof(ack)) == 0);
}

/* v308: join one deck of the pair beside the CDJs already on the network. */
static dj_link_session_t join_deck(uint32_t *t, uint8_t pair, uint8_t sibling,
                                   const uint8_t *cdjs, size_t cdj_count)
{
    dj_link_session_t s;
    uint8_t types[16];
    dj_link_session_reset(&s);
    dj_link_session_set_pair(&s, pair);
    dj_link_session_start(&s, "PAJONIIIR", k_mac, OUR_IP, *t);
    dj_link_session_set_sibling(&s, sibling);
    for (size_t i = 0; i < cdj_count; i++) {
        dj_link_session_note_device(&s, cdjs[i], *t);
    }
    join(&s, t, types, 16);
    CHECK(dj_link_session_active(&s));
    return s;
}

/* v308: vynull / beat-link list players by number, so deck 1 must hold the
 * lower one: deck 1 claims the low number of a free pair, deck 2 the next
 * free number above it, whatever the network already holds. */
static void test_session_pair_numbers(void)
{
    static const struct {
        uint8_t cdjs[4];
        uint8_t cdj_count;
        uint8_t d1, d2;
    } k_cases[] = {
        { { 0 }, 0, 3, 4 },                 /* alone: 3/4 like two CDJs */
        { { 1, 2 }, 2, 3, 4 },
        { { 4 }, 1, 2, 3 },
        { { 3 }, 1, 1, 2 },
        { { 1, 2, 3, 4 }, 4, 5, 6 },
        { { 1, 3, 5 }, 3, 4, 6 },           /* no free pair: still ordered */
    };
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        uint32_t t = 0;
        dj_link_session_t d1 = join_deck(&t, DJ_LINK_PAIR_LOW, 0, k_cases[i].cdjs,
                                         k_cases[i].cdj_count);
        dj_link_session_t d2 = join_deck(&t, DJ_LINK_PAIR_HIGH, dj_link_session_number(&d1),
                                         k_cases[i].cdjs, k_cases[i].cdj_count);
        if (dj_link_session_number(&d1) != k_cases[i].d1 ||
            dj_link_session_number(&d2) != k_cases[i].d2) {
            printf("pair case %zu: got %u/%u, want %u/%u\n", i,
                   (unsigned)dj_link_session_number(&d1), (unsigned)dj_link_session_number(&d2),
                   (unsigned)k_cases[i].d1, (unsigned)k_cases[i].d2);
            s_failures++;
        }
    }

    /* Same network, same numbers: a restart (reset + rejoin) gives 3/4 again. */
    for (int round = 0; round < 2; round++) {
        uint32_t t = 5000u * (uint32_t)round;
        dj_link_session_t d1 = join_deck(&t, DJ_LINK_PAIR_LOW, 0, NULL, 0);
        dj_link_session_t d2 = join_deck(&t, DJ_LINK_PAIR_HIGH, 3, NULL, 0);
        CHECK(dj_link_session_number(&d1) == 3 && dj_link_session_number(&d2) == 4);
    }

    /* Reset forgets the role: a lone session keeps the old preference. */
    dj_link_session_t s;
    dj_link_session_set_pair(&s, DJ_LINK_PAIR_LOW);
    dj_link_session_reset(&s);
    CHECK(s.pair == DJ_LINK_PAIR_NONE);
    dj_link_session_set_pair(NULL, DJ_LINK_PAIR_LOW);

    /* Re-claims keep the order: deck 1 goes below deck 2, deck 2 above
     * deck 1; with no room there they fall back to any free number. */
    uint32_t t = 0;
    static const uint8_t cdj3[] = { 3 };
    dj_link_session_t d1 = join_deck(&t, DJ_LINK_PAIR_LOW, 4, cdj3, 1);
    CHECK(dj_link_session_number(&d1) == 2);
    static const uint8_t cdj1[] = { 1 };
    d1 = join_deck(&t, DJ_LINK_PAIR_LOW, 2, cdj1, 1);
    CHECK(dj_link_session_number(&d1) == 4);
    dj_link_session_t d2 = join_deck(&t, DJ_LINK_PAIR_HIGH, 6, NULL, 0);
    CHECK(dj_link_session_number(&d2) == 4);
}

/* v298: both decks join from one MAC/IP and never share a number. Deck 2
 * starts once deck 1 is active, with deck 1's number as its sibling. */
static void test_session_two_decks(void)
{
    uint8_t buf[DJLINK_MAX_PACKET];
    uint8_t types[16];
    uint32_t t = 0;
    dj_link_session_t d1 = join_deck(&t, DJ_LINK_PAIR_LOW, 0, NULL, 0);
    CHECK(dj_link_session_number(&d1) == 3);

    static const uint8_t cdj1[] = { 1 };
    dj_link_session_t d2 = join_deck(&t, DJ_LINK_PAIR_HIGH, dj_link_session_number(&d1),
                                     cdj1, 1);
    CHECK(dj_link_session_number(&d2) == 4);
    dj_link_session_set_sibling(&d1, dj_link_session_number(&d2));

    /* Keep-alive counts the CDJ, the sibling and ourselves. */
    dj_link_session_note_device(&d2, 1, t);
    t += DJ_LINK_KEEPALIVE_MS;
    CHECK(dj_link_session_poll(&d2, t, buf, sizeof(buf)) > 0);
    CHECK(buf[0x0a] == 0x06 && buf[0x24] == 4 && buf[0x30] == 3);

    /* Deck 2 losing its number never re-claims deck 1's and stays above it. */
    memset(buf, 0, 0x29);
    memcpy(buf, DJLINK_MAGIC, DJLINK_MAGIC_LEN);
    buf[0x0a] = 0x08;
    buf[0x24] = 4;
    CHECK(dj_link_session_on_discovery(&d2, buf, 0x29, PEER_IP, t));
    CHECK(d2.device_number == 5);

    /* Loads: 0x40 (zero-based) names the deck; deck 1 is the library. */
    uint8_t numbers[2] = { 3, 0 };
    dj_link_load_cmd_t cmd;
    join(&d2, &t, types, 16);
    CHECK(dj_link_session_active(&d2) && dj_link_session_number(&d2) == 5);
    numbers[1] = 5;
    dj_link_session_set_sibling(&d1, 5);
    size_t n = make_load(buf, 1, 3, DJLINK_SLOT_USB, 77);
    buf[0x40] = 4;
    CHECK(dj_link_session_check_load(&d2, 3, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(dj_link_load_target_deck(numbers, &cmd) == 1);
    buf[0x40] = 2;
    CHECK(dj_link_session_check_load(&d1, 3, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(dj_link_load_target_deck(numbers, &cmd) == 0);
    buf[0x40] = 3;
    CHECK(dj_link_session_check_load(&d1, 3, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(dj_link_load_target_deck(numbers, &cmd) == -1);
    CHECK(dj_link_load_target_deck(NULL, &cmd) == -1);
    CHECK(dj_link_load_target_deck(numbers, NULL) == -1);
    /* The usual pair: player 3 is deck 1, player 4 deck 2. */
    const uint8_t pair[2] = { 3, 4 };
    buf[0x40] = 2;
    CHECK(dj_link_session_check_load(&d1, 3, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(dj_link_load_target_deck(pair, &cmd) == 0);
    buf[0x40] = 3;
    CHECK(dj_link_session_check_load(&d1, 3, buf, n, true, &cmd) == DJ_LINK_LOAD_OK);
    CHECK(dj_link_load_target_deck(pair, &cmd) == 1);
    /* Our other deck as sender, a track on deck 2's "USB", or no library:
     * refused. */
    n = make_load(buf, 3, 3, DJLINK_SLOT_USB, 77);
    CHECK(dj_link_session_check_load(&d2, 3, buf, n, true, &cmd) == DJ_LINK_LOAD_BAD_SENDER);
    n = make_load(buf, 1, 5, DJLINK_SLOT_USB, 77);
    CHECK(dj_link_session_check_load(&d2, 3, buf, n, true, &cmd) ==
          DJ_LINK_LOAD_REMOTE_SOURCE);
    n = make_load(buf, 1, 3, DJLINK_SLOT_USB, 77);
    CHECK(dj_link_session_check_load(&d2, 0, buf, n, true, &cmd) ==
          DJ_LINK_LOAD_REMOTE_SOURCE);
}

/* v298: the CDJ status each deck broadcasts. */
static void test_session_status(void)
{
    uint8_t buf[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    djlink_status_t st;
    dj_link_deck_report_t deck = {0};

    dj_link_session_t idle;
    dj_link_session_reset(&idle);
    CHECK(dj_link_session_status(&idle, 4, &deck, 1, buf, sizeof(buf)) == 0);

    dj_link_session_t s = make_active(&t);
    CHECK(dj_link_session_status(&s, 4, NULL, 1, buf, sizeof(buf)) == 0);
    CHECK(dj_link_session_status(&s, 4, &deck, 1, buf, 0x10) == 0);

    /* Empty deck. */
    int n = dj_link_session_status(&s, 4, &deck, 1, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_STATUS_PACKET_LEN);
    CHECK(djlink_status_parse(buf, (size_t)n, &st) == DJLINK_OK);
    CHECK(st.device_number == 4 && !st.active);
    CHECK(st.play_state == DJLINK_PLAY_NO_TRACK && st.bpm100 == 0xffffu);
    CHECK(st.source_device == 0 && st.rekordbox_id == 0 && st.flags == 0);
    CHECK(st.pitch_raw == 0x00100000);
    char name[DJLINK_NAME_LEN + 1];
    djlink_name_to_str(st.name, name);
    CHECK(strcmp(name, "PAJONIIIR") == 0);

    /* Our own track, paused, -8 %: on the library player's USB. */
    deck.loaded = true;
    deck.rekordbox_id = 501;
    deck.bpm100 = 12400;
    deck.pitch_centipercent = -800;
    n = dj_link_session_status(&s, 4, &deck, 2, buf, sizeof(buf));
    CHECK(djlink_status_parse(buf, (size_t)n, &st) == DJLINK_OK);
    CHECK(st.play_state == DJLINK_PLAY_PAUSE && !st.active);
    CHECK(st.source_device == 4 && st.source_slot == DJLINK_SLOT_USB);
    CHECK(st.rekordbox_id == 501 && st.bpm100 == 12400);
    CHECK(st.pitch_raw == 0x00100000 - 83886);   /* -8 % of 0x100000, truncated */
    CHECK(djlink_rd32(&buf[0xc8]) == 2u);

    /* A vynull collection track, playing, synced master. */
    deck.playing = true;
    deck.sync = true;
    deck.master = true;
    deck.source_number = 17;
    deck.source_slot = DJLINK_SLOT_LAPTOP;
    deck.bpm100 = 0;
    deck.pitch_centipercent = 10000;
    n = dj_link_session_status(&s, 4, &deck, 3, buf, sizeof(buf));
    CHECK(djlink_status_parse(buf, (size_t)n, &st) == DJLINK_OK);
    CHECK(st.play_state == DJLINK_PLAY_PLAYING && st.active);
    CHECK(st.source_device == 17 && st.source_slot == DJLINK_SLOT_LAPTOP);
    CHECK(st.bpm100 == 0xffffu);                  /* unknown BPM */
    CHECK(st.flags == (DJLINK_FLAG_PLAYING | DJLINK_FLAG_SYNC | DJLINK_FLAG_MASTER));
    CHECK(st.master_meaningful == 1);
    CHECK(st.pitch_raw == 0x00200000);            /* +100 % */
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

/* v301: a 120 BPM grid, the first beat at 250 ms, bars of four. */
#define GRID_BEATS 100u

static bool grid_get(const void *grid, size_t i, dj_link_report_beat_t *out)
{
    (void)grid;
    if (i >= GRID_BEATS) {
        return false;
    }
    out->time_ms = 250u + 500u * (uint32_t)i;
    out->beat_in_bar = (uint8_t)(i % 4u + 1u);
    return true;
}

static dj_link_deck_report_t playing_deck(uint32_t position_ms, uint32_t stamp_ms)
{
    dj_link_deck_report_t d;
    memset(&d, 0, sizeof(d));
    d.loaded = true;
    d.playing = true;
    d.bpm100 = 12000u;
    d.position_ms = position_ms;
    d.duration_ms = 60000u;
    d.speed_permille = 1000u;
    d.stamp_ms = stamp_ms;
    dj_link_report_beats(&d, &d, GRID_BEATS, grid_get);
    return d;
}

static void test_report_beats(void)
{
    dj_link_deck_report_t d = playing_deck(0u, 0u);
    CHECK(d.beat_count == DJ_LINK_REPORT_BEATS && d.beats[0].time_ms == 250u);
    d = playing_deck(1250u, 0u);                    /* on a beat: it is the current */
    CHECK(d.beats[0].time_ms == 1250u && d.beats[0].beat_in_bar == 3u);
    d = playing_deck(1300u, 0u);
    CHECK(d.beats[0].time_ms == 1250u && d.beats[1].time_ms == 1750u);
    CHECK(d.beats[15].time_ms == 1250u + 15u * 500u);
    d = playing_deck(49800u, 0u);                   /* past the last beat */
    CHECK(d.beat_count == 1u && d.beats[0].time_ms == 49750u);
    d = playing_deck(49700u, 0u);
    CHECK(d.beat_count == 2u);
    dj_link_report_beats(&d, NULL, GRID_BEATS, grid_get);
    CHECK(d.beat_count == 0u);
    d.beat_count = 3u;
    dj_link_report_beats(&d, &d, 0u, grid_get);
    CHECK(d.beat_count == 0u);
    dj_link_report_beats(NULL, &d, GRID_BEATS, grid_get);
}

static void test_deck_playhead(void)
{
    dj_link_deck_report_t d = playing_deck(10000u, 100u);
    CHECK(dj_link_deck_playhead(&d, 100u) == 10000u);
    CHECK(dj_link_deck_playhead(&d, 600u) == 10500u);
    CHECK(dj_link_deck_playhead(&d, 50u) == 10000u);    /* stamped after now */
    CHECK(dj_link_deck_playhead(&d, 5000u) == 11000u);  /* held at most 1 s */
    d.speed_permille = 500u;
    CHECK(dj_link_deck_playhead(&d, 600u) == 10250u);
    d.duration_ms = 10100u;
    CHECK(dj_link_deck_playhead(&d, 600u) == 10100u);
    d.duration_ms = 0u;                                 /* unknown: no clamp */
    CHECK(dj_link_deck_playhead(&d, 600u) == 10250u);
    d.playing = false;
    CHECK(dj_link_deck_playhead(&d, 600u) == 10000u);
    d.loaded = false;
    CHECK(dj_link_deck_playhead(&d, 600u) == 0u);
    CHECK(dj_link_deck_playhead(NULL, 600u) == 0u);

    /* Next beat 10250, wall time at the deck's speed. */
    d = playing_deck(10000u, 0u);
    CHECK(dj_link_deck_next_beat_in_ms(&d, 0u) == 250u);
    CHECK(dj_link_deck_next_beat_in_ms(&d, 100u) == 150u);
    d.speed_permille = 500u;
    CHECK(dj_link_deck_next_beat_in_ms(&d, 0u) == 500u);
    d.speed_permille = 0u;                              /* scratch hold */
    CHECK(dj_link_deck_next_beat_in_ms(&d, 0u) == UINT32_MAX);
    d = playing_deck(49800u, 0u);                       /* none ahead */
    CHECK(dj_link_deck_next_beat_in_ms(&d, 0u) == UINT32_MAX);
    d.playing = false;
    CHECK(dj_link_deck_next_beat_in_ms(&d, 0u) == UINT32_MAX);
}

/* v301: absolute position 0x0b, playing or paused, while a track is loaded. */
static void test_session_position(void)
{
    uint8_t buf[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    djlink_position_t pos;
    dj_link_deck_report_t d = playing_deck(10000u, 0u);
    d.duration_ms = 245500u;
    d.pitch_centipercent = 326;

    dj_link_session_t idle;
    dj_link_session_reset(&idle);
    CHECK(dj_link_session_position(&idle, &d, 0u, buf, sizeof(buf)) == 0);

    dj_link_session_t s = make_active(&t);
    CHECK(dj_link_session_position(&s, NULL, t, buf, sizeof(buf)) == 0);
    CHECK(dj_link_session_position(&s, &d, t, buf, 0x10) == 0);
    d.stamp_ms = t;
    int n = dj_link_session_position(&s, &d, t + 30u, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_POSITION_PACKET_LEN);
    CHECK(djlink_position_parse(buf, (size_t)n, &pos) == DJLINK_OK);
    CHECK(pos.device_number == 4 && pos.track_length_s == 246u);
    CHECK(pos.playhead_ms == 10030u);
    CHECK(pos.pitch_x100 == 326 && pos.bpm10 == 1239);  /* 120 x 1.0326 */

    d.playing = false;                                  /* paused: still sent */
    d.bpm100 = 0u;
    n = dj_link_session_position(&s, &d, t + 30u, buf, sizeof(buf));
    CHECK(n > 0 && djlink_position_parse(buf, (size_t)n, &pos) == DJLINK_OK);
    CHECK(pos.playhead_ms == 10000u && pos.bpm10 == -1);
    d.loaded = false;
    CHECK(dj_link_session_position(&s, &d, t, buf, sizeof(buf)) == 0);
}

/* v301: beat 0x28 when the extrapolated playhead crosses a grid beat. */
static void test_session_beat(void)
{
    uint8_t buf[DJLINK_MAX_PACKET];
    uint32_t t = 0;
    djlink_beat_t b;
    dj_link_beat_tracker_t tr;
    memset(&tr, 0, sizeof(tr));
    dj_link_session_t s = make_active(&t);
    dj_link_deck_report_t d = playing_deck(1000u, t);

    CHECK(dj_link_session_beat(&s, &d, NULL, t, buf, sizeof(buf)) == 0);
    CHECK(dj_link_session_beat(&s, &d, &tr, t, buf, sizeof(buf)) == 0);  /* arms */
    CHECK(tr.valid && tr.playhead_ms == 1000u);
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 200u, buf, sizeof(buf)) == 0);
    int n = dj_link_session_beat(&s, &d, &tr, t + 300u, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_BEAT_PACKET_LEN);
    CHECK(djlink_beat_parse(buf, (size_t)n, &b) == DJLINK_OK);
    CHECK(b.device_number == 4 && b.beat_in_bar == 3u && b.bpm100 == 12000u);
    CHECK(b.pitch_raw == 0x00100000);
    CHECK(b.next_beat_ms == 500u && b.second_beat_ms == 1000u);
    CHECK(b.fourth_beat_ms == 2000u && b.eighth_beat_ms == 4000u);
    CHECK(b.next_bar_ms == 1000u && b.second_bar_ms == 3000u);
    /* Once per beat. */
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 300u, buf, sizeof(buf)) == 0);
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 700u, buf, sizeof(buf)) == 0);
    /* A fresh report a little behind the estimate sends nothing twice. */
    d = playing_deck(1680u, t + 700u);
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 700u, buf, sizeof(buf)) == 0);
    n = dj_link_session_beat(&s, &d, &tr, t + 780u, buf, sizeof(buf));
    CHECK(n > 0 && djlink_beat_parse(buf, (size_t)n, &b) == DJLINK_OK);
    CHECK(b.beat_in_bar == 4u && b.next_bar_ms == 500u && b.second_bar_ms == 2500u);
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 790u, buf, sizeof(buf)) == 0);

    /* A seek re-arms without a beat, then counts from there. */
    d = playing_deck(20000u, t + 800u);
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 800u, buf, sizeof(buf)) == 0);
    n = dj_link_session_beat(&s, &d, &tr, t + 1100u, buf, sizeof(buf));
    CHECK(n > 0 && djlink_beat_parse(buf, (size_t)n, &b) == DJLINK_OK);
    CHECK(b.beat_in_bar == 1u && b.next_bar_ms == 2000u);  /* 20250 is beat 40 */

    /* Window end: distances past it are "beyond the end". */
    d = playing_deck(49700u, t + 1200u);
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 1200u, buf, sizeof(buf)) == 0); /* seek */
    n = dj_link_session_beat(&s, &d, &tr, t + 1260u, buf, sizeof(buf));
    CHECK(n > 0 && djlink_beat_parse(buf, (size_t)n, &b) == DJLINK_OK);
    CHECK(b.next_beat_ms == 0xffffffffu && b.next_bar_ms == 0xffffffffu);

    /* Paused or no number: nothing, and the tracker re-arms. */
    d.playing = false;
    CHECK(dj_link_session_beat(&s, &d, &tr, t + 1300u, buf, sizeof(buf)) == 0);
    CHECK(!tr.valid);
    dj_link_session_t idle;
    dj_link_session_reset(&idle);
    d.playing = true;
    CHECK(dj_link_session_beat(&idle, &d, &tr, t + 1300u, buf, sizeof(buf)) == 0);
    CHECK(!tr.valid);
}

/* v304: the beat clock network sync follows. */
static void test_beat_clock(void)
{
    dj_link_table_t t;
    uint8_t buf[DJLINK_MAX_PACKET];
    dj_link_beat_clock_t c;
    dj_link_table_reset(&t);

    dj_link_table_beat_clock(&t, 0, 1000, &c);
    CHECK(!c.valid);

    /* 128 BPM at +2%: 130.56 BPM, 459.6 ms beats. */
    size_t n = make_beat(buf, 1, "CDJ-3000", 12800, PITCH_ZERO + PITCH_ZERO / 50, 2, 469);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 1000) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_beat_clock(&t, 0, 1100, &c);
    CHECK(c.valid && c.player == 1 && c.beat_in_bar == 2 && c.anchor_ms == 1000);
    CHECK(c.period_us >= 459558u && c.period_us <= 459560u);

    /* The 0.1 BPM of an absolute position never replaces the beat tempo. */
    n = make_position(buf, 1, 60000, 1306);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 1050) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_beat_clock(&t, 0, 1100, &c);
    CHECK(c.period_us >= 459558u && c.period_us <= 459560u);

    /* Stale after 2.5 beats without a packet. */
    dj_link_table_beat_clock(&t, 0, 1000 + 1148, &c);
    CHECK(c.valid);
    dj_link_table_beat_clock(&t, 0, 1000 + 1150, &c);
    CHECK(!c.valid);

    /* Two unconfirmed players: the followed one sticks while it beats. */
    n = make_beat(buf, 1, "CDJ-3000", 12800, PITCH_ZERO, 3, 469);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 2000) == DJ_LINK_RX_ACCEPTED);
    n = make_beat(buf, 2, "CDJ-3000", 12000, PITCH_ZERO, 1, 500);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_BEAT, buf, n, 2100) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_beat_clock(&t, 0, 2150, &c);
    CHECK(c.valid && c.player == 2);                 /* newest */
    dj_link_table_beat_clock(&t, 1, 2150, &c);
    CHECK(c.valid && c.player == 1 && c.beat_in_bar == 3);

    /* A status-confirmed master wins over the followed player... */
    n = make_status(buf, 2, DJLINK_FLAG_MASTER | DJLINK_FLAG_PLAYING, 12000, PITCH_ZERO);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_STATUS, buf, n, 2120) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_beat_clock(&t, 1, 2150, &c);
    CHECK(c.valid && c.player == 2 && c.period_us == 500000u);

    /* ...unless it reports itself stopped. */
    n = make_status(buf, 2, DJLINK_FLAG_MASTER, 12000, PITCH_ZERO);
    CHECK(dj_link_table_ingest(&t, DJLINK_PORT_STATUS, buf, n, 2130) == DJ_LINK_RX_ACCEPTED);
    dj_link_table_beat_clock(&t, 1, 2150, &c);
    CHECK(c.valid && c.player == 1);
}

int main(void)
{
    test_drop_to_null();
    test_beat_clock();
    test_beat_master_inferred();
    test_status_master_wins();
    test_position();
    test_players_ip();
    test_keepalive_sources();
    test_capacity_and_expiry();
    test_format_states();
    test_session_join_alone();
    test_session_claim_packets();
    test_session_conflicts();
    test_session_numbers_exhausted();
    test_session_media_reply();
    test_session_load_track();
    test_session_pair_numbers();
    test_session_two_decks();
    test_session_status();
    test_report_beats();
    test_deck_playhead();
    test_session_position();
    test_session_beat();
    test_pick_target_deck();
    if (s_failures) {
        printf("%d dj_link_state check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_state tests passed\n");
    return 0;
}
