/* Host tests for dj_link_db: the client runs against a minimal mock peer
 * over real localhost TCP. The mock answers the port discovery, the greeting,
 * the context setup, a 3-track all-tracks menu and one track's metadata,
 * with every reply built by the esp-djlink dbserver codec. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "djlink/dbserver.h"
#include "djlink/status.h"
#include "dj_link_db.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

#define PEER_IP      0xc0a80102u /* 192.168.1.2, only checked, we dial 127.0.0.1 */
#define PEER_NUMBER  2u
#define OUR_NUMBER   4u
#define SERVER_NUMBER 2u

/* ---------------------------------------------------------------- mock peer */

typedef enum {
    MOCK_NORMAL = 0,
    MOCK_SILENT_DISCOVERY, /* accepts the port query, never answers */
    MOCK_BAD_MAGIC,        /* answers the track menu with garbage */
} mock_mode_t;

typedef struct {
    mock_mode_t mode;
    int disc_fd;
    int db_fd;
    uint16_t disc_port;
    uint16_t db_port;
    /* What the mock saw, checked by the tests. */
    int port_queries;
    int setups;
    uint32_t setup_number;
    int track_menus;
    uint32_t track_menu_dmst;
    int renders;
    int metadata_requests;
    uint32_t metadata_id;
} mock_t;

static const struct {
    uint32_t id;
    const char *title;
    const char *artist;
    uint32_t seconds;
    uint32_t bpm100;
} k_tracks[3] = {
    {101u, "Strings of Life", "Rhythim Is Rhythim", 391u, 12400u},
    {102u, "Caf\xc3\xa9 del Mar", "Energy 52", 452u, 13350u},
    {103u, "Windowlicker", "Aphex Twin", 367u, 12800u},
};

static int listen_any(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
        perror("mock listen");
        exit(2);
    }
    getsockname(fd, (struct sockaddr *)&a, &alen);
    *port = ntohs(a.sin_port);
    return fd;
}

static int read_full(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static void write_full(int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) {
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

/* UTF-8 (ASCII + 2-byte sequences) to UTF-16BE with the trailing NUL the
 * players send. */
static size_t to_utf16be(const char *s, uint8_t *out)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = *p++;
        if ((cp & 0xe0u) == 0xc0u && *p) {
            cp = ((cp & 0x1fu) << 6) | (*p++ & 0x3fu);
        }
        out[o++] = (uint8_t)(cp >> 8);
        out[o++] = (uint8_t)cp;
    }
    out[o++] = 0;
    out[o++] = 0;
    return o;
}

static void send_msg(int fd, uint32_t txid, uint16_t type, const djlink_db_arg_t *args,
                     uint8_t argc)
{
    uint8_t buf[1024];
    int n = djlink_db_msg_build(txid, type, args, argc, buf, sizeof(buf));
    if (n > 0) {
        write_full(fd, buf, (size_t)n);
    }
}

static void send_success(int fd, uint32_t txid, uint32_t request, uint32_t count)
{
    djlink_db_arg_t a[2];
    memset(a, 0, sizeof(a));
    a[0].type = DJLINK_DB_FIELD_INT32;
    a[0].num = request;
    a[1].type = DJLINK_DB_FIELD_INT32;
    a[1].num = count;
    send_msg(fd, txid, DJLINK_DB_TYPE_SUCCESS, a, 2);
}

static void send_item(int fd, uint32_t txid, uint32_t num, const char *label1,
                      const char *label2, uint32_t item_type)
{
    static uint8_t l1[256];
    static uint8_t l2[256];
    djlink_db_arg_t a[12];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 12; i++) {
        a[i].type = DJLINK_DB_FIELD_INT32;
    }
    a[1].num = num;
    a[3].type = DJLINK_DB_FIELD_STRING;
    a[3].bin = l1;
    a[3].bin_len = to_utf16be(label1, l1);
    a[2].num = (uint32_t)a[3].bin_len;
    a[5].type = DJLINK_DB_FIELD_STRING;
    a[5].bin = l2;
    a[5].bin_len = to_utf16be(label2, l2);
    a[4].num = (uint32_t)a[5].bin_len;
    a[6].num = item_type;
    send_msg(fd, txid, DJLINK_DB_TYPE_MENU_ITEM, a, 12);
}

/* Read one request: bytes until the codec can parse a whole message. */
static int read_msg(int fd, uint8_t *buf, size_t cap, djlink_db_msg_t *m)
{
    size_t len = 0;
    for (;;) {
        int n = dj_link_db_msg_size(buf, len);
        if (n > 0) {
            return djlink_db_msg_parse(buf, (size_t)n, m) == DJLINK_OK ? 0 : -1;
        }
        if (n < 0 || len == cap) {
            return -1;
        }
        ssize_t r = read(fd, buf + len, 1);
        if (r <= 0) {
            return -1;
        }
        len++;
    }
}

static void *mock_thread(void *arg)
{
    mock_t *mk = arg;
    uint8_t buf[512];
    uint8_t expect[DJLINK_DB_PORT_QUERY_LEN];

    int fd = accept(mk->disc_fd, NULL, NULL);
    if (fd < 0 || read_full(fd, buf, DJLINK_DB_PORT_QUERY_LEN) != 0) {
        return NULL;
    }
    djlink_db_port_query_build(expect, sizeof(expect));
    if (memcmp(buf, expect, sizeof(expect)) == 0) {
        mk->port_queries++;
    }
    if (mk->mode == MOCK_SILENT_DISCOVERY) {
        if (read(fd, buf, 1) < 0) { /* hold until the client gives up and closes */
        }
        close(fd);
        return NULL;
    }
    uint8_t port[2] = {(uint8_t)(mk->db_port >> 8), (uint8_t)mk->db_port};
    write_full(fd, port, 2);
    close(fd);

    fd = accept(mk->db_fd, NULL, NULL);
    if (fd < 0 || read_full(fd, buf, DJLINK_DB_SETUP_LEN) != 0) {
        return NULL;
    }
    write_full(fd, buf, DJLINK_DB_SETUP_LEN); /* greeting echo */

    uint32_t last_request = 0;
    uint32_t last_id = 0;
    djlink_db_msg_t m;
    while (read_msg(fd, buf, sizeof(buf), &m) == 0) {
        if (m.type == DJLINK_DB_TYPE_SETUP) {
            mk->setups++;
            mk->setup_number = m.arg_count ? m.args[0].num : 0;
            send_success(fd, m.txid, 0, SERVER_NUMBER);
        } else if (m.type == DJ_LINK_DB_TYPE_TRACK_MENU) {
            mk->track_menus++;
            mk->track_menu_dmst = m.args[0].num;
            last_request = m.type;
            if (mk->mode == MOCK_BAD_MAGIC) {
                static const uint8_t junk[20] = {0x11, 0xde, 0xad, 0xbe, 0xef};
                write_full(fd, junk, sizeof(junk));
                continue;
            }
            send_success(fd, m.txid, m.type, 3);
        } else if (m.type == DJLINK_DB_TYPE_METADATA_REQUEST) {
            mk->metadata_requests++;
            mk->metadata_id = m.args[1].num;
            last_request = m.type;
            last_id = m.args[1].num;
            send_success(fd, m.txid, m.type, 4);
        } else if (m.type == DJLINK_DB_TYPE_RENDER) {
            uint32_t offset = m.args[1].num;
            uint32_t limit = m.args[2].num;
            mk->renders++;
            send_msg(fd, m.txid, DJLINK_DB_TYPE_MENU_HEADER, NULL, 0);
            if (last_request == DJ_LINK_DB_TYPE_TRACK_MENU) {
                for (uint32_t i = offset; i < 3u && i < offset + limit; i++) {
                    send_item(fd, m.txid, k_tracks[i].id, k_tracks[i].title,
                              k_tracks[i].artist, DJ_LINK_DB_ITEM_TITLE_ARTIST);
                }
            } else {
                for (int i = 0; i < 3; i++) {
                    if (k_tracks[i].id != last_id) {
                        continue;
                    }
                    send_item(fd, m.txid, 0, k_tracks[i].title, "", DJ_LINK_DB_ITEM_TITLE);
                    send_item(fd, m.txid, 0, k_tracks[i].artist, "", DJ_LINK_DB_ITEM_ARTIST);
                    send_item(fd, m.txid, k_tracks[i].seconds, "", "",
                              DJ_LINK_DB_ITEM_DURATION);
                    send_item(fd, m.txid, k_tracks[i].bpm100, "", "", DJ_LINK_DB_ITEM_TEMPO);
                }
            }
            send_msg(fd, m.txid, DJLINK_DB_TYPE_MENU_FOOTER, NULL, 0);
        }
    }
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------ client driver */

#define MAX_ROWS 8u

typedef struct {
    mock_t *mock;
    int fd;
    bool connect_done;
    int connects;
    uint32_t connect_ip;
    uint32_t total;
    uint32_t listed;
    dj_link_peer_track_t rows[MAX_ROWS];
    uint32_t want_detail;      /* index wanted, UINT32_MAX = none */
    int details;
} driver_t;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int io_connect(void *ctx, uint32_t ip, uint16_t port)
{
    driver_t *d = ctx;
    struct sockaddr_in a;
    d->connects++;
    d->connect_ip = ip;
    d->fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(d->fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(d->fd);
        d->fd = -1;
        return -1;
    }
    d->connect_done = true; /* reported from the loop, like the lwIP callback */
    return 0;
}

static int io_send(void *ctx, const uint8_t *buf, size_t len)
{
    driver_t *d = ctx;
    write_full(d->fd, buf, len);
    return 0;
}

static void io_close(void *ctx)
{
    driver_t *d = ctx;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    d->connect_done = false;
}

static void io_list_begin(void *ctx, uint32_t total)
{
    ((driver_t *)ctx)->total = total;
}

static void io_track(void *ctx, uint32_t index, const dj_link_peer_track_t *t, bool detail)
{
    driver_t *d = ctx;
    if (index >= MAX_ROWS) {
        return;
    }
    if (detail) {
        d->rows[index].duration_s = t->duration_s;
        d->rows[index].bpm100 = t->bpm100;
        if (t->artist[0]) {
            snprintf(d->rows[index].artist, sizeof(d->rows[index].artist), "%s", t->artist);
        }
        d->rows[index].has_detail = true;
        d->details++;
    } else {
        d->rows[index] = *t;
        d->listed++;
    }
}

static bool io_next_detail(void *ctx, uint32_t *index, uint32_t *id)
{
    driver_t *d = ctx;
    if (d->want_detail >= d->listed || d->rows[d->want_detail].has_detail) {
        return false;
    }
    *index = d->want_detail;
    *id = d->rows[d->want_detail].rekordbox_id;
    return true;
}

static dj_link_db_t s_client;

/* Pump events until `done` holds or `ms` elapse. */
static void run(driver_t *d, bool (*done)(void), uint32_t ms)
{
    uint32_t start = now_ms();
    while ((uint32_t)(now_ms() - start) < ms && !done()) {
        if (d->connect_done) {
            d->connect_done = false;
            dj_link_db_on_connected(&s_client, now_ms());
        }
        if (d->fd >= 0) {
            struct pollfd p = {.fd = d->fd, .events = POLLIN};
            if (poll(&p, 1, 10) > 0) {
                uint8_t buf[300];
                ssize_t n = read(d->fd, buf, sizeof(buf));
                if (n > 0) {
                    dj_link_db_on_data(&s_client, buf, (size_t)n, now_ms());
                } else {
                    close(d->fd);
                    d->fd = -1;
                    dj_link_db_on_closed(&s_client, now_ms());
                }
            }
        } else {
            usleep(10000);
        }
        dj_link_db_poll(&s_client, now_ms());
    }
}

static bool listed_or_failed(void)
{
    return (dj_link_db_list_done(&s_client) && s_client.phase == DJ_LINK_DB_READY) ||
           s_client.phase == DJ_LINK_DB_FAILED;
}

static driver_t *s_driver;
static bool detail_or_failed(void)
{
    return s_driver->details > 0 || s_client.phase == DJ_LINK_DB_FAILED;
}

static bool failed(void)
{
    return s_client.phase == DJ_LINK_DB_FAILED;
}

static void mock_start(mock_t *mk, pthread_t *th, mock_mode_t mode)
{
    memset(mk, 0, sizeof(*mk));
    mk->mode = mode;
    mk->disc_fd = listen_any(&mk->disc_port);
    mk->db_fd = listen_any(&mk->db_port);
    pthread_create(th, NULL, mock_thread, mk);
}

static void mock_stop(mock_t *mk, pthread_t th)
{
    shutdown(mk->disc_fd, SHUT_RDWR);
    shutdown(mk->db_fd, SHUT_RDWR);
    pthread_join(th, NULL);
    close(mk->disc_fd);
    close(mk->db_fd);
}

static void client_start(driver_t *d, mock_t *mk, uint32_t step_timeout_ms)
{
    memset(d, 0, sizeof(*d));
    d->mock = mk;
    d->fd = -1;
    d->want_detail = UINT32_MAX;
    s_driver = d;
    dj_link_db_io_t io = {
        .connect = io_connect, .send = io_send, .close = io_close,
        .list_begin = io_list_begin, .track = io_track, .next_detail = io_next_detail,
        .ctx = d,
    };
    dj_link_db_init(&s_client, &io, MAX_ROWS);
    s_client.discovery_port = mk->disc_port;
    s_client.step_timeout_ms = step_timeout_ms;
    dj_link_db_start(&s_client, PEER_IP, PEER_NUMBER, DJLINK_SLOT_USB, OUR_NUMBER, now_ms());
}

static void test_browse_three_tracks(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start(&d, &mk, 2000);

    run(&d, listed_or_failed, 3000);
    CHECK(s_client.phase == DJ_LINK_DB_READY);
    CHECK(dj_link_db_list_done(&s_client));
    /* discovery */
    CHECK(mk.port_queries == 1);
    CHECK(d.connect_ip == PEER_IP);
    CHECK(d.connects == 2); /* 12523-equivalent, then the announced port */
    /* setup with our player number */
    CHECK(mk.setups == 1);
    CHECK(mk.setup_number == OUR_NUMBER);
    /* all-tracks menu on the peer's USB, asked as D=4, main menu, rekordbox */
    CHECK(mk.track_menus == 1);
    CHECK(mk.track_menu_dmst == ((OUR_NUMBER << 24) | (1u << 16) | (DJLINK_SLOT_USB << 8) | 1u));
    CHECK(mk.renders == 1);
    CHECK(d.total == 3u);
    CHECK(d.listed == 3u);
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(d.rows[i].rekordbox_id == k_tracks[i].id);
        CHECK(strcmp(d.rows[i].title, k_tracks[i].title) == 0);
        CHECK(strcmp(d.rows[i].artist, k_tracks[i].artist) == 0);
        CHECK(d.rows[i].duration_s == 0u); /* list carries no duration */
        CHECK(d.rows[i].audio == DJ_LINK_PEER_AUDIO_METADATA_ONLY);
    }
    CHECK(strcmp(d.rows[1].title, "Caf\xc3\xa9 del Mar") == 0); /* UTF-16 -> UTF-8 */

    /* Nothing asked: no metadata request goes out on its own. */
    run(&d, detail_or_failed, 150);
    CHECK(mk.metadata_requests == 0);

    /* Metadata on demand for row 1 over the same session. */
    d.want_detail = 1;
    run(&d, detail_or_failed, 3000);
    CHECK(mk.metadata_requests == 1);
    CHECK(mk.metadata_id == 102u);
    CHECK(d.rows[1].has_detail);
    CHECK(d.rows[1].duration_s == 452u);
    CHECK(d.rows[1].bpm100 == 13350u);
    CHECK(d.connects == 2); /* no reconnect */
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    dj_link_db_stop(&s_client);
    CHECK(d.fd < 0);
    mock_stop(&mk, th);
}

static void test_load_refused_metadata_only(void)
{
    dj_link_peer_track_t t;
    char reason[96];
    memset(&t, 0, sizeof(t));
    t.rekordbox_id = 101u;
    t.audio = DJ_LINK_PEER_AUDIO_METADATA_ONLY;
    CHECK(dj_link_peer_load_check(&t, reason, sizeof(reason)) ==
          DJ_LINK_PEER_LOAD_METADATA_ONLY);
    CHECK(strncmp(reason, "metadata only", 13) == 0);
    t.has_detail = true; /* metadata does not make the audio reachable */
    CHECK(dj_link_peer_load_check(&t, NULL, 0) == DJ_LINK_PEER_LOAD_METADATA_ONLY);
    t.rekordbox_id = 0;
    CHECK(dj_link_peer_load_check(&t, reason, sizeof(reason)) == DJ_LINK_PEER_LOAD_NO_TRACK);
    CHECK(dj_link_peer_load_check(NULL, reason, sizeof(reason)) == DJ_LINK_PEER_LOAD_NO_TRACK);

    /* v249: NFS-reachable tracks load, with or without their detail. */
    t.rekordbox_id = 101u;
    t.has_detail = false;
    t.audio = DJ_LINK_PEER_AUDIO_NFS;
    CHECK(dj_link_peer_load_check(&t, reason, sizeof(reason)) == DJ_LINK_PEER_LOAD_OK);
    CHECK(strstr(reason, "NFS") != NULL);
    t.rekordbox_id = 0;
    CHECK(dj_link_peer_load_check(&t, reason, sizeof(reason)) == DJ_LINK_PEER_LOAD_NO_TRACK);
}

/* The owner's reachability flag is stamped on every listed row. */
static void test_browse_stamps_nfs(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start(&d, &mk, 2000);
    s_client.audio = DJ_LINK_PEER_AUDIO_NFS; /* before any row arrives */
    run(&d, listed_or_failed, 3000);
    CHECK(d.listed == 3u);
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(d.rows[i].audio == DJ_LINK_PEER_AUDIO_NFS);
        CHECK(dj_link_peer_load_check(&d.rows[i], NULL, 0) == DJ_LINK_PEER_LOAD_OK);
    }
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);
}

static void test_peer_track_key(void)
{
    uint32_t k = dj_link_peer_track_key(0xC0A80102u, 2, 101u);
    CHECK(k != 0u);
    CHECK(k == dj_link_peer_track_key(0xC0A80102u, 2, 101u));
    CHECK(k != dj_link_peer_track_key(0xC0A80103u, 2, 101u));
    CHECK(k != dj_link_peer_track_key(0xC0A80102u, 3, 101u));
    CHECK(k != dj_link_peer_track_key(0xC0A80102u, 2, 102u));
    /* FNV-1a over the 9 big-endian bytes 00 00 00 00 00 00 00 00 00. */
    CHECK(dj_link_peer_track_key(0, 0, 0) == 0xc8e581ffu);
}

static void test_discovery_timeout(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_SILENT_DISCOVERY);
    client_start(&d, &mk, 200);
    run(&d, failed, 2000);
    CHECK(s_client.phase == DJ_LINK_DB_FAILED);
    CHECK(strcmp(dj_link_db_error(&s_client), "TIMEOUT PORT") == 0);
    CHECK(d.fd < 0); /* closed on failure */
    CHECK(d.listed == 0u);
    mock_stop(&mk, th);
}

static void test_bad_reply(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_BAD_MAGIC);
    client_start(&d, &mk, 2000);
    run(&d, failed, 3000);
    CHECK(s_client.phase == DJ_LINK_DB_FAILED);
    CHECK(strcmp(dj_link_db_error(&s_client), "BAD REPLY") == 0);
    CHECK(d.fd < 0);
    mock_stop(&mk, th);
}

static void test_refuse_without_number(void)
{
    driver_t d;
    mock_t mk;
    memset(&mk, 0, sizeof(mk));
    memset(&d, 0, sizeof(d));
    d.fd = -1;
    dj_link_db_io_t io = {.connect = io_connect, .send = io_send, .close = io_close, .ctx = &d};
    dj_link_db_init(&s_client, &io, MAX_ROWS);
    dj_link_db_start(&s_client, PEER_IP, PEER_NUMBER, DJLINK_SLOT_USB, 0, 0);
    CHECK(s_client.phase == DJ_LINK_DB_FAILED);
    CHECK(strcmp(dj_link_db_error(&s_client), "NOT JOINED") == 0);
    CHECK(d.connects == 0);
    dj_link_db_start(&s_client, PEER_IP, OUR_NUMBER, DJLINK_SLOT_USB, OUR_NUMBER, 0);
    CHECK(strcmp(dj_link_db_error(&s_client), "BAD PEER") == 0);
    CHECK(d.connects == 0);
}

static void test_msg_size_and_utf16(void)
{
    uint8_t buf[256];
    char out[16];
    int n = djlink_db_context_setup_build(4, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(dj_link_db_msg_size(buf, (size_t)n) == n);
    CHECK(dj_link_db_msg_size(buf, (size_t)n - 1u) == 0);  /* need more */
    CHECK(dj_link_db_msg_size(buf, 3) == 0);
    CHECK(dj_link_db_msg_size(buf, 0) == 0);                /* nothing yet */
    buf[1] ^= 0xffu;
    CHECK(dj_link_db_msg_size(buf, (size_t)n) == -1);      /* bad magic */

    static const uint8_t pair[] = {0x00, 'A', 0xd8, 0x3d, 0xde, 0x00, 0x00, 'B', 0, 0, 0, 'C'};
    dj_link_db_utf16be_to_utf8(pair, sizeof(pair), out, sizeof(out));
    CHECK(strcmp(out, "A?B") == 0);                          /* stops at NUL */
    static const uint8_t e_acute[] = {0x00, 0xe9, 0x20, 0xac};
    dj_link_db_utf16be_to_utf8(e_acute, sizeof(e_acute), out, sizeof(out));
    CHECK(strcmp(out, "\xc3\xa9\xe2\x82\xac") == 0);
    dj_link_db_utf16be_to_utf8(e_acute, sizeof(e_acute), out, 3);
    CHECK(strcmp(out, "\xc3\xa9") == 0);                    /* never splits a char */
}

int main(void)
{
    test_msg_size_and_utf16();
    test_refuse_without_number();
    test_browse_three_tracks();
    test_load_refused_metadata_only();
    test_browse_stamps_nfs();
    test_peer_track_key();
    test_discovery_timeout();
    test_bad_reply();
    if (s_failures) {
        fprintf(stderr, "%d dj_link_db check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_db tests passed\n");
    return 0;
}
