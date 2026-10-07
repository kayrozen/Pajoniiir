/* v341: host tests for fader start and on-air from a mixer. */
#include <stdio.h>
#include <string.h>

#include "dj_link_mixer.h"
#include "djlink/mixer.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

static size_t fader_pkt(uint8_t *buf, uint8_t sender, uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4)
{
    djlink_fader_t f;
    memset(&f, 0, sizeof(f));
    djlink_name_from_str("DJM-900NXS2", f.name);
    f.mixer_number = sender;
    f.players[0] = (djlink_fader_cmd_t)c1;
    f.players[1] = (djlink_fader_cmd_t)c2;
    f.players[2] = (djlink_fader_cmd_t)c3;
    f.players[3] = (djlink_fader_cmd_t)c4;
    int n = djlink_fader_build(&f, buf, DJLINK_MAX_PACKET);
    buf[0x21] = sender;           /* the builder may stamp its own number */
    return (size_t)n;
}

static size_t onair_pkt(uint8_t *buf, uint8_t sender, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    djlink_onair_t o;
    memset(&o, 0, sizeof(o));
    djlink_name_from_str("DJM-900NXS2", o.name);
    o.mixer_number = sender;
    o.on_air[0] = a; o.on_air[1] = b; o.on_air[2] = c; o.on_air[3] = d;
    int n = djlink_onair_build(&o, buf, DJLINK_MAX_PACKET);
    buf[0x21] = sender;
    return (size_t)n;
}

static void test_fader_edges(void)
{
    dj_link_mixer_t m;
    uint8_t b[DJLINK_MAX_PACKET];
    dj_link_mixer_reset(&m);
    /* Nothing yet. */
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_NONE);

    /* Channel 3 starts. IGNORE on the others is the resting value. */
    size_t n = fader_pkt(b, 0x21, 2, 2, DJLINK_FADER_START, 2);
    CHECK(dj_link_mixer_ingest(&m, b, n, 100));
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_START);
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_NONE);       /* once */
    CHECK(dj_link_mixer_fader_take(&m, 1) == DJ_LINK_FADER_NONE);

    /* The same command repeated is not a new event. */
    CHECK(dj_link_mixer_ingest(&m, b, n, 130));
    CHECK(dj_link_mixer_ingest(&m, b, n, 160));
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_NONE);

    /* Stop, then start again. */
    n = fader_pkt(b, 0x21, 2, 2, DJLINK_FADER_STOP, 2);
    CHECK(dj_link_mixer_ingest(&m, b, n, 200));
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_STOP);
    n = fader_pkt(b, 0x21, 2, 2, DJLINK_FADER_START, 2);
    CHECK(dj_link_mixer_ingest(&m, b, n, 300));
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_START);

    /* An event not taken before the next change is replaced, not queued. */
    n = fader_pkt(b, 0x21, 2, 2, DJLINK_FADER_STOP, 2);
    dj_link_mixer_ingest(&m, b, n, 400);
    n = fader_pkt(b, 0x21, 2, 2, DJLINK_FADER_START, 2);
    dj_link_mixer_ingest(&m, b, n, 410);
    CHECK(dj_link_mixer_fader_take(&m, 3) == DJ_LINK_FADER_START);

    /* IGNORE after START cancels a pending START nobody took. */
    n = fader_pkt(b, 0x21, DJLINK_FADER_START, 2, 2, 2);
    dj_link_mixer_ingest(&m, b, n, 500);
    n = fader_pkt(b, 0x21, 2, 2, 2, 2);
    dj_link_mixer_ingest(&m, b, n, 510);
    CHECK(dj_link_mixer_fader_take(&m, 1) == DJ_LINK_FADER_NONE);
    /* Later the START comes again: it is a new edge. */
    n = fader_pkt(b, 0x21, DJLINK_FADER_START, 2, 2, 2);
    dj_link_mixer_ingest(&m, b, n, 520);
    CHECK(dj_link_mixer_fader_take(&m, 1) == DJ_LINK_FADER_START);

    /* Out-of-range players. */
    CHECK(dj_link_mixer_fader_take(&m, 0) == DJ_LINK_FADER_NONE);
    CHECK(dj_link_mixer_fader_take(&m, 5) == DJ_LINK_FADER_NONE);
    CHECK(dj_link_mixer_fader_take(NULL, 1) == DJ_LINK_FADER_NONE);
}

static void test_refusals(void)
{
    dj_link_mixer_t m;
    uint8_t b[DJLINK_MAX_PACKET];
    dj_link_mixer_reset(&m);

    /* A player (number 3) sending fader start: refused, used up. */
    size_t n = fader_pkt(b, 3, DJLINK_FADER_START, 2, 2, 2);
    CHECK(dj_link_mixer_ingest(&m, b, n, 100));
    CHECK(m.rejected == 1 && m.accepted == 0);
    CHECK(dj_link_mixer_fader_take(&m, 1) == DJ_LINK_FADER_NONE);
    n = onair_pkt(b, 2, 1, 1, 1, 1);
    CHECK(dj_link_mixer_ingest(&m, b, n, 100));
    CHECK(!dj_link_mixer_on_air(&m, 1, 100));

    /* A command byte that is not START / STOP / IGNORE is skipped, the others kept. */
    n = fader_pkt(b, 0x21, 0x07, DJLINK_FADER_START, 2, 2);
    CHECK(dj_link_mixer_ingest(&m, b, n, 200));
    CHECK(dj_link_mixer_fader_take(&m, 1) == DJ_LINK_FADER_NONE);
    CHECK(dj_link_mixer_fader_take(&m, 2) == DJ_LINK_FADER_START);

    /* Not ours: other types, short, bad magic. */
    n = fader_pkt(b, 0x21, 0, 2, 2, 2);
    CHECK(!dj_link_mixer_ingest(&m, b, 0x20, 300));
    b[0] ^= 0xff;
    CHECK(!dj_link_mixer_ingest(&m, b, n, 300));
    CHECK(!dj_link_mixer_ingest(&m, NULL, 0, 300));
    uint8_t beat[0x60];
    memset(beat, 0, sizeof(beat));
    memcpy(beat, DJLINK_MAGIC, DJLINK_MAGIC_LEN);
    beat[0x0a] = 0x28;
    CHECK(!dj_link_mixer_ingest(&m, beat, sizeof(beat), 300));
}

static void test_on_air(void)
{
    dj_link_mixer_t m;
    uint8_t b[DJLINK_MAX_PACKET];
    dj_link_mixer_reset(&m);
    CHECK(!dj_link_mixer_on_air(&m, 1, 0));                   /* never heard */

    size_t n = onair_pkt(b, 0x21, 1, 0, 1, 0);
    CHECK(dj_link_mixer_ingest(&m, b, n, 1000));
    CHECK(dj_link_mixer_on_air(&m, 1, 1000));
    CHECK(!dj_link_mixer_on_air(&m, 2, 1000));
    CHECK(dj_link_mixer_on_air(&m, 3, 1000));
    CHECK(!dj_link_mixer_on_air(&m, 4, 1000));

    /* Held for DJ_LINK_ON_AIR_HOLD_MS after the last packet, then off. */
    CHECK(dj_link_mixer_on_air(&m, 1, 1000 + DJ_LINK_ON_AIR_HOLD_MS - 1));
    CHECK(!dj_link_mixer_on_air(&m, 1, 1000 + DJ_LINK_ON_AIR_HOLD_MS));

    /* A new packet changes the picture. */
    n = onair_pkt(b, 0x21, 0, 1, 0, 0);
    CHECK(dj_link_mixer_ingest(&m, b, n, 2000));
    CHECK(!dj_link_mixer_on_air(&m, 1, 2000) && dj_link_mixer_on_air(&m, 2, 2000));

    /* The millisecond clock wraps. */
    n = onair_pkt(b, 0x21, 1, 0, 0, 0);
    CHECK(dj_link_mixer_ingest(&m, b, n, 0xfffffff0u));
    CHECK(dj_link_mixer_on_air(&m, 1, 0x00000100u));
    CHECK(!dj_link_mixer_on_air(&m, 1, 0xfffffff0u + DJ_LINK_ON_AIR_HOLD_MS + 5));

    /* Out of range. */
    CHECK(!dj_link_mixer_on_air(&m, 0, 2000) && !dj_link_mixer_on_air(&m, 5, 2000));
    CHECK(!dj_link_mixer_on_air(NULL, 1, 2000));
    dj_link_mixer_reset(&m);
    CHECK(!dj_link_mixer_on_air(&m, 1, 2000));
}

int main(void)
{
    test_fader_edges();
    test_refusals();
    test_on_air();
    if (s_failures) {
        printf("%d dj_link_mixer check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_mixer tests passed\n");
    return 0;
}
