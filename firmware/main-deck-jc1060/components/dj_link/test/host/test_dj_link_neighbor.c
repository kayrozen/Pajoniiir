/* v338: host tests for the neighbour cache (what the other players loaded). */
#include <stdio.h>
#include <string.h>

#include "dj_link_neighbor.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

static dj_link_peer_t *add_peer(dj_link_table_t *t, uint8_t n, uint8_t dev, uint8_t slot,
                                uint32_t id)
{
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        if (!t->peers[i].in_use) {
            dj_link_peer_t *p = &t->peers[i];
            memset(p, 0, sizeof(*p));
            p->in_use = true;
            p->device_number = n;
            p->has_status = true;
            p->source_device = dev;
            p->source_slot = slot;
            p->rekordbox_id = id;
            return p;
        }
    }
    return NULL;
}

static dj_link_peer_track_t meta(const char *title, const char *artist)
{
    dj_link_peer_track_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.title, sizeof(m.title), "%s", title);
    snprintf(m.artist, sizeof(m.artist), "%s", artist);
    m.duration_s = 245;
    m.bpm100 = 12800;
    return m;
}

/* A PWV3 detail: one byte per entry, height 0..31 in the low 5 bits. */
static void fill_detail(uint8_t *d, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        d[i] = (uint8_t)(i % 32u);
    }
}

static void test_follow_and_fetch(void)
{
    dj_link_table_t t;
    memset(&t, 0, sizeof(t));
    dj_link_nb_cache_t c;
    dj_link_nb_reset(&c);
    add_peer(&t, 2, 2, 3, 4711);        /* player 2, its own USB */
    add_peer(&t, 1, 0, 0, 0);           /* no track: not followed */
    CHECK(dj_link_nb_sync(&c, &t));
    CHECK(!dj_link_nb_sync(&c, &t));    /* nothing new */
    CHECK(dj_link_nb_find(&c, 2) != NULL);
    CHECK(dj_link_nb_find(&c, 1) == NULL);

    dj_link_nb_req_t r;
    CHECK(dj_link_nb_next(&c, 0, &r));
    CHECK(r.number == 2 && r.device == 2 && r.slot == 3 && r.rekordbox_id == 4711 && !r.want_wave);

    dj_link_peer_track_t m = meta("Strobe", "deadmau5");
    dj_link_nb_on_meta(&c, &r, &m);
    const dj_link_nb_slot_t *s = dj_link_nb_find(&c, 2);
    CHECK(s->has_meta && strcmp(s->title, "Strobe") == 0 && strcmp(s->artist, "deadmau5") == 0);
    CHECK(s->duration_s == 245 && s->bpm100 == 12800);

    CHECK(dj_link_nb_next(&c, 0, &r) && r.want_wave);
    uint8_t detail[1500];
    fill_detail(detail, sizeof(detail));
    dj_link_nb_on_wave(&c, &r, detail, sizeof(detail));
    s = dj_link_nb_find(&c, 2);
    CHECK(s->has_wave && s->state == DJ_LINK_NB_DONE);
    CHECK(!dj_link_nb_next(&c, 0, &r));
}

static void test_track_change_restarts(void)
{
    dj_link_table_t t;
    memset(&t, 0, sizeof(t));
    dj_link_nb_cache_t c;
    dj_link_nb_reset(&c);
    dj_link_peer_t *p = add_peer(&t, 3, 3, 3, 10);
    dj_link_nb_sync(&c, &t);
    dj_link_nb_req_t r;
    dj_link_nb_next(&c, 0, &r);
    dj_link_peer_track_t m = meta("Old", "A");
    dj_link_nb_on_meta(&c, &r, &m);
    const uint32_t gen = dj_link_nb_find(&c, 3)->gen;

    p->rekordbox_id = 11;               /* the player loaded another track */
    CHECK(dj_link_nb_sync(&c, &t));
    const dj_link_nb_slot_t *s = dj_link_nb_find(&c, 3);
    CHECK(!s->has_meta && s->title[0] == '\0' && s->gen != gen);
    CHECK(s->state == DJ_LINK_NB_WANT_META);

    /* The old answer arrives late: ignored. */
    dj_link_nb_on_wave(&c, &r, (const uint8_t *)"x", 1);
    m = meta("Stale", "A");
    dj_link_nb_on_meta(&c, &r, &m);
    CHECK(!dj_link_nb_find(&c, 3)->has_meta);

    p->in_use = false;                  /* the player left */
    CHECK(dj_link_nb_sync(&c, &t));
    CHECK(dj_link_nb_find(&c, 3) == NULL);
}

static void test_retry_and_give_up(void)
{
    dj_link_table_t t;
    memset(&t, 0, sizeof(t));
    dj_link_nb_cache_t c;
    dj_link_nb_reset(&c);
    add_peer(&t, 1, 1, 3, 5);
    dj_link_nb_sync(&c, &t);
    dj_link_nb_req_t r;
    CHECK(dj_link_nb_next(&c, 1000, &r));
    dj_link_nb_on_fail(&c, &r, 1000);
    CHECK(!dj_link_nb_next(&c, 1000 + DJ_LINK_NEIGHBOR_RETRY_MS - 1, &r));   /* waits */
    CHECK(dj_link_nb_next(&c, 1000 + DJ_LINK_NEIGHBOR_RETRY_MS, &r));
    dj_link_nb_on_meta(&c, &r, NULL);                                        /* 2nd failure */
    dj_link_nb_on_fail(&c, &r, 20000);                                       /* 3rd */
    CHECK(dj_link_nb_find(&c, 1)->state == DJ_LINK_NB_GAVE_UP);
    CHECK(!dj_link_nb_next(&c, 99999, &r));
    CHECK(!dj_link_nb_sync(&c, &t));                                         /* same track: stays */
    CHECK(dj_link_nb_find(&c, 1)->state == DJ_LINK_NB_GAVE_UP);
}

static void test_wave_failure_keeps_text(void)
{
    dj_link_table_t t;
    memset(&t, 0, sizeof(t));
    dj_link_nb_cache_t c;
    dj_link_nb_reset(&c);
    add_peer(&t, 4, 4, 2, 77);
    dj_link_nb_sync(&c, &t);
    dj_link_nb_req_t r;
    dj_link_nb_next(&c, 0, &r);
    dj_link_peer_track_t m = meta("Title", "Art");
    dj_link_nb_on_meta(&c, &r, &m);
    dj_link_nb_next(&c, 0, &r);
    dj_link_nb_on_wave(&c, &r, NULL, 0);        /* the source has no waveform */
    const dj_link_nb_slot_t *s = dj_link_nb_find(&c, 4);
    CHECK(s->state == DJ_LINK_NB_DONE && s->has_meta && !s->has_wave);
    CHECK(!dj_link_nb_next(&c, 0, &r));
}

static void test_wanted_sources_and_capacity(void)
{
    dj_link_table_t t;
    memset(&t, 0, sizeof(t));
    dj_link_nb_cache_t c;
    dj_link_nb_reset(&c);
    add_peer(&t, 1, 1, 1, 9);           /* CD: nothing to ask */
    add_peer(&t, 2, 2, 4, 9);           /* rekordbox laptop: yes */
    add_peer(&t, 3, 3, 2, 9);           /* SD: yes */
    add_peer(&t, 4, 4, 3, 9);
    add_peer(&t, 5, 5, 3, 9);
    add_peer(&t, 6, 6, 3, 9);
    dj_link_nb_sync(&c, &t);
    CHECK(dj_link_nb_find(&c, 1) == NULL);
    CHECK(dj_link_nb_find(&c, 2) && dj_link_nb_find(&c, 3) && dj_link_nb_find(&c, 4) &&
          dj_link_nb_find(&c, 5));
    CHECK(dj_link_nb_find(&c, 6) == NULL);      /* only DJ_LINK_NEIGHBOR_MAX, lowest first */
    dj_link_nb_req_t r;
    CHECK(dj_link_nb_next(&c, 0, &r) && r.number == 2);   /* lowest player first */
}

static void test_position(void)
{
    dj_link_peer_t p;
    memset(&p, 0, sizeof(p));
    uint32_t ms = 0;
    bool est = true;
    CHECK(!dj_link_nb_position_ms(&p, 1000, &ms, &est) && !est);

    p.beat_count = 65;                  /* 64 beats at 120 BPM = 32 s */
    p.bpm = 120.0f;
    CHECK(dj_link_nb_position_ms(&p, 1000, &ms, &est) && est && ms == 32000u);

    p.has_position = true;
    p.playhead_ms = 90000;
    p.position_rx_ms = 5000;
    p.playing = true;
    CHECK(dj_link_nb_position_ms(&p, 5200, &ms, &est) && !est && ms == 90200u);
    p.playing = false;
    CHECK(dj_link_nb_position_ms(&p, 5200, &ms, &est) && ms == 90000u);
}

int main(void)
{
    test_follow_and_fetch();
    test_track_change_restarts();
    test_retry_and_give_up();
    test_wave_failure_keeps_text();
    test_wanted_sources_and_capacity();
    test_position();
    if (s_failures) {
        printf("%d dj_link_neighbor check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_neighbor tests passed\n");
    return 0;
}
