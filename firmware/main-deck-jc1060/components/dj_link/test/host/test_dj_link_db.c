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
#include "dj_link_anlz.h"

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
    /* v307: vynull's replies (dbserver/menuitem.go, track.go): list items
     * 0x0d04 "title / BPM - key" (its default menu detail), metadata as
     * its 16 items, artist second, original artist and remixer last. */
    MOCK_VYNULL,
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
    uint32_t track_menu_sort;   /* v310: arg 1 of the last 0x1004 */
    int      playlist_menus;    /* v311: 0x1105 requests */
    uint32_t playlist_argc, playlist_sort, playlist_id, playlist_folder;
    int renders;
    int metadata_requests;
    uint32_t metadata_id;
    /* v297: track info 0x2102 (file path). seq orders requests. */
    int seq;
    int metadata_seq;
    int track_info_requests;
    int track_info_seq;
    uint32_t track_info_id;
    uint32_t track_info_dmst;
    /* v300: analysis blobs. */
    int wave_requests;
    uint8_t wave_argc;
    uint32_t wave_dmst;
    int grid_requests;
    uint8_t grid_argc;
    uint32_t grid_id;
    int art_requests;
    uint8_t art_argc;
    int tag_requests;           /* v314: 0x2c04 */
    uint32_t tag_argc, tag_id, tag_fourcc, tag_file;
    uint32_t art_id;
    /* v303: nxs2 cue list. */
    int cue_requests;
    uint8_t cue_argc;
    uint32_t cue_id;
} mock_t;

/* v300: track 101 has 60 s of PWV3 detail (larger than the client's rx
 * buffer) and a 120-beat grid; any other id has no analysis. */
#define MOCK_WAVE_LEN   9000u
#define MOCK_GRID_BEATS 120u
/* v300: artwork 77 is a 3000-byte JPEG, 78 one too big for the client's
 * buffer; any other id is vynull's "not found". */
#define MOCK_ART_ID     77u
#define MOCK_ART_BIG_ID 78u
#define MOCK_ART_LEN    3000u
#define MOCK_ART_BIG    5000u

static uint8_t mock_wave_byte(size_t i)
{
    return (uint8_t)((i * 7u) & 0xffu);
}

static const struct {
    uint32_t id;
    const char *title;
    const char *artist;
    uint32_t seconds;
    uint32_t bpm100;
    const char *path;   /* v297: 0x2102 answer, from the NFS export root */
    uint32_t artwork;   /* v300: title item argument 8 */
} k_tracks[3] = {
    {101u, "Strings of Life", "Rhythim Is Rhythim", 391u, 12400u,
     "/Music/Rhythim Is Rhythim/Strings of Life.mp3", MOCK_ART_ID},
    {102u, "Caf\xc3\xa9 del Mar", "Energy 52", 452u, 13350u,
     "/Music/Energy 52/Caf\xc3\xa9 del Mar.flac", 0u},
    {103u, "Windowlicker", "Aphex Twin", 367u, 12800u, "/Music/Aphex Twin/Windowlicker.mp3",
     MOCK_ART_BIG_ID},
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

static void send_item_art(int fd, uint32_t txid, uint32_t num, const char *label1,
                          const char *label2, uint32_t item_type, uint32_t artwork)
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
    a[8].num = artwork;
    send_msg(fd, txid, DJLINK_DB_TYPE_MENU_ITEM, a, 12);
}

static void send_item(int fd, uint32_t txid, uint32_t num, const char *label1,
                      const char *label2, uint32_t item_type)
{
    send_item_art(fd, txid, num, label1, label2, item_type, 0u);
}

/* v300: vynull's analysis replies, [request, 0, len, blob, 1] or, without
 * data, [request, 0, 0] (wave detail) / [request, 0, 0, blob(nil), 0]. */
static void send_blob_reply(int fd, uint32_t txid, uint16_t type, uint32_t request,
                            const uint8_t *blob, size_t len, bool nil_blob)
{
    djlink_db_arg_t a[5];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 5; i++) {
        a[i].type = DJLINK_DB_FIELD_INT32;
    }
    a[0].num = request;
    a[2].num = (uint32_t)len;
    a[3].type = DJLINK_DB_FIELD_BINARY;
    a[3].bin = blob;
    a[3].bin_len = len;
    a[4].num = len ? 1u : 0u;
    uint8_t argc = len || nil_blob ? 5u : 3u;
    size_t cap = len + 256u;
    uint8_t *buf = malloc(cap);
    int n = djlink_db_msg_build(txid, type, a, argc, buf, cap);
    if (n > 0) {
        write_full(fd, buf, (size_t)n);
    }
    free(buf);
}

static void send_analysis(int fd, const djlink_db_msg_t *m)
{
    bool wave = m->type == DJLINK_DB_TYPE_WAVEFORM_REQUEST;
    uint16_t reply = wave ? DJLINK_DB_TYPE_WAVEFORM_REPLY : DJLINK_DB_TYPE_BEATGRID_REPLY;
    if (m->arg_count < 2u || m->args[1].num != 101u) {
        send_blob_reply(fd, m->txid, reply, m->type, NULL, 0, !wave);
        return;
    }
    if (wave) {
        uint8_t *b = malloc(MOCK_WAVE_LEN);
        for (size_t i = 0; i < MOCK_WAVE_LEN; i++) {
            b[i] = mock_wave_byte(i);
        }
        send_blob_reply(fd, m->txid, reply, m->type, b, MOCK_WAVE_LEN, false);
        free(b);
        return;
    }
    /* 20-byte LE preamble, then 16 LE bytes per beat (128 BPM). */
    size_t len = 20u + 16u * MOCK_GRID_BEATS;
    uint8_t *b = calloc(1, len);
    b[2] = 0x08u;
    b[4] = (uint8_t)MOCK_GRID_BEATS;
    for (uint32_t i = 0; i < MOCK_GRID_BEATS; i++) {
        uint8_t *e = &b[20u + 16u * i];
        uint32_t t = 100u + (i * 60000u) / 128u;
        e[0] = (uint8_t)(i % 4u + 1u);
        e[2] = (uint8_t)(12800u & 0xffu);
        e[3] = (uint8_t)(12800u >> 8);
        e[4] = (uint8_t)t;
        e[5] = (uint8_t)(t >> 8);
        e[6] = (uint8_t)(t >> 16);
        e[7] = (uint8_t)(t >> 24);
        memset(&e[8], 0xff, 8);
    }
    send_blob_reply(fd, m->txid, reply, m->type, b, len, false);
    free(b);
}

/* v300: vynull's artwork reply, [request, 0, len, blob]; "not found" is
 * [request, 0x32, 0] with a fourth (blob) tag declared but never sent. */
/* v314: vynull's 0x2c04 answer for track 101's PWV4: [0x2c04, 0, len,
 * blob, 1], blob = LE section length + the "PWV4" section (24-byte head,
 * 1200 x 6 bytes). Another track: status 0x32, the blob never sent. */
static void send_anlz_tag(int fd, const djlink_db_msg_t *m)
{
    const uint32_t id = m->arg_count >= 2u ? m->args[1].num : 0u;
    const bool have = id == 101u;
    static uint8_t blob[4 + 24 + 7200];
    const uint32_t sec = 24u + 7200u;
    memset(blob, 0, sizeof(blob));
    blob[0] = (uint8_t)sec; blob[1] = (uint8_t)(sec >> 8);
    memcpy(&blob[4], "PWV4", 4);
    const uint32_t f[4] = { 24u, sec, 6u, 1200u };
    for (int k = 0; k < 4; k++) {
        for (int j = 0; j < 4; j++) blob[8 + k * 4 + j] = (uint8_t)(f[k] >> (24 - 8 * j));
    }
    for (uint32_t i = 0; i < 7200u; i++) blob[28 + i] = (uint8_t)(i * 5u + 1u);
    djlink_db_arg_t a[5];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 5; i++) a[i].type = DJLINK_DB_FIELD_INT32;
    a[0].num = DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST;
    a[1].num = have ? 0u : 0x32u;
    a[2].num = have ? (uint32_t)sizeof(blob) : 0u;
    a[3].type = DJLINK_DB_FIELD_BINARY;
    a[3].bin = blob;
    a[3].bin_len = have ? sizeof(blob) : 0u;
    a[4].num = 1u;
    static uint8_t buf[sizeof(blob) + 256u];
    int n = djlink_db_msg_build(m->txid, DJ_LINK_DB_TYPE_ANLZ_TAG_REPLY, a, have ? 5u : 4u,
                                buf, sizeof(buf));
    if (n > 0) {
        write_full(fd, buf, (size_t)n - (have ? 0u : 5u));
    }
}

static void send_artwork(int fd, const djlink_db_msg_t *m)
{
    uint32_t id = m->arg_count >= 2u ? m->args[1].num : 0u;
    size_t len = id == MOCK_ART_ID ? MOCK_ART_LEN : id == MOCK_ART_BIG_ID ? MOCK_ART_BIG : 0u;
    uint8_t *blob = calloc(1, len + 1u);
    for (size_t i = 0; i < len; i++) {
        blob[i] = (uint8_t)(i * 3u + id);
    }
    djlink_db_arg_t a[4];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 3; i++) {
        a[i].type = DJLINK_DB_FIELD_INT32;
    }
    a[0].num = m->type;
    a[1].num = len ? 0u : 0x32u;
    a[2].num = (uint32_t)len;
    a[3].type = DJLINK_DB_FIELD_BINARY;
    a[3].bin = blob;
    a[3].bin_len = len;
    size_t cap = len + 256u;
    uint8_t *buf = malloc(cap);
    int n = djlink_db_msg_build(m->txid, DJLINK_DB_TYPE_ARTWORK, a, 4u, buf, cap);
    if (n > 0) {
        /* Not found: drop the empty blob field (tag + u32 length). */
        write_full(fd, buf, (size_t)n - (len ? 0u : 5u));
    }
    free(buf);
    free(blob);
}

/* v303: vynull's nxs2 cue list. Track 101: [0x2705, 0, len, blob, 2] with
 * two 124-byte entries (memory cue 0 at 1234 ms, hot cue B loop 2000-4000).
 * Anything else: the empty answer, tags 06 06 06 03 06 but only four int32
 * on the wire, [0x2b04, 1, 0, 0] (the blob's place holds a zero int32). */
#define MOCK_CUE_ENTRY 124u
static void send_cues(int fd, const djlink_db_msg_t *m)
{
    djlink_db_arg_t a[5];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 5; i++) {
        a[i].type = DJLINK_DB_FIELD_INT32;
    }
    uint8_t blob[2u * MOCK_CUE_ENTRY];
    uint8_t buf[512];
    int n;
    if (m->arg_count >= 2u && m->args[1].num == 101u) {
        memset(blob, 0, sizeof(blob));
        for (int k = 0; k < 2; k++) {
            uint8_t *e = &blob[k * MOCK_CUE_ENTRY];
            const uint32_t at = k ? 2000u : 1234u;
            const uint32_t end = k ? 4000u : 0xffffffffu;
            e[0] = (uint8_t)MOCK_CUE_ENTRY;
            e[4] = (uint8_t)(k ? 2u : 0u);      /* number: 0 memory, B */
            e[6] = (uint8_t)(k ? 2u : 1u);      /* type: cue / loop */
            e[0x0c] = (uint8_t)at;
            e[0x0d] = (uint8_t)(at >> 8);
            for (int b = 0; b < 4; b++) {
                e[0x20 + b] = (uint8_t)(end >> (8 * b));
            }
        }
        a[0].num = 0x2705u;
        a[2].num = sizeof(blob);
        a[3].type = DJLINK_DB_FIELD_BINARY;
        a[3].bin = blob;
        a[3].bin_len = sizeof(blob);
        a[4].num = 2u;
        n = djlink_db_msg_build(m->txid, DJLINK_DB_TYPE_CUES_EXT_REPLY, a, 5u, buf, sizeof(buf));
    } else {
        a[0].num = DJLINK_DB_TYPE_CUES_EXT_REQUEST;
        a[1].num = 1u;
        n = djlink_db_msg_build(m->txid, DJLINK_DB_TYPE_CUES_EXT_REPLY, a, 4u, buf, sizeof(buf));
        if (n > 0) {
            /* argc 5, tags 06 06 06 03 06: the builder writes 12 tag bytes
             * after the argc field (bytes 20..31). */
            buf[14] = 5u;
            buf[23] = 0x03u;
            buf[24] = 0x06u;
        }
    }
    if (n > 0) {
        write_full(fd, buf, (size_t)n);
    }
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
            mk->track_menu_sort = m.arg_count > 1 ? m.args[1].num : 0xffffffffu;
            last_request = m.type;
            if (mk->mode == MOCK_BAD_MAGIC) {
                static const uint8_t junk[20] = {0x11, 0xde, 0xad, 0xbe, 0xef};
                write_full(fd, junk, sizeof(junk));
                continue;
            }
            send_success(fd, m.txid, m.type, 3);
        } else if (m.type == DJ_LINK_DB_TYPE_PLAYLIST_MENU) {
            /* v311: folder 12 holds folder 13 and playlist 40; playlist 40
             * holds tracks 103 then 101 (its own order). */
            mk->playlist_menus++;
            mk->playlist_argc = m.arg_count;
            mk->playlist_sort = m.arg_count > 1 ? m.args[1].num : 0xffffffffu;
            mk->playlist_id = m.arg_count > 2 ? m.args[2].num : 0xffffffffu;
            mk->playlist_folder = m.arg_count > 3 ? m.args[3].num : 0xffffffffu;
            last_request = m.type;
            send_success(fd, m.txid, m.type, 2);
        } else if (m.type == DJLINK_DB_TYPE_METADATA_REQUEST) {
            mk->metadata_requests++;
            mk->metadata_seq = ++mk->seq;
            mk->metadata_id = m.args[1].num;
            last_request = m.type;
            last_id = m.args[1].num;
            send_success(fd, m.txid, m.type, 4);
        } else if (m.type == DJ_LINK_DB_TYPE_TRACK_INFO) {
            /* As vynull answers it: 7 items, the path one of type 0x0000;
             * an unknown id gets an empty menu. */
            mk->track_info_requests++;
            mk->track_info_seq = ++mk->seq;
            mk->track_info_dmst = m.args[0].num;
            mk->track_info_id = m.args[1].num;
            last_request = m.type;
            last_id = m.args[1].num;
            bool known = false;
            for (int i = 0; i < 3; i++) {
                known = known || k_tracks[i].id == last_id;
            }
            send_success(fd, m.txid, m.type, known ? 7u : 0u);
        } else if (m.type == DJLINK_DB_TYPE_WAVEFORM_REQUEST ||
                   m.type == DJLINK_DB_TYPE_BEATGRID_REQUEST) {
            if (m.type == DJLINK_DB_TYPE_WAVEFORM_REQUEST) {
                mk->wave_requests++;
                mk->wave_argc = m.arg_count;
                mk->wave_dmst = m.args[0].num;
            } else {
                mk->grid_requests++;
                mk->grid_argc = m.arg_count;
                mk->grid_id = m.args[1].num;
            }
            send_analysis(fd, &m);
        } else if (m.type == DJLINK_DB_TYPE_CUES_EXT_REQUEST) {
            mk->cue_requests++;
            mk->cue_argc = m.arg_count;
            mk->cue_id = m.arg_count >= 2u ? m.args[1].num : 0u;
            send_cues(fd, &m);
        } else if (m.type == DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST) {
            mk->tag_requests++;
            mk->tag_argc = m.arg_count;
            mk->tag_id = m.arg_count >= 2u ? m.args[1].num : 0u;
            mk->tag_fourcc = m.arg_count >= 3u ? m.args[2].num : 0u;
            mk->tag_file = m.arg_count >= 4u ? m.args[3].num : 0u;
            send_anlz_tag(fd, &m);
        } else if (m.type == DJLINK_DB_TYPE_ARTWORK_REQUEST) {
            mk->art_requests++;
            mk->art_argc = m.arg_count;
            mk->art_id = m.arg_count >= 2u ? m.args[1].num : 0u;
            send_artwork(fd, &m);
        } else if (m.type == DJLINK_DB_TYPE_RENDER) {
            uint32_t offset = m.args[1].num;
            uint32_t limit = m.args[2].num;
            mk->renders++;
            send_msg(fd, m.txid, DJLINK_DB_TYPE_MENU_HEADER, NULL, 0);
            if (last_request == DJ_LINK_DB_TYPE_PLAYLIST_MENU && mk->playlist_folder == 1u) {
                if (offset == 0u) {
                    send_item(fd, m.txid, 13, "Deep", "", DJ_LINK_DB_ITEM_FOLDER);
                }
                if (offset + limit > 1u) {
                    send_item(fd, m.txid, 40, "Warm up", "", DJ_LINK_DB_ITEM_PLAYLIST);
                }
            } else if (last_request == DJ_LINK_DB_TYPE_PLAYLIST_MENU) {
                static const int order[2] = { 2, 0 };
                for (uint32_t i = offset; i < 2u && i < offset + limit; i++) {
                    const int k = order[i];
                    send_item_art(fd, m.txid, k_tracks[k].id, k_tracks[k].title,
                                  k_tracks[k].artist, DJ_LINK_DB_ITEM_TITLE_ARTIST,
                                  k_tracks[k].artwork);
                }
            } else if (last_request == DJ_LINK_DB_TYPE_TRACK_MENU && mk->mode == MOCK_VYNULL) {
                for (uint32_t i = offset; i < 3u && i < offset + limit; i++) {
                    char bpm[24];
                    snprintf(bpm, sizeof(bpm), "%.1f bpm - 8A", k_tracks[i].bpm100 / 100.0);
                    send_item_art(fd, m.txid, k_tracks[i].id, k_tracks[i].title, bpm,
                                  DJ_LINK_DB_ITEM_TEMPO_TITLE, k_tracks[i].artwork);
                }
            } else if (last_request == DJ_LINK_DB_TYPE_TRACK_MENU) {
                for (uint32_t i = offset; i < 3u && i < offset + limit; i++) {
                    send_item_art(fd, m.txid, k_tracks[i].id, k_tracks[i].title,
                                  k_tracks[i].artist, DJ_LINK_DB_ITEM_TITLE_ARTIST,
                                  k_tracks[i].artwork);
                }
            } else if (last_request == DJ_LINK_DB_TYPE_TRACK_INFO) {
                for (int i = 0; i < 3; i++) {
                    if (k_tracks[i].id != last_id) {
                        continue;
                    }
                    send_item(fd, m.txid, 0, k_tracks[i].title, "", DJ_LINK_DB_ITEM_TITLE);
                    send_item(fd, m.txid, k_tracks[i].seconds, "", "",
                              DJ_LINK_DB_ITEM_DURATION);
                    send_item(fd, m.txid, k_tracks[i].bpm100, "", "", DJ_LINK_DB_ITEM_TEMPO);
                    send_item(fd, m.txid, k_tracks[i].id, "a comment", "", 0x0023u);
                    send_item(fd, m.txid, k_tracks[i].id, k_tracks[i].path, "",
                              DJ_LINK_DB_ITEM_FILE_PATH);
                    send_item(fd, m.txid, 1, "", "", 0x002fu);
                    send_item(fd, m.txid, 5, "8A", "", 0x000fu);
                }
            } else if (mk->mode == MOCK_VYNULL) {
                for (int i = 0; i < 3; i++) {
                    if (k_tracks[i].id != last_id) {
                        continue;
                    }
                    const uint32_t id = k_tracks[i].id;
                    send_item_art(fd, m.txid, id, k_tracks[i].title, "", DJ_LINK_DB_ITEM_TITLE,
                                  k_tracks[i].artwork);
                    send_item(fd, m.txid, id, k_tracks[i].artist, "", DJ_LINK_DB_ITEM_ARTIST);
                    send_item(fd, m.txid, id, "An Album", "", 0x0002u);
                    send_item(fd, m.txid, k_tracks[i].seconds, "", "",
                              DJ_LINK_DB_ITEM_DURATION);
                    send_item(fd, m.txid, k_tracks[i].bpm100, "", "", DJ_LINK_DB_ITEM_TEMPO);
                    send_item(fd, m.txid, 5u, "8A", "", 0x000fu);
                    send_item(fd, m.txid, 0u, "", "", 0x000au);
                    send_item(fd, m.txid, 0u, "", "", 0x0013u);
                    send_item(fd, m.txid, 0u, "Techno", "", 0x0006u);
                    send_item(fd, m.txid, id, "2026-09-30", "", 0x002eu);
                    send_item(fd, m.txid, id, "a comment", "", 0x0023u);
                    send_item(fd, m.txid, 0u, "", "", 0x000eu);
                    send_item(fd, m.txid, 320u, "", "", 0x0010u);
                    send_item(fd, m.txid, 1999u, "", "", 0x0011u);
                    send_item(fd, m.txid, 0u, "Original Someone", "", 0x0028u);
                    send_item(fd, m.txid, 0u, "A Remixer", "", 0x0029u);
                }
            } else {
                for (int i = 0; i < 3; i++) {
                    if (k_tracks[i].id != last_id) {
                        continue;
                    }
                    send_item_art(fd, m.txid, 0, k_tracks[i].title, "", DJ_LINK_DB_ITEM_TITLE,
                                  k_tracks[i].artwork);
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
    int paths;                 /* v297: io.path answers */
    uint32_t path_id;
    char path[DJ_LINK_DB_PATH_MAX];
    int blobs;                 /* v300: io.blob answers */
    uint32_t blob_id;
    uint16_t blob_request;
    size_t blob_len;
    bool blob_answered;        /* v303: last answer had the reply type */
    int blobs_answered;
    /* v303: the next blob, asked from inside io.blob like the fetch job's
     * wave -> grid -> cues -> art chain (0 = none). */
    uint16_t chain_request;
    uint32_t chain_id;
    uint8_t *chain_dst;
    size_t chain_cap;
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
        d->rows[index].artwork_id = t->artwork_id;
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

static void io_path(void *ctx, uint32_t id, const char *path)
{
    driver_t *d = ctx;
    d->paths++;
    d->path_id = id;
    snprintf(d->path, sizeof(d->path), "%s", path);
}

static dj_link_db_t s_client;

static void io_blob(void *ctx, uint32_t id, uint16_t request, size_t len, bool answered)
{
    driver_t *d = ctx;
    d->blobs++;
    d->blob_answered = answered;
    d->blobs_answered += answered ? 1 : 0;
    d->blob_id = id;
    d->blob_request = request;
    d->blob_len = len;
    if (d->chain_request != 0u) {
        uint16_t next = d->chain_request;
        d->chain_request = 0u;
        (void)dj_link_db_want_blob(&s_client, next, d->chain_id, d->chain_dst, d->chain_cap,
                                   now_ms());
    }
}


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

static int s_paths_wanted;
static bool path_or_failed(void)
{
    return s_driver->paths >= s_paths_wanted || s_client.phase == DJ_LINK_DB_FAILED;
}

static int s_blobs_wanted;
static bool blob_or_failed(void)
{
    return s_driver->blobs >= s_blobs_wanted || s_client.phase == DJ_LINK_DB_FAILED;
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

static void client_start_slot(driver_t *d, mock_t *mk, uint32_t step_timeout_ms,
                              uint8_t slot)
{
    memset(d, 0, sizeof(*d));
    d->mock = mk;
    d->fd = -1;
    d->want_detail = UINT32_MAX;
    s_driver = d;
    dj_link_db_io_t io = {
        .connect = io_connect, .send = io_send, .close = io_close,
        .list_begin = io_list_begin, .track = io_track, .next_detail = io_next_detail,
        .path = io_path, .blob = io_blob, .ctx = d,
    };
    dj_link_db_init(&s_client, &io, MAX_ROWS);
    s_client.discovery_port = mk->disc_port;
    s_client.step_timeout_ms = step_timeout_ms;
    dj_link_db_start(&s_client, PEER_IP, PEER_NUMBER, slot, OUR_NUMBER, now_ms());
}

static void client_start(driver_t *d, mock_t *mk, uint32_t step_timeout_ms)
{
    client_start_slot(d, mk, step_timeout_ms, DJLINK_SLOT_USB);
}

/* v297: a rekordbox source's collection (slot 4) has no export.pdb; the
 * file path comes from track info 0x2102, ahead of pending metadata. */
static void test_track_info_path(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start_slot(&d, &mk, 2000, DJLINK_SLOT_LAPTOP);
    CHECK(!dj_link_db_want_path(&s_client, 0u, now_ms()));
    /* Asked before the list is in: kept, sent once it is. */
    CHECK(dj_link_db_want_path(&s_client, 102u, now_ms()));
    run(&d, listed_or_failed, 3000);
    CHECK(d.listed == 3u);
    d.want_detail = 0;
    s_paths_wanted = 1;
    run(&d, path_or_failed, 3000);
    CHECK(d.paths == 1);
    CHECK(d.path_id == 102u);
    CHECK(strcmp(d.path, "/Music/Energy 52/Caf\xc3\xa9 del Mar.flac") == 0);
    CHECK(mk.track_info_requests == 1);
    CHECK(mk.track_info_id == 102u);
    CHECK(mk.track_info_dmst ==
          ((OUR_NUMBER << 24) | (1u << 16) | (DJLINK_SLOT_LAPTOP << 8) | 1u));
    /* The path went out before the row metadata that was already wanted. */
    run(&d, detail_or_failed, 3000);
    CHECK(d.details == 1 && d.rows[0].duration_s == 391u);
    CHECK(mk.track_info_seq < mk.metadata_seq);

    /* Unknown id: an empty answer, and the session carries on. */
    CHECK(dj_link_db_want_path(&s_client, 999u, now_ms()));
    s_paths_wanted = 2;
    run(&d, path_or_failed, 3000);
    CHECK(d.paths == 2 && d.path_id == 999u && d.path[0] == '\0');
    CHECK(s_client.phase == DJ_LINK_DB_READY);
    CHECK(d.connects == 2);

    /* After an idle close, a path request reconnects on its own. */
    dj_link_db_on_closed(&s_client, now_ms());
    io_close(&d);
    CHECK(s_client.phase == DJ_LINK_DB_IDLE);
    CHECK(dj_link_db_want_path(&s_client, 101u, now_ms()));
    dj_link_db_poll(&s_client, now_ms());
    CHECK(d.connects == 3 && s_client.phase == DJ_LINK_DB_DISC_CONNECTING);

    /* Stop drops the request; a stopped client takes none. */
    dj_link_db_stop(&s_client);
    CHECK(s_client.path_id == 0u);
    CHECK(!dj_link_db_want_path(&s_client, 101u, now_ms()));
    mock_stop(&mk, th);
}

/* v300: wave detail and beat grid stream into the owner's buffer, even
 * past the rx buffer; a missing analysis is an empty answer. */
static void test_analysis_blobs(void)
{
    static uint8_t wave[16384];
    static uint8_t grid[4096];
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start_slot(&d, &mk, 2000, DJLINK_SLOT_LAPTOP);
    CHECK(!dj_link_db_want_blob(&s_client, 0x1234u, 101u, wave, sizeof(wave), now_ms()));
    CHECK(!dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_WAVEFORM_REQUEST, 101u, NULL,
                                sizeof(wave), now_ms()));
    /* Asked before the list is in: sent once it is. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_WAVEFORM_REQUEST, 101u, wave,
                               sizeof(wave), now_ms()));
    CHECK(!dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_BEATGRID_REQUEST, 101u, grid,
                                sizeof(grid), now_ms()));
    run(&d, listed_or_failed, 3000);
    CHECK(d.listed == 3u);
    s_blobs_wanted = 1;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 1);
    CHECK(d.blob_id == 101u && d.blob_request == DJLINK_DB_TYPE_WAVEFORM_REQUEST);
    CHECK(d.blob_len == MOCK_WAVE_LEN);
    CHECK(MOCK_WAVE_LEN > DJ_LINK_DB_RX_MAX);
    bool same = true;
    for (size_t i = 0; i < MOCK_WAVE_LEN; i++) {
        same = same && wave[i] == mock_wave_byte(i);
    }
    CHECK(same);
    CHECK(mk.wave_requests == 1 && mk.wave_argc == 3u);
    CHECK(mk.wave_dmst ==
          ((OUR_NUMBER << 24) | (1u << 16) | (DJLINK_SLOT_LAPTOP << 8) | 1u));
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* Beat grid. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_BEATGRID_REQUEST, 101u, grid,
                               sizeof(grid), now_ms()));
    s_blobs_wanted = 2;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 2 && d.blob_request == DJLINK_DB_TYPE_BEATGRID_REQUEST);
    CHECK(d.blob_len == 20u + 16u * MOCK_GRID_BEATS);
    CHECK(mk.grid_requests == 1 && mk.grid_argc == 2u && mk.grid_id == 101u);
    CHECK(grid[20] == 1u && grid[36] == 2u && grid[22] == (12800u & 0xffu));

    /* A smaller buffer keeps the head of the blob; the session goes on. */
    memset(wave, 0, sizeof(wave));
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_WAVEFORM_REQUEST, 101u, wave,
                               1000u, now_ms()));
    s_blobs_wanted = 3;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 3 && d.blob_len == 1000u);
    CHECK(wave[999] == mock_wave_byte(999) && wave[1000] == 0u);
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* No analysis: [req, 0, 0] and [req, 0, 0, nil, 0] answer 0 bytes. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_WAVEFORM_REQUEST, 999u, wave,
                               sizeof(wave), now_ms()));
    s_blobs_wanted = 4;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 4 && d.blob_id == 999u && d.blob_len == 0u);
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_BEATGRID_REQUEST, 999u, grid,
                               sizeof(grid), now_ms()));
    s_blobs_wanted = 5;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 5 && d.blob_len == 0u);
    CHECK(d.blobs_answered == 5);   /* "no analysis" is still an answer */
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* Cancelled in flight: drained without an answer or a write. */
    memset(wave, 0, sizeof(wave));
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_WAVEFORM_REQUEST, 101u, wave,
                               sizeof(wave), now_ms()));
    CHECK(s_client.phase == DJ_LINK_DB_BLOB_WAIT);
    dj_link_db_cancel_blob(&s_client);
    run(&d, failed, 300);
    CHECK(d.blobs == 5 && wave[1] == 0u);
    CHECK(s_client.phase == DJ_LINK_DB_READY && s_client.blob_request == 0u);
    CHECK(mk.wave_requests == 4);

    /* The session still answers paths afterwards. */
    CHECK(dj_link_db_want_path(&s_client, 102u, now_ms()));
    s_paths_wanted = 1;
    run(&d, path_or_failed, 3000);
    CHECK(d.paths == 1 && d.path_id == 102u && d.path[0] == '/');
    CHECK(d.connects == 2);

    /* Stop drops a wanted blob. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_BEATGRID_REQUEST, 101u, grid,
                               sizeof(grid), now_ms()));
    dj_link_db_stop(&s_client);
    CHECK(s_client.blob_request == 0u && s_client.blob_dst == NULL);
    mock_stop(&mk, th);
}

/* v303: the nxs2 cue list streams like the other analysis blobs, with the
 * trailing 0 argument; vynull's empty answer (a zero int32 in the declared
 * blob's place) answers 0 bytes and leaves the next reply framed. */
static void test_cues(void)
{
    static uint8_t cues[1024];
    static uint8_t wave[4096];
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start(&d, &mk, 2000);
    run(&d, listed_or_failed, 3000);
    CHECK(d.listed == 3u);

    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_CUES_EXT_REQUEST, 101u, cues,
                               sizeof(cues), now_ms()));
    s_blobs_wanted = 1;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 1 && d.blob_id == 101u);
    CHECK(d.blob_request == DJLINK_DB_TYPE_CUES_EXT_REQUEST && d.blob_len == 2u * MOCK_CUE_ENTRY);
    CHECK(cues[0] == MOCK_CUE_ENTRY && cues[MOCK_CUE_ENTRY + 4u] == 2u);
    CHECK(mk.cue_requests == 1 && mk.cue_argc == 3u && mk.cue_id == 101u);
    CHECK(d.blob_answered && d.blobs_answered == 1);
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* Empty answer, then the next request straight from io.blob: the
     * stray int32 must be gone before the wave reply is framed. */
    d.chain_request = DJLINK_DB_TYPE_WAVEFORM_REQUEST;
    d.chain_id = 101u;
    d.chain_dst = wave;
    d.chain_cap = sizeof(wave);
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_CUES_EXT_REQUEST, 102u, cues,
                               sizeof(cues), now_ms()));
    s_blobs_wanted = 3;
    run(&d, blob_or_failed, 3000);
    CHECK(s_client.phase == DJ_LINK_DB_READY);
    CHECK(d.blobs == 3 && d.blob_id == 101u && d.blob_len == sizeof(wave));
    CHECK(mk.cue_requests == 2 && mk.wave_requests == 1 && d.connects == 2);
    CHECK(d.blobs_answered == 3);   /* the empty cue list counts as "none" */
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);
}

/* v300: the artwork id rides on the title items (list and metadata); the
 * JPEG streams like an analysis blob, a too-big one is dropped whole, and
 * vynull's "not found" (blob declared, never sent) answers 0 bytes. */
/* v314: the PWV4 colour preview of a track over 0x2c04, then the session
 * stays framed; a track without one answers "not found" (len 0). */
static void test_anlz_tag_pwv4(void)
{
    static uint8_t blob[DJ_LINK_ANLZ_COLOR_BLOB_MAX];
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start(&d, &mk, 2000);
    run(&d, listed_or_failed, 3000);
    CHECK(dj_link_db_want_blob(&s_client, DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST, 101u, blob,
                               sizeof(blob), now_ms()));
    s_blobs_wanted = 1;
    run(&d, blob_or_failed, 3000);
    CHECK(mk.tag_requests == 1 && mk.tag_argc == 4u && mk.tag_id == 101u);
    CHECK(mk.tag_fourcc == DJ_LINK_DB_ANLZ_TAG_PWV4 && mk.tag_file == DJ_LINK_DB_ANLZ_FILE_EXT);
    CHECK(d.blobs == 1 && d.blob_request == DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST);
    CHECK(d.blob_len == sizeof(blob));
    const uint8_t *e = NULL;
    size_t n = 0;
    CHECK(dj_link_anlz_color_entries(blob, d.blob_len, &e, &n) && n == 7200u);
    CHECK(e && e[0] == 1u && e[1] == 6u && e[7199] == (uint8_t)(7199u * 5u + 1u));
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    CHECK(dj_link_db_want_blob(&s_client, DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST, 102u, blob,
                               sizeof(blob), now_ms()));
    s_blobs_wanted = 2;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 2 && d.blob_len == 0u && s_client.phase == DJ_LINK_DB_READY);
    /* and the session is still framed */
    d.want_detail = 1;
    run(&d, detail_or_failed, 3000);
    CHECK(d.rows[1].has_detail && s_client.phase == DJ_LINK_DB_READY);
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);
}

static void test_artwork(void)
{
    static uint8_t art[4096];
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    client_start(&d, &mk, 2000);
    run(&d, listed_or_failed, 3000);
    CHECK(d.listed == 3u);
    CHECK(d.rows[0].artwork_id == MOCK_ART_ID);
    CHECK(d.rows[1].artwork_id == 0u);
    CHECK(d.rows[2].artwork_id == MOCK_ART_BIG_ID);
    d.rows[0].artwork_id = 0u;
    d.want_detail = 0;
    run(&d, detail_or_failed, 3000);
    CHECK(d.details == 1 && d.rows[0].artwork_id == MOCK_ART_ID);

    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_ARTWORK_REQUEST, MOCK_ART_ID, art,
                               sizeof(art), now_ms()));
    s_blobs_wanted = 1;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 1 && d.blob_id == MOCK_ART_ID);
    CHECK(d.blob_request == DJLINK_DB_TYPE_ARTWORK_REQUEST && d.blob_len == MOCK_ART_LEN);
    CHECK(art[0] == (uint8_t)MOCK_ART_ID && art[MOCK_ART_LEN - 1u] ==
          (uint8_t)((MOCK_ART_LEN - 1u) * 3u + MOCK_ART_ID));
    CHECK(mk.art_requests == 1 && mk.art_argc == 2u && mk.art_id == MOCK_ART_ID);
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* Bigger than the buffer: no partial JPEG, the session goes on. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_ARTWORK_REQUEST, MOCK_ART_BIG_ID, art,
                               sizeof(art), now_ms()));
    s_blobs_wanted = 2;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 2 && d.blob_id == MOCK_ART_BIG_ID && d.blob_len == 0u);
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* Not found: the phantom blob tag does not stall the session. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_ARTWORK_REQUEST, 5u, art,
                               sizeof(art), now_ms()));
    s_blobs_wanted = 3;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 3 && d.blob_id == 5u && d.blob_len == 0u);
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    /* The next request is framed right after it. */
    CHECK(dj_link_db_want_blob(&s_client, DJLINK_DB_TYPE_ARTWORK_REQUEST, MOCK_ART_ID, art,
                               sizeof(art), now_ms()));
    s_blobs_wanted = 4;
    run(&d, blob_or_failed, 3000);
    CHECK(d.blobs == 4 && d.blob_len == MOCK_ART_LEN);
    CHECK(d.connects == 2 && mk.art_requests == 4);
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);
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
    CHECK(mk.track_menu_sort == DJ_LINK_DB_SORT_DEFAULT);
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

/* v307: vynull's list rows carry title and "BPM - key", never the artist:
 * the row artist stays empty (not the BPM text) and the metadata request
 * brings it, whatever row asks. */
static void test_vynull_artist_from_metadata(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_VYNULL);
    client_start(&d, &mk, 2000);

    run(&d, listed_or_failed, 3000);
    CHECK(s_client.phase == DJ_LINK_DB_READY);
    CHECK(d.listed == 3u);
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(d.rows[i].rekordbox_id == k_tracks[i].id);
        CHECK(strcmp(d.rows[i].title, k_tracks[i].title) == 0);
        CHECK(d.rows[i].artist[0] == '\0');
        CHECK(!d.rows[i].has_detail);
        CHECK(d.rows[i].artwork_id == k_tracks[i].artwork);
    }

    /* Last row, as a network load-track off the visible page asks it. */
    d.want_detail = 2;
    run(&d, detail_or_failed, 3000);
    CHECK(mk.metadata_requests == 1);
    CHECK(mk.metadata_id == 103u);
    CHECK(d.rows[2].has_detail);
    CHECK(strcmp(d.rows[2].artist, "Aphex Twin") == 0);   /* not original artist / remixer */
    CHECK(d.rows[2].duration_s == 367u);
    CHECK(d.rows[2].bpm100 == 12800u);
    CHECK(d.rows[2].artwork_id == MOCK_ART_BIG_ID);
    CHECK(d.rows[0].artist[0] == '\0' && d.rows[1].artist[0] == '\0');

    /* All 16 items were consumed: the next request on the session still works. */
    d.details = 0;
    d.want_detail = 1;
    run(&d, detail_or_failed, 3000);
    CHECK(mk.metadata_requests == 2);
    CHECK(strcmp(d.rows[1].artist, "Energy 52") == 0);
    CHECK(s_client.phase == DJ_LINK_DB_READY);

    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);
}

/* v307: the metadata order: a row being loaded first, then the page. */
static void test_pick_detail(void)
{
    dj_link_peer_track_t rows[6];
    memset(rows, 0, sizeof(rows));
    for (uint32_t i = 0; i < 6u; i++) {
        rows[i].rekordbox_id = 200u + i;
    }
    uint32_t idx = UINT32_MAX;
    /* no priority: first row of the window without metadata */
    rows[1].has_detail = true;
    CHECK(dj_link_db_pick_detail(rows, 6u, 1u, 3u, 0u, 0u, &idx) && idx == 2u);
    /* priority row off the window wins */
    CHECK(dj_link_db_pick_detail(rows, 6u, 1u, 3u, 5u, 205u, &idx) && idx == 5u);
    /* stale priority (another track at that index now): the window */
    CHECK(dj_link_db_pick_detail(rows, 6u, 1u, 3u, 5u, 999u, &idx) && idx == 2u);
    /* priority already answered: the window */
    rows[5].has_detail = true;
    CHECK(dj_link_db_pick_detail(rows, 6u, 1u, 3u, 5u, 205u, &idx) && idx == 2u);
    /* priority beyond the listed rows: ignored */
    CHECK(dj_link_db_pick_detail(rows, 4u, 0u, 0u, 5u, 205u, &idx) == false);
    /* no window (stale page generation), only the priority row */
    rows[5].has_detail = false;
    CHECK(dj_link_db_pick_detail(rows, 6u, 0u, 0u, 5u, 205u, &idx) && idx == 5u);
    /* window clipped to the list, overflow-safe; id 0 rows skipped */
    for (uint32_t i = 0; i < 6u; i++) rows[i].has_detail = true;
    rows[4].has_detail = false;
    rows[3].has_detail = false;
    rows[3].rekordbox_id = 0u;
    CHECK(dj_link_db_pick_detail(rows, 6u, 3u, UINT32_MAX, 0u, 0u, &idx) && idx == 4u);
    rows[4].has_detail = true;
    CHECK(dj_link_db_pick_detail(rows, 6u, 0u, 64u, 0u, 0u, &idx) == false);
    CHECK(dj_link_db_pick_detail(NULL, 6u, 0u, 6u, 0u, 0u, &idx) == false);
}

/* v310: the sort order goes out with the all-tracks menu; a start keeps
 * it, init resets it. */
static void test_sort_request(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    memset(&d, 0, sizeof(d));
    d.mock = &mk;
    d.fd = -1;
    d.want_detail = UINT32_MAX;
    s_driver = &d;
    dj_link_db_io_t io = {
        .connect = io_connect, .send = io_send, .close = io_close,
        .list_begin = io_list_begin, .track = io_track, .next_detail = io_next_detail,
        .path = io_path, .blob = io_blob, .ctx = &d,
    };
    dj_link_db_init(&s_client, &io, MAX_ROWS);
    CHECK(s_client.sort == DJ_LINK_DB_SORT_DEFAULT);
    s_client.discovery_port = mk.disc_port;
    s_client.step_timeout_ms = 2000;
    dj_link_db_set_sort(&s_client, DJ_LINK_DB_SORT_BPM);
    dj_link_db_start(&s_client, PEER_IP, PEER_NUMBER, DJLINK_SLOT_USB, OUR_NUMBER, now_ms());
    CHECK(s_client.sort == DJ_LINK_DB_SORT_BPM);
    run(&d, listed_or_failed, 3000);
    CHECK(dj_link_db_list_done(&s_client));
    CHECK(mk.track_menus == 1 && mk.track_menu_sort == DJ_LINK_DB_SORT_BPM);
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);
    dj_link_db_set_sort(NULL, 1);   /* ignored */
}

/* v311: a playlist folder lists folders and playlists (not tracks: no
 * metadata asked for them, never downloadable); a playlist lists its tracks
 * in its order, sort 0 even when all tracks were sorted. */
static void test_playlist_menus(void)
{
    mock_t mk;
    pthread_t th;
    driver_t d;
    mock_start(&mk, &th, MOCK_NORMAL);
    memset(&d, 0, sizeof(d));
    d.mock = &mk;
    d.fd = -1;
    d.want_detail = UINT32_MAX;
    s_driver = &d;
    dj_link_db_io_t io = {
        .connect = io_connect, .send = io_send, .close = io_close,
        .list_begin = io_list_begin, .track = io_track, .next_detail = io_next_detail,
        .path = io_path, .blob = io_blob, .ctx = &d,
    };
    dj_link_db_init(&s_client, &io, MAX_ROWS);
    CHECK(s_client.menu == DJ_LINK_DB_MENU_ALL_TRACKS && s_client.menu_id == 0u);
    s_client.discovery_port = mk.disc_port;
    s_client.step_timeout_ms = 2000;
    dj_link_db_set_sort(&s_client, DJ_LINK_DB_SORT_BPM);
    dj_link_db_set_menu(&s_client, DJ_LINK_DB_MENU_FOLDER, 12);
    dj_link_db_start(&s_client, PEER_IP, PEER_NUMBER, DJLINK_SLOT_USB, OUR_NUMBER, now_ms());
    run(&d, listed_or_failed, 3000);
    CHECK(dj_link_db_list_done(&s_client));
    CHECK(mk.track_menus == 0 && mk.playlist_menus == 1);
    CHECK(mk.playlist_argc == 4u && mk.playlist_sort == DJ_LINK_DB_SORT_DEFAULT);
    CHECK(mk.playlist_id == 12u && mk.playlist_folder == 1u);
    CHECK(d.total == 2u && d.listed == 2u);
    CHECK(d.rows[0].kind == DJ_LINK_PEER_ROW_FOLDER && d.rows[0].rekordbox_id == 13u);
    CHECK(strcmp(d.rows[0].title, "Deep") == 0 && d.rows[0].artist[0] == '\0');
    CHECK(d.rows[1].kind == DJ_LINK_PEER_ROW_PLAYLIST && d.rows[1].rekordbox_id == 40u);
    CHECK(strcmp(d.rows[1].title, "Warm up") == 0);
    for (int i = 0; i < 2; i++) {
        CHECK(d.rows[i].has_detail && d.rows[i].audio == DJ_LINK_PEER_AUDIO_METADATA_ONLY);
        CHECK(dj_link_peer_load_check(&d.rows[i], NULL, 0) != DJ_LINK_PEER_LOAD_OK);
    }
    /* Nothing to fetch for them, even when asked. */
    uint32_t idx;
    CHECK(!dj_link_db_pick_detail(d.rows, 2, 0, 2, 0, 13u, &idx));
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);

    /* Playlist 40: its tracks, in its order. */
    mock_start(&mk, &th, MOCK_NORMAL);
    memset(&d, 0, sizeof(d));
    d.mock = &mk;
    d.fd = -1;
    d.want_detail = UINT32_MAX;
    s_driver = &d;
    dj_link_db_init(&s_client, &io, MAX_ROWS);
    s_client.discovery_port = mk.disc_port;
    s_client.step_timeout_ms = 2000;
    s_client.audio = DJ_LINK_PEER_AUDIO_NFS;
    dj_link_db_set_menu(&s_client, DJ_LINK_DB_MENU_PLAYLIST, 40);
    dj_link_db_start(&s_client, PEER_IP, PEER_NUMBER, DJLINK_SLOT_USB, OUR_NUMBER, now_ms());
    run(&d, listed_or_failed, 3000);
    CHECK(mk.playlist_menus == 1 && mk.playlist_id == 40u && mk.playlist_folder == 0u);
    CHECK(d.listed == 2u);
    CHECK(d.rows[0].kind == DJ_LINK_PEER_ROW_TRACK && d.rows[0].rekordbox_id == k_tracks[2].id);
    CHECK(d.rows[1].rekordbox_id == k_tracks[0].id);
    CHECK(strcmp(d.rows[1].artist, k_tracks[0].artist) == 0);
    CHECK(d.rows[0].audio == DJ_LINK_PEER_AUDIO_NFS && !d.rows[0].has_detail);
    /* Track rows of a playlist get their metadata as usual. */
    d.want_detail = 0;
    run(&d, detail_or_failed, 3000);
    CHECK(mk.metadata_requests == 1 && mk.metadata_id == k_tracks[2].id);
    dj_link_db_stop(&s_client);
    mock_stop(&mk, th);

    /* Back to all tracks: 0x1004 again, with the sort. */
    dj_link_db_set_menu(&s_client, DJ_LINK_DB_MENU_ALL_TRACKS, 40);
    CHECK(s_client.menu == DJ_LINK_DB_MENU_ALL_TRACKS && s_client.menu_id == 0u);
    dj_link_db_set_menu(NULL, DJ_LINK_DB_MENU_FOLDER, 1);   /* ignored */
}

/* v310: descending lists are stored reversed; the mapping is an involution
 * and leaves out-of-range indices alone. */
static void test_row_index(void)
{
    CHECK(dj_link_db_row_index(5, false, 3) == 3);
    CHECK(dj_link_db_row_index(5, true, 0) == 4);
    CHECK(dj_link_db_row_index(5, true, 4) == 0);
    CHECK(dj_link_db_row_index(5, true, 2) == 2);
    for (uint32_t i = 0; i < 7; i++) {
        CHECK(dj_link_db_row_index(7, true, dj_link_db_row_index(7, true, i)) == i);
    }
    CHECK(dj_link_db_row_index(5, true, 5) == 5);
    CHECK(dj_link_db_row_index(0, true, 0) == 0);
}

/* v310: column taps: new column ascending, same column descending, then the
 * player's own order; the default "column" never reverses. */
static void test_next_sort(void)
{
    bool desc = true;
    CHECK(dj_link_db_next_sort(DJ_LINK_DB_SORT_DEFAULT, false, DJ_LINK_DB_SORT_BPM, &desc) ==
              DJ_LINK_DB_SORT_BPM && !desc);
    CHECK(dj_link_db_next_sort(DJ_LINK_DB_SORT_BPM, false, DJ_LINK_DB_SORT_BPM, &desc) ==
              DJ_LINK_DB_SORT_BPM && desc);
    CHECK(dj_link_db_next_sort(DJ_LINK_DB_SORT_BPM, true, DJ_LINK_DB_SORT_BPM, &desc) ==
              DJ_LINK_DB_SORT_DEFAULT && !desc);
    CHECK(dj_link_db_next_sort(DJ_LINK_DB_SORT_BPM, true, DJ_LINK_DB_SORT_KEY, &desc) ==
              DJ_LINK_DB_SORT_KEY && !desc);
    CHECK(dj_link_db_next_sort(DJ_LINK_DB_SORT_DEFAULT, false, DJ_LINK_DB_SORT_DEFAULT, &desc) ==
              DJ_LINK_DB_SORT_DEFAULT && !desc);
    CHECK(dj_link_db_next_sort(DJ_LINK_DB_SORT_ARTIST, false, DJ_LINK_DB_SORT_TITLE, NULL) ==
              DJ_LINK_DB_SORT_TITLE);
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
    /* vynull's SETUP reply: 2 tag bytes, not 12; then the next message. */
    static const uint8_t vynull_setup[] = {
        0x11, 0x87, 0x23, 0x49, 0xae, 0x11, 0xff, 0xff, 0xff, 0xfe, 0x10, 0x40,
        0x00, 0x0f, 0x02, 0x14, 0x00, 0x00, 0x00, 0x02, 0x06, 0x06, 0x11, 0x00,
        0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x11, 0x11, 0x87,
    };
    CHECK(dj_link_db_msg_size(vynull_setup, sizeof(vynull_setup)) == 32);
    CHECK(dj_link_db_msg_size(vynull_setup, 31) == 0);

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
    test_row_index();
    test_next_sort();
    test_sort_request();
    test_playlist_menus();
    test_anlz_tag_pwv4();
    test_msg_size_and_utf16();
    test_refuse_without_number();
    test_browse_three_tracks();
    test_vynull_artist_from_metadata();
    test_pick_detail();
    test_load_refused_metadata_only();
    test_browse_stamps_nfs();
    test_track_info_path();
    test_analysis_blobs();
    test_artwork();
    test_cues();
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
