/* v305: host tests for the tempo master negotiation (dj_link_master) and the
 * Mh byte of our CDJ status. Peer status goes through the real codec and
 * peer table. */
#include <stdio.h>
#include <string.h>

#include "dj_link_master.h"
#include "dj_link_session.h"
#include "dj_link_state.h"
#include "djlink/status.h"
#include "djlink/sync.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

#define PEER_IP 0xc0a8000au   /* 192.168.0.10 */
#define PEER2_IP 0xc0a8000bu

/* Peer `dev` status: playing, master flag and Mh as given. */
static void peer_status(dj_link_table_t *t, uint8_t dev, uint32_t ip, bool master,
                        uint8_t mh, uint32_t now_ms)
{
    djlink_status_t st;
    uint8_t buf[DJLINK_STATUS_PACKET_LEN];
    memset(&st, 0, sizeof(st));
    djlink_name_from_str("CDJ-3000", st.name);
    st.device_number = dev;
    st.play_state = DJLINK_PLAY_PLAYING;
    st.flags = DJLINK_FLAG_PLAYING | (master ? DJLINK_FLAG_MASTER : 0u);
    st.pitch_raw = 0x100000;
    st.bpm100 = 12800;
    st.beat = 0xffffffffu;
    st.master_handoff = mh;
    int n = djlink_status_build(&st, 1u, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_STATUS_PACKET_LEN);
    CHECK(dj_link_table_ingest_from(t, DJLINK_PORT_STATUS, buf, (size_t)n, ip, now_ms) ==
          DJ_LINK_RX_ACCEPTED);
}

static void decks_init(dj_link_master_deck_t d[2])
{
    memset(d, 0, 2 * sizeof(*d));
    d[0].number = 1;
    d[1].number = 2;
    d[0].loaded = d[1].loaded = true;
}

static void test_no_master_asserts(void)
{
    dj_link_master_t m;
    dj_link_table_t t;
    dj_link_master_deck_t d[2];
    dj_link_master_out_t out;
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    decks_init(d);

    dj_link_master_update(&m, d, &t, 1000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE);
    CHECK(!dj_link_master_asserts(&m, 0) && !dj_link_master_asserts(&m, 1));

    d[1].want = true;
    dj_link_master_update(&m, d, &t, 1010, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.deck == 1);
    CHECK(!out.send_request);
    CHECK(dj_link_master_asserts(&m, 1) && !dj_link_master_asserts(&m, 0));
    CHECK(dj_link_master_handoff(&m, 1) == DJ_LINK_MH_NONE);

    /* SYNC MASTER moves to deck 1 (index 0): after the grace, it follows. */
    d[1].want = false;
    d[0].want = true;
    dj_link_master_update(&m, d, &t, 1100, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.deck == 1);   /* grace */
    dj_link_master_update(&m, d, &t, 1010 + DJ_LINK_MASTER_GRACE_MS, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.deck == 0);
    CHECK(out.cmd[0] == DJ_LINK_DECK_CMD_NONE && out.cmd[1] == DJ_LINK_DECK_CMD_NONE);

    /* Losing our number ends it. */
    d[0].number = 0;
    dj_link_master_update(&m, d, &t, 5000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE);
    CHECK(!dj_link_master_asserts(&m, 0));
}

static void test_request_granted(void)
{
    dj_link_master_t m;
    dj_link_table_t t;
    dj_link_master_deck_t d[2];
    dj_link_master_out_t out;
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    decks_init(d);
    peer_status(&t, 3, PEER_IP, true, 0xff, 1000);

    d[0].want = true;
    dj_link_master_update(&m, d, &t, 1000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_REQUESTING && m.peer == 3);
    CHECK(out.send_request && out.request_ip == PEER_IP && out.request_number == 1);
    CHECK(!dj_link_master_asserts(&m, 0));
    CHECK(dj_link_master_on_response(&m, 3));
    CHECK(!dj_link_master_on_response(&m, 4));

    /* Nothing new before the retry period. */
    dj_link_master_update(&m, d, &t, 1200, &out);
    CHECK(!out.send_request);

    /* The master names us in Mh: assert, while it still asserts too. */
    peer_status(&t, 3, PEER_IP, true, 1, 1300);
    dj_link_master_update(&m, d, &t, 1300, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.deck == 0 && m.peer == 3);
    CHECK(dj_link_master_asserts(&m, 0));
    dj_link_master_update(&m, d, &t, 1500, &out);   /* overlap: no drop */
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING);
    CHECK(out.cmd[0] == DJ_LINK_DECK_CMD_NONE);

    /* It stands down. */
    peer_status(&t, 3, PEER_IP, false, 0xff, 1700);
    dj_link_master_update(&m, d, &t, 1700, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING);
    CHECK(!dj_link_master_on_response(&m, 3));   /* not requesting any more */
}

static void test_request_refused(void)
{
    dj_link_master_t m;
    dj_link_table_t t;
    dj_link_master_deck_t d[2];
    dj_link_master_out_t out;
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    decks_init(d);
    peer_status(&t, 3, PEER_IP, true, 0xff, 1000);

    d[0].want = true;
    dj_link_master_update(&m, d, &t, 1000, &out);
    CHECK(out.send_request);
    dj_link_master_update(&m, d, &t, 1000 + DJ_LINK_MASTER_REQUEST_RETRY_MS, &out);
    CHECK(out.send_request && m.phase == DJ_LINK_MASTER_REQUESTING);   /* resent */
    peer_status(&t, 3, PEER_IP, true, 0xff, 2500);
    dj_link_master_update(&m, d, &t, 1000 + DJ_LINK_MASTER_REQUEST_TIMEOUT_MS, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE && m.backoff[0]);
    CHECK(!dj_link_master_asserts(&m, 0));

    /* No new request while SYNC MASTER stays set. */
    dj_link_master_update(&m, d, &t, 4000, &out);
    CHECK(!out.send_request && m.phase == DJ_LINK_MASTER_IDLE);
    /* Pressed again (dropped, then set): asks again. */
    d[0].want = false;
    dj_link_master_update(&m, d, &t, 4100, &out);
    CHECK(!m.backoff[0]);
    d[0].want = true;
    dj_link_master_update(&m, d, &t, 4200, &out);
    CHECK(out.send_request && m.phase == DJ_LINK_MASTER_REQUESTING);

    /* Master moved to another peer meanwhile: ask that one. */
    peer_status(&t, 3, PEER_IP, false, 0xff, 4300);
    peer_status(&t, 4, PEER2_IP, true, 0xff, 4300);
    dj_link_master_update(&m, d, &t, 4300, &out);
    CHECK(out.send_request && out.request_ip == PEER2_IP && m.peer == 4);

    /* The master vanished: nobody to ask, assert. */
    peer_status(&t, 4, PEER2_IP, false, 0xff, 4400);
    dj_link_master_update(&m, d, &t, 4400, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && dj_link_master_asserts(&m, 0));

    /* Request abandoned when SYNC MASTER drops. */
    dj_link_master_reset(&m);
    peer_status(&t, 4, PEER2_IP, true, 0xff, 5000);
    dj_link_master_update(&m, d, &t, 5000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_REQUESTING);
    d[0].want = false;
    dj_link_master_update(&m, d, &t, 5100, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE && !out.send_request);
}

static void test_rival_drops_us(void)
{
    dj_link_master_t m;
    dj_link_table_t t;
    dj_link_master_deck_t d[2];
    dj_link_master_out_t out;
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    decks_init(d);
    d[0].want = true;
    dj_link_master_update(&m, d, &t, 1000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING);

    peer_status(&t, 3, PEER_IP, true, 0xff, 2000);
    dj_link_master_update(&m, d, &t, 2000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE);
    CHECK(out.cmd[0] == DJ_LINK_DECK_CMD_MASTER_DROP);
    CHECK(!dj_link_master_asserts(&m, 0));
    CHECK(m.backoff[0]);
    /* No request storm while deck_core catches up. */
    dj_link_master_update(&m, d, &t, 2100, &out);
    CHECK(!out.send_request && out.cmd[0] == DJ_LINK_DECK_CMD_NONE);

    /* The outgoing master of a granted handoff is not a rival - until the
     * yield timeout. */
    dj_link_master_reset(&m);
    peer_status(&t, 3, PEER_IP, true, 1, 3000);
    dj_link_master_update(&m, d, &t, 3000, &out);   /* Mh already names us: unasked */
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.peer == 3);
    CHECK(out.cmd[0] == DJ_LINK_DECK_CMD_NONE);      /* SYNC MASTER already set */
    peer_status(&t, 3, PEER_IP, true, 1, 3000 + DJ_LINK_MASTER_YIELD_TIMEOUT_MS);
    dj_link_master_update(&m, d, &t, 3000 + DJ_LINK_MASTER_YIELD_TIMEOUT_MS, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE && out.cmd[0] == DJ_LINK_DECK_CMD_MASTER_DROP);
}

static void test_yield(void)
{
    dj_link_master_t m;
    dj_link_table_t t;
    dj_link_master_deck_t d[2];
    dj_link_master_out_t out;
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    decks_init(d);

    /* Not master: nothing to give. */
    CHECK(dj_link_master_on_request(&m, d, 3, 900) == 0);

    d[1].want = true;
    dj_link_master_update(&m, d, &t, 1000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.deck == 1);
    CHECK(dj_link_master_on_request(&m, d, 1, 1050) == 0);   /* our own deck */
    CHECK(dj_link_master_on_request(&m, d, 0, 1050) == 0);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING);

    peer_status(&t, 3, PEER_IP, false, 0xff, 1100);
    CHECK(dj_link_master_on_request(&m, d, 3, 1100) == 2);   /* answer from #2 */
    CHECK(m.phase == DJ_LINK_MASTER_YIELDING && m.peer == 3);
    CHECK(dj_link_master_asserts(&m, 1));                    /* still master */
    CHECK(dj_link_master_handoff(&m, 1) == 3);
    CHECK(dj_link_master_handoff(&m, 0) == DJ_LINK_MH_NONE);
    CHECK(dj_link_master_on_request(&m, d, 3, 1150) == 2);   /* repeated: again */
    CHECK(dj_link_master_on_request(&m, d, 4, 1150) == 0);   /* another: no */

    dj_link_master_update(&m, d, &t, 1200, &out);
    CHECK(m.phase == DJ_LINK_MASTER_YIELDING);

    /* The requester asserts: we stop and drop SYNC MASTER. */
    peer_status(&t, 3, PEER_IP, true, 0xff, 1400);
    dj_link_master_update(&m, d, &t, 1400, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE);
    CHECK(out.cmd[1] == DJ_LINK_DECK_CMD_MASTER_DROP);
    CHECK(!dj_link_master_asserts(&m, 1));
    CHECK(dj_link_master_handoff(&m, 1) == DJ_LINK_MH_NONE);
    /* deck_core not caught up yet: no request back to the new master. */
    dj_link_master_update(&m, d, &t, 1500, &out);
    CHECK(!out.send_request && m.phase == DJ_LINK_MASTER_IDLE);

    /* Requester never asserts: keep it. */
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    dj_link_master_update(&m, d, &t, 2000, &out);
    CHECK(dj_link_master_on_request(&m, d, 3, 2000) == 2);
    dj_link_master_update(&m, d, &t, 2000 + DJ_LINK_MASTER_YIELD_TIMEOUT_MS, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING);
    CHECK(dj_link_master_handoff(&m, 1) == DJ_LINK_MH_NONE);
    CHECK(out.cmd[1] == DJ_LINK_DECK_CMD_NONE);

    /* SYNC MASTER dropped locally while yielding: just stop. */
    CHECK(dj_link_master_on_request(&m, d, 3, 6000) == 2);
    d[1].want = false;
    dj_link_master_update(&m, d, &t, 6100, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE && out.cmd[1] == DJ_LINK_DECK_CMD_NONE);
}

static void test_unsolicited_handoff(void)
{
    dj_link_master_t m;
    dj_link_table_t t;
    dj_link_master_deck_t d[2];
    dj_link_master_out_t out;
    dj_link_master_reset(&m);
    dj_link_table_reset(&t);
    decks_init(d);

    /* Mh names an empty deck: ignored. */
    d[1].loaded = false;
    peer_status(&t, 3, PEER_IP, true, 2, 1000);
    dj_link_master_update(&m, d, &t, 1000, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE);

    d[1].loaded = true;
    dj_link_master_update(&m, d, &t, 1100, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING && m.deck == 1);
    CHECK(out.cmd[1] == DJ_LINK_DECK_CMD_MASTER_TAKE);
    CHECK(dj_link_master_asserts(&m, 1));

    /* deck_core never set SYNC MASTER: let go after the grace. */
    peer_status(&t, 3, PEER_IP, false, 0xff, 1200);
    dj_link_master_update(&m, d, &t, 1200, &out);
    CHECK(m.phase == DJ_LINK_MASTER_ASSERTING);
    dj_link_master_update(&m, d, &t, 1100 + DJ_LINK_MASTER_GRACE_MS, &out);
    CHECK(m.phase == DJ_LINK_MASTER_IDLE);
}

static void test_sync_target(void)
{
    dj_link_master_deck_t d[2];
    decks_init(d);
    CHECK(dj_link_sync_target_deck(NULL) == -1);
    CHECK(dj_link_sync_target_deck(d) == -1);         /* both loaded, both paused */
    d[1].playing = true;
    CHECK(dj_link_sync_target_deck(d) == 1);
    d[0].playing = true;
    CHECK(dj_link_sync_target_deck(d) == -1);         /* both playing */
    d[0].loaded = false;
    CHECK(dj_link_sync_target_deck(d) == 1);          /* only loaded one */
    d[1].loaded = false;
    CHECK(dj_link_sync_target_deck(d) == -1);         /* none loaded */
    d[1].number = 0;
    CHECK(dj_link_sync_target_deck(d) == 0);          /* deck 2 not joined */
    d[0].number = 0;
    CHECK(dj_link_sync_target_deck(d) == -1);

    CHECK(dj_link_sync_action_cmd(DJLINK_SYNC_ON) == DJ_LINK_DECK_CMD_SYNC_ON);
    CHECK(dj_link_sync_action_cmd(DJLINK_SYNC_OFF) == DJ_LINK_DECK_CMD_SYNC_OFF);
    CHECK(dj_link_sync_action_cmd(DJLINK_SYNC_MASTER) == DJ_LINK_DECK_CMD_MASTER_TAKE);
    CHECK(dj_link_sync_action_cmd(0x42) == DJ_LINK_DECK_CMD_NONE);

    /* The codec round trip of what a commander sends. */
    djlink_sync_t sc = { .sender_number = 3, .target_number = 3,
                         .action = DJLINK_SYNC_MASTER };
    djlink_name_from_str("rekordbox", sc.name);
    uint8_t buf[DJLINK_SYNC_PACKET_LEN];
    CHECK(djlink_sync_build(&sc, buf, sizeof(buf)) == (int)DJLINK_SYNC_PACKET_LEN);
    djlink_sync_t back;
    CHECK(djlink_sync_parse(buf, sizeof(buf), &back) == DJLINK_OK);
    CHECK(dj_link_sync_action_cmd(back.action) == DJ_LINK_DECK_CMD_MASTER_TAKE);
}

static void test_status_mh(void)
{
    dj_link_session_t s;
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    memset(&s, 0, sizeof(s));
    dj_link_session_start(&s, "PAJONIIIR", mac, 0xc0a80002u, 0);
    s.phase = DJ_LINK_CLAIM_ACTIVE;
    s.device_number = 2;

    dj_link_deck_report_t r;
    memset(&r, 0, sizeof(r));
    r.loaded = true;
    r.playing = true;
    r.bpm100 = 12800;
    uint8_t buf[DJLINK_STATUS_PACKET_LEN];
    int n = dj_link_session_status(&s, 2, &r, 1, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_STATUS_PACKET_LEN);
    CHECK(buf[0x9f] == 0xff);
    CHECK((buf[0x89] & DJLINK_FLAG_MASTER) == 0u && buf[0x9e] == 0u);

    r.master = true;
    r.master_handoff = 3;
    n = dj_link_session_status(&s, 2, &r, 2, buf, sizeof(buf));
    CHECK(n == (int)DJLINK_STATUS_PACKET_LEN);
    CHECK(buf[0x9f] == 3);
    CHECK((buf[0x89] & DJLINK_FLAG_MASTER) != 0u && buf[0x9e] == 1u);

    /* What a peer then sees. */
    dj_link_table_t t;
    dj_link_table_reset(&t);
    CHECK(dj_link_table_ingest_from(&t, DJLINK_PORT_STATUS, buf, (size_t)n, 0xc0a80002u, 10) ==
          DJ_LINK_RX_ACCEPTED);
    CHECK(t.peers[0].master && t.peers[0].master_handoff == 3);
}

int main(void)
{
    test_no_master_asserts();
    test_request_granted();
    test_request_refused();
    test_rival_drops_us();
    test_yield();
    test_unsolicited_handoff();
    test_sync_target();
    test_status_mh();
    if (s_failures) {
        printf("%d dj_link_master check(s) failed\n", s_failures);
        return 1;
    }
    printf("dj_link_master: all tests passed\n");
    return 0;
}
