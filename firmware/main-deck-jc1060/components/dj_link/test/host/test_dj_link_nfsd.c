/* v342: host tests for the read-only NFS export of our USB key.
 *
 * The server core (dj_link_nfsd.c) and its POSIX backend run against a real
 * temporary directory; the existing NFS client (djlink_nfs, the one that
 * downloads from a CDJ) fetches from it through an in-memory datagram pump,
 * with the names a CDJ uses (UTF-16LE) and with plain bytes. Raw requests built
 * with the client's own builders check what the client never sends: write
 * procedures, hostile names, mangled handles, an export that went away. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dj_link_nfsd.h"
#include "dj_link_nfsd_fs.h"
#include "djlink/nfs.h"
#include "djlink/packet.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

#define PMAP 111u
#define NFSP 2049u
#define CLIENT_A 0xc0a80164u
#define CLIENT_B 0xc0a80165u

static char s_root[64];
static dj_link_nfsd_fs_posix_t s_fs;
static dj_link_nfsd_t s_srv;
static int s_gate_depth, s_gate_total;

static void gate_begin(void) { s_gate_depth++; s_gate_total++; }
static void gate_end(void) { s_gate_depth--; }

/* ---- fixture ------------------------------------------------------------- */

static uint8_t *make_data(size_t n, uint32_t seed)
{
    uint8_t *d = malloc(n);
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        d[i] = (uint8_t)(seed >> 24);
    }
    return d;
}

static void write_file(const char *rel, const uint8_t *d, size_t n)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", s_root, rel);
    FILE *f = fopen(p, "wb");
    if (!f || (n > 0 && fwrite(d, 1, n, f) != n)) {
        printf("fixture write failed: %s\n", p);
        exit(2);
    }
    fclose(f);
}

static void make_dir(const char *rel)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", s_root, rel);
    mkdir(p, 0755);
}

#define PDB_SIZE 50000u
#define MP3_SIZE 300017u
static uint8_t *s_pdb, *s_mp3;
#define MP3_NAME_UTF8 "Czárdás – Ünïcode.mp3"      /* a name that is not ASCII */

static void build_fixture(void)
{
    snprintf(s_root, sizeof(s_root), "/tmp/nfsd_test_XXXXXX");
    if (!mkdtemp(s_root)) {
        printf("mkdtemp failed\n");
        exit(2);
    }
    make_dir("PIONEER");
    make_dir("PIONEER/rekordbox");
    make_dir("Contents");
    make_dir("Contents/Artist");
    make_dir("many");
    s_pdb = make_data(PDB_SIZE, 1);
    s_mp3 = make_data(MP3_SIZE, 2);
    write_file("PIONEER/rekordbox/export.pdb", s_pdb, PDB_SIZE);
    write_file("Contents/Artist/" MP3_NAME_UTF8, s_mp3, MP3_SIZE);
    write_file("Contents/empty.bin", NULL, 0);
    for (int i = 0; i < 120; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "many/track_%03d.mp3", i);
        write_file(rel, (const uint8_t *)"x", 1);
    }
}

static void remove_tree(const char *path)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) {
        printf("cleanup failed\n");
    }
}

static void server_up(void)
{
    dj_link_nfsd_fs_init(&s_fs, s_root, gate_begin, gate_end);
    const dj_link_nfsd_fs_t ops = dj_link_nfsd_fs_ops(&s_fs);
    dj_link_nfsd_init(&s_srv, &ops, NFSP, NFSP, PMAP, NULL);
    dj_link_nfsd_set_available(&s_srv, true);
}

/* ---- the datagram pump: client <-> server -------------------------------- */

typedef struct {
    uint8_t d[DJ_LINK_NFSD_REPLY_MAX];
    size_t n;
} dg_t;

#define QMAX 64
static dg_t s_q[QMAX];
static int s_qn;
static uint32_t s_client_addr = CLIENT_A;
static size_t s_reply_cap = DJ_LINK_NFSD_REPLY_MAX;
static int s_drop_every;             /* drop every Nth reply (lost datagram) */
static int s_reply_count;
static int s_sent_pmap, s_sent_rpc;

static int client_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)ip;
    static uint8_t out[DJ_LINK_NFSD_REPLY_MAX];
    size_t n = 0;
    if (port == PMAP) {
        s_sent_pmap++;
        n = dj_link_nfsd_portmap(&s_srv, buf, len, out, s_reply_cap);
    } else if (port == NFSP) {
        s_sent_rpc++;
        n = dj_link_nfsd_rpc(&s_srv, s_client_addr, buf, len, out, s_reply_cap);
    }
    if (n > 0 && s_qn < QMAX) {
        s_reply_count++;
        if (s_drop_every && s_reply_count % s_drop_every == 0) {
            return 0;
        }
        memcpy(s_q[s_qn].d, out, n);
        s_q[s_qn].n = n;
        s_qn++;
    }
    return 0;
}

typedef struct {
    uint8_t *data;
    size_t cap, got;
    uint32_t opened_size;
    int bad_order;
} sink_t;

static int sink_open(void *ctx, uint32_t size)
{
    sink_t *s = ctx;
    s->opened_size = size;
    return size > s->cap ? 1 : 0;
}

static int sink_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    sink_t *s = ctx;
    if (offset != s->got || offset + len > s->cap) {
        s->bad_order++;
        return 1;
    }
    memcpy(&s->data[offset], data, len);
    s->got += len;
    return 0;
}

static uint8_t s_window[DJLINK_NFS_WINDOW_MAX * DJLINK_NFS_MAXDATA];
static djlink_nfs_t s_client;

static djlink_nfs_state_t fetch(const char *export_path, const char *path, djlink_nfs_charset_t cs,
                                uint32_t read_size, uint8_t window, sink_t *sink)
{
    djlink_nfs_fetch_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host_ip = 0x7f000001u;
    cfg.export_path = export_path;
    cfg.path = path;
    cfg.charset = cs;
    cfg.portmap_port = PMAP;
    cfg.read_size = read_size;
    cfg.window = window;
    cfg.window_buf = s_window;
    cfg.window_buf_len = sizeof(s_window);
    cfg.xid_seed = 7000u;
    djlink_nfs_io_t io = { client_send, sink_open, sink_write, NULL, sink };
    s_qn = 0;
    s_reply_count = 0;
    uint32_t now = 1000u;
    djlink_nfs_fetch(&s_client, &cfg, &io, now);
    for (int iter = 0; iter < 200000; iter++) {
        while (s_qn > 0) {
            dg_t d = s_q[0];
            memmove(&s_q[0], &s_q[1], (size_t)(s_qn - 1) * sizeof(dg_t));
            s_qn--;
            djlink_nfs_on_datagram(&s_client, d.d, d.n, now);
        }
        const djlink_nfs_state_t st = djlink_nfs_state(&s_client);
        if (st != DJLINK_NFS_BUSY) {
            return st;
        }
        now += 50u;                  /* nothing came back: let the retransmit timers run */
        djlink_nfs_poll(&s_client, now);
    }
    return DJLINK_NFS_BUSY;
}

static void test_fetch_like_a_cdj(void)
{
    /* export.pdb with a CDJ's UTF-16LE names, every READ size, windows 1 and 4. */
    static const uint32_t sizes[] = { 512u, 1024u, 2048u, 8192u };
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        for (int w = 1; w <= 4; w += 3) {
            sink_t sink = { calloc(1, PDB_SIZE + 16), PDB_SIZE + 16, 0, 0, 0 };
            const djlink_nfs_state_t st = fetch("/C/", "/PIONEER/rekordbox/export.pdb",
                                                DJLINK_NFS_NAMES_UTF16LE, sizes[k], (uint8_t)w,
                                                &sink);
            CHECK(st == DJLINK_NFS_DONE);
            CHECK(sink.opened_size == PDB_SIZE && sink.got == PDB_SIZE && sink.bad_order == 0);
            CHECK(sink.got == PDB_SIZE && memcmp(sink.data, s_pdb, PDB_SIZE) == 0);
            free(sink.data);
        }
    }
    CHECK(s_srv.reads > 0 && s_srv.bytes_read >= PDB_SIZE);
    CHECK(s_gate_depth == 0 && s_gate_total > 0);          /* the gate wraps every medium access */
}

static void test_fetch_plain_names_and_non_ascii(void)
{
    /* A Linux-style client (UTF-8 bytes) and a name outside ASCII, both charsets. */
    for (int cs = 0; cs < 2; cs++) {
        sink_t sink = { calloc(1, MP3_SIZE + 16), MP3_SIZE + 16, 0, 0, 0 };
        const djlink_nfs_state_t st = fetch(cs ? "/C/" : "/C/", "/Contents/Artist/" MP3_NAME_UTF8,
                                            cs ? DJLINK_NFS_NAMES_BYTES : DJLINK_NFS_NAMES_UTF16LE,
                                            2048u, 4u, &sink);
        CHECK(st == DJLINK_NFS_DONE);
        CHECK(sink.got == MP3_SIZE && memcmp(sink.data, s_mp3, MP3_SIZE) == 0);
        free(sink.data);
    }
    /* A file of 0 bytes, and one that is not there. */
    sink_t sink = { calloc(1, 16), 16, 0, 0, 0 };
    CHECK(fetch("/C/", "/Contents/empty.bin", DJLINK_NFS_NAMES_UTF16LE, 1024u, 1u, &sink) ==
          DJLINK_NFS_DONE);
    CHECK(sink.got == 0u && sink.opened_size == 0u);
    CHECK(fetch("/C/", "/Contents/nope.mp3", DJLINK_NFS_NAMES_UTF16LE, 1024u, 1u, &sink) ==
          DJLINK_NFS_FAILED);
    CHECK(djlink_nfs_error_text(&s_client) != NULL);
    free(sink.data);
}

static void test_fetch_survives_lost_replies(void)
{
    s_drop_every = 5;
    sink_t sink = { calloc(1, PDB_SIZE + 16), PDB_SIZE + 16, 0, 0, 0 };
    CHECK(fetch("/C/", "/PIONEER/rekordbox/export.pdb", DJLINK_NFS_NAMES_UTF16LE, 1024u, 4u,
                &sink) == DJLINK_NFS_DONE);
    CHECK(memcmp(sink.data, s_pdb, PDB_SIZE) == 0 && s_client.retransmits > 0);
    free(sink.data);
    s_drop_every = 0;
}

static void test_fetch_with_a_small_reply_buffer(void)
{
    /* A transmit buffer that cannot hold 8 KB caps the READ to what fits. */
    s_reply_cap = 1400;
    sink_t sink = { calloc(1, PDB_SIZE + 16), PDB_SIZE + 16, 0, 0, 0 };
    CHECK(fetch("/C/", "/PIONEER/rekordbox/export.pdb", DJLINK_NFS_NAMES_BYTES, 8192u, 1u,
                &sink) == DJLINK_NFS_DONE);
    CHECK(sink.got == PDB_SIZE && memcmp(sink.data, s_pdb, PDB_SIZE) == 0);
    free(sink.data);
    s_reply_cap = DJ_LINK_NFSD_REPLY_MAX;
}

/* ---- raw requests -------------------------------------------------------- */

static uint8_t s_tx[1024], s_rx[DJ_LINK_NFSD_REPLY_MAX];

static void wr32(uint8_t *p, size_t *pos, uint32_t v)
{
    djlink_wr32(&p[*pos], v);
    *pos += 4;
}

/* An RPC call header for (prog, vers, proc); returns the length so far. */
static size_t call_head(uint8_t *p, uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc)
{
    size_t n = 0;
    wr32(p, &n, xid);
    wr32(p, &n, 0u);                 /* CALL */
    wr32(p, &n, 2u);                 /* RPC v2 */
    wr32(p, &n, prog);
    wr32(p, &n, vers);
    wr32(p, &n, proc);
    wr32(p, &n, 0u);                 /* AUTH_NONE */
    wr32(p, &n, 0u);
    wr32(p, &n, 0u);                 /* verifier */
    wr32(p, &n, 0u);
    return n;
}

static void put_opaque(uint8_t *p, size_t *n, const uint8_t *d, size_t len)
{
    wr32(p, n, (uint32_t)len);
    memcpy(&p[*n], d, len);
    memset(&p[*n + len], 0, ((len + 3u) & ~3u) - len);
    *n += (len + 3u) & ~3u;
}

/* The reply: {status word index} helpers. Offsets: 24 = first result word. */
typedef struct {
    size_t n;
    uint32_t accept;
    uint32_t status;
} reply_t;

static reply_t rpc(uint32_t client, const uint8_t *req, size_t len)
{
    reply_t r = { 0, 99u, 99u };
    r.n = dj_link_nfsd_rpc(&s_srv, client, req, len, s_rx, sizeof(s_rx));
    if (r.n >= 24u) {
        r.accept = djlink_rd32(&s_rx[20]);
    }
    if (r.n >= 28u) {
        r.status = djlink_rd32(&s_rx[24]);
    }
    return r;
}

static void mnt(const char *path, bool utf16, uint8_t fh[32], uint32_t *status)
{
    size_t n = call_head(s_tx, 1u, DJLINK_MOUNT_PROG, 1u, 1u);
    uint8_t enc[64];
    size_t el = strlen(path);
    if (utf16) {
        for (size_t i = 0; i < el; i++) {
            enc[i * 2] = (uint8_t)path[i];
            enc[i * 2 + 1] = 0;
        }
        el *= 2;
        put_opaque(s_tx, &n, enc, el);
    } else {
        put_opaque(s_tx, &n, (const uint8_t *)path, el);
    }
    reply_t r = rpc(CLIENT_A, s_tx, n);
    *status = r.status;
    if (r.status == 0u) {
        CHECK(r.n == 24u + 4u + 32u);
        memcpy(fh, &s_rx[28], 32);
    }
}

static reply_t lookup(uint32_t client, const uint8_t fh[32], const uint8_t *name, size_t len,
                      uint8_t out_fh[32])
{
    size_t n = call_head(s_tx, 2u, DJLINK_NFS_PROG, 2u, 4u);
    memcpy(&s_tx[n], fh, 32);
    n += 32;
    put_opaque(s_tx, &n, name, len);
    reply_t r = rpc(client, s_tx, n);
    if (r.status == 0u && out_fh) {
        memcpy(out_fh, &s_rx[28], 32);
    }
    return r;
}

static reply_t lookup_str(const uint8_t fh[32], const char *name, uint8_t out_fh[32])
{
    return lookup(CLIENT_A, fh, (const uint8_t *)name, strlen(name), out_fh);
}

static void test_mount_and_portmap(void)
{
    uint8_t fh[32];
    uint32_t st;
    /* The export: "/C/" in either charset, "C", "" ; the SD slot is not served. */
    mnt("/C/", true, fh, &st);
    CHECK(st == 0u);
    static const uint8_t zero[32];
    CHECK(memcmp(fh, zero, 32) == 0);                      /* all-zero root, like a rekordbox */
    mnt("/C/", false, fh, &st);
    CHECK(st == 0u);
    mnt("C", false, fh, &st);
    CHECK(st == 0u);
    mnt("", false, fh, &st);
    CHECK(st == 0u);
    mnt("/B/", true, fh, &st);
    CHECK(st == DJ_LINK_NFSERR_NOENT);
    mnt("/D/", false, fh, &st);
    CHECK(st == DJ_LINK_NFSERR_NOENT);
    mnt("/Cx/", false, fh, &st);
    CHECK(st == DJ_LINK_NFSERR_NOENT);

    /* GETPORT. */
    size_t n = call_head(s_tx, 3u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, DJLINK_MOUNT_PROG);
    wr32(s_tx, &n, 1u);
    wr32(s_tx, &n, 17u);
    wr32(s_tx, &n, 0u);
    size_t rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == NFSP);
    n = call_head(s_tx, 4u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, DJLINK_NFS_PROG);
    wr32(s_tx, &n, 2u);
    wr32(s_tx, &n, 17u);
    wr32(s_tx, &n, 0u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == NFSP);
    /* TCP, an unknown program and a wrong version: port 0. */
    n = call_head(s_tx, 5u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, DJLINK_NFS_PROG); wr32(s_tx, &n, 2u); wr32(s_tx, &n, 6u); wr32(s_tx, &n, 0u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == 0u);
    n = call_head(s_tx, 6u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, 123456u); wr32(s_tx, &n, 1u); wr32(s_tx, &n, 17u); wr32(s_tx, &n, 0u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == 0u);
    n = call_head(s_tx, 7u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, DJLINK_NFS_PROG); wr32(s_tx, &n, 3u); wr32(s_tx, &n, 17u); wr32(s_tx, &n, 0u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == 0u);
    /* The portmapper's own port, the NULL procedure, a bad procedure, short args. */
    n = call_head(s_tx, 8u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, DJLINK_PMAP_PROG); wr32(s_tx, &n, 2u); wr32(s_tx, &n, 17u); wr32(s_tx, &n, 0u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == PMAP);
    n = call_head(s_tx, 9u, DJLINK_PMAP_PROG, 2u, 0u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 24u && djlink_rd32(&s_rx[20]) == DJLINK_RPC_SUCCESS);
    n = call_head(s_tx, 10u, DJLINK_PMAP_PROG, 2u, 4u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 24u && djlink_rd32(&s_rx[20]) == DJLINK_RPC_PROC_UNAVAIL);
    n = call_head(s_tx, 11u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, 1u);
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 24u && djlink_rd32(&s_rx[20]) == DJLINK_RPC_GARBAGE_ARGS);
    n = call_head(s_tx, 12u, 100003u, 2u, 3u);               /* not the portmapper's program */
    rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 24u && djlink_rd32(&s_rx[20]) == DJLINK_RPC_PROG_UNAVAIL);

    /* Not an RPC call at all: no reply. */
    CHECK(dj_link_nfsd_portmap(&s_srv, (const uint8_t *)"hello", 5, s_rx, sizeof(s_rx)) == 0u);
    CHECK(dj_link_nfsd_rpc(&s_srv, CLIENT_A, (const uint8_t *)"hello", 5, s_rx, sizeof(s_rx)) == 0u);
    CHECK(dj_link_nfsd_rpc(&s_srv, CLIENT_A, s_tx, 3, s_rx, sizeof(s_rx)) == 0u);
    CHECK(dj_link_nfsd_rpc(&s_srv, CLIENT_A, s_tx, DJ_LINK_NFSD_RPC_MAX + 1u, s_rx, sizeof(s_rx)) == 0u);
}

static void test_export_list(void)
{
    size_t n = call_head(s_tx, 20u, DJLINK_MOUNT_PROG, 1u, 5u);
    reply_t r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == 0u && r.status == 1u);                 /* one export follows */
    CHECK(djlink_rd32(&s_rx[28]) == 3u && memcmp(&s_rx[32], "/C/", 3) == 0);
    CHECK(djlink_rd32(&s_rx[36]) == 1u);                     /* one group follows */
    CHECK(djlink_rd32(&s_rx[40]) == 15u && memcmp(&s_rx[44], "0.0.0.0/0.0.0.0", 15) == 0);
    CHECK(djlink_rd32(&s_rx[60]) == 0u && djlink_rd32(&s_rx[64]) == 0u && r.n == 68u);

    /* Wrong MOUNT version: a program mismatch that says which one we speak. */
    n = call_head(s_tx, 21u, DJLINK_MOUNT_PROG, 3u, 1u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == 2u && r.n == 32u && djlink_rd32(&s_rx[24]) == 1u && djlink_rd32(&s_rx[28]) == 1u);
}

static uint32_t attr_type(void) { return djlink_rd32(&s_rx[28 + 32 + 0]); }

static void test_lookup_getattr_and_names(void)
{
    uint8_t root[32] = { 0 };
    uint8_t fh[32], pio[32], cont[32];
    reply_t r = lookup_str(root, "PIONEER", pio);
    CHECK(r.status == 0u && r.n == 24u + 4u + 32u + 68u);
    CHECK(attr_type() == DJLINK_NFS_TYPE_DIR);
    r = lookup_str(pio, "rekordbox", fh);
    CHECK(r.status == 0u);
    r = lookup_str(fh, "export.pdb", fh);
    CHECK(r.status == 0u && attr_type() == DJLINK_NFS_TYPE_REG);
    CHECK(djlink_rd32(&s_rx[28 + 32 + 20]) == PDB_SIZE);

    /* GETATTR of that handle, and of the root. */
    size_t n = call_head(s_tx, 3u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], fh, 32);
    n += 32;
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == 0u && r.n == 24u + 4u + 68u && djlink_rd32(&s_rx[28]) == DJLINK_NFS_TYPE_REG &&
          djlink_rd32(&s_rx[28 + 20]) == PDB_SIZE);
    n = call_head(s_tx, 4u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], root, 32);
    n += 32;
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[28]) == DJLINK_NFS_TYPE_DIR);

    /* A CDJ rewrites bytes 12..31 of a handle: the first 12 still find the file. */
    uint8_t mangled[32];
    memcpy(mangled, fh, 32);
    for (int i = 12; i < 32; i++) {
        mangled[i] = (uint8_t)(0xA0 + i);
    }
    n = call_head(s_tx, 5u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], mangled, 32);
    n += 32;
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[28 + 20]) == PDB_SIZE);
    /* ...but a changed generation or hash is stale. */
    mangled[5] ^= 1;
    n = call_head(s_tx, 6u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], mangled, 32);
    n += 32;
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == DJ_LINK_NFSERR_STALE);

    /* A name that is not a plain element never leaves the export. */
    CHECK(lookup_str(root, "..", NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup_str(root, ".", NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup_str(pio, "../Contents", NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup_str(root, "PIONEER/rekordbox", NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup_str(root, "PIONEER\\rekordbox", NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup_str(root, "/etc/passwd", NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup(CLIENT_A, root, (const uint8_t *)"", 0, NULL).status == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup(CLIENT_A, root, (const uint8_t *)"a\0b", 3, NULL).status != 0u);
    /* The same in UTF-16LE. */
    static const uint8_t dotdot16[] = { '.', 0, '.', 0 };
    CHECK(lookup(CLIENT_A, root, dotdot16, sizeof(dotdot16), NULL).status == DJ_LINK_NFSERR_NOENT);
    static const uint8_t sep16[] = { 'a', 0, '/', 0, 'b', 0 };
    CHECK(lookup(CLIENT_A, root, sep16, sizeof(sep16), NULL).status == DJ_LINK_NFSERR_NOENT);
    /* A lone surrogate is not a name. */
    static const uint8_t bad16[] = { 0x00, 0xD8, 'a', 0 };
    CHECK(lookup(CLIENT_A, root, bad16, sizeof(bad16), NULL).status != 0u);
    /* Too long. */
    uint8_t longname[400];
    memset(longname, 'x', sizeof(longname));
    CHECK(lookup(CLIENT_A, root, longname, 300, NULL).status == DJ_LINK_NFSERR_NAMETOOLONG);
    /* Not a directory; missing. */
    uint8_t file[32];
    CHECK(lookup_str(root, "Contents", cont).status == 0u);
    CHECK(lookup_str(cont, "empty.bin", file).status == 0u);
    CHECK(lookup_str(file, "x", NULL).status == DJ_LINK_NFSERR_NOTDIR);
    CHECK(lookup_str(root, "NOPE", NULL).status == DJ_LINK_NFSERR_NOENT);
    /* Garbage handle. */
    uint8_t junk[32];
    memset(junk, 0xEE, sizeof(junk));
    CHECK(lookup_str(junk, "PIONEER", NULL).status == DJ_LINK_NFSERR_STALE);
    /* A short request is garbage args. */
    n = call_head(s_tx, 7u, DJLINK_NFS_PROG, 2u, 4u);
    memcpy(&s_tx[n], root, 16);
    r = rpc(CLIENT_A, s_tx, n + 16);
    CHECK(r.accept == DJLINK_RPC_GARBAGE_ARGS && r.n == 24u);
}

static void test_read_edges(void)
{
    uint8_t root[32] = { 0 }, cont[32], pdb[32], fh[32], dir[32];
    CHECK(lookup_str(root, "PIONEER", dir).status == 0u);
    CHECK(lookup_str(dir, "rekordbox", dir).status == 0u);
    CHECK(lookup_str(dir, "export.pdb", pdb).status == 0u);
    CHECK(lookup_str(root, "Contents", cont).status == 0u);
    (void)fh;

#define READ_REQ(handle, off, cnt)                                         \
    do {                                                                    \
        size_t rn_ = call_head(s_tx, 9u, DJLINK_NFS_PROG, 2u, 6u);          \
        memcpy(&s_tx[rn_], (handle), 32);                                   \
        rn_ += 32;                                                          \
        wr32(s_tx, &rn_, (off));                                            \
        wr32(s_tx, &rn_, (cnt));                                            \
        wr32(s_tx, &rn_, 0u);                                               \
        r = rpc(CLIENT_A, s_tx, rn_);                                       \
    } while (0)
    reply_t r;
    /* Past the end: no data, still a success with attributes. */
    READ_REQ(pdb, PDB_SIZE, 1024u);
    CHECK(r.status == 0u && r.n == 24u + 4u + 68u + 4u && djlink_rd32(&s_rx[24 + 4 + 68]) == 0u);
    READ_REQ(pdb, 0xfffffff0u, 1024u);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[24 + 4 + 68]) == 0u);
    /* The last partial block. */
    READ_REQ(pdb, PDB_SIZE - 10u, 4096u);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[24 + 4 + 68]) == 10u && r.n == 24u + 4u + 68u + 4u + 12u);
    CHECK(memcmp(&s_rx[24 + 4 + 68 + 4], &s_pdb[PDB_SIZE - 10u], 10) == 0);
    /* An absurd count is capped at 8192. */
    READ_REQ(pdb, 0u, 0xffffffffu);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[24 + 4 + 68]) == DJLINK_NFS_MAXDATA);
    CHECK(memcmp(&s_rx[24 + 4 + 68 + 4], s_pdb, DJLINK_NFS_MAXDATA) == 0);
    /* Zero bytes asked, zero given. */
    READ_REQ(pdb, 0u, 0u);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[24 + 4 + 68]) == 0u);
    /* An unaligned length still pads to 4 and gives the bytes. */
    READ_REQ(pdb, 3u, 7u);
    CHECK(r.status == 0u && djlink_rd32(&s_rx[24 + 4 + 68]) == 7u && r.n == 24u + 4u + 68u + 4u + 8u);
    CHECK(memcmp(&s_rx[24 + 4 + 68 + 4], &s_pdb[3], 7) == 0 && s_rx[r.n - 1] == 0u);
    /* A directory is not read. */
    READ_REQ(cont, 0u, 1024u);
    CHECK(r.status == DJ_LINK_NFSERR_ISDIR && r.n == 28u);
    /* Short arguments. */
    size_t n = call_head(s_tx, 10u, DJLINK_NFS_PROG, 2u, 6u);
    memcpy(&s_tx[n], pdb, 32);
    n += 32;
    wr32(s_tx, &n, 0u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == DJLINK_RPC_GARBAGE_ARGS);
}

static size_t readdir_all(const uint8_t dir[32], uint32_t client, uint32_t count_budget, char names[200][64],
                          int *calls, bool *saw_utf16)
{
    size_t total = 0;
    uint32_t cookie = 0;
    *calls = 0;
    for (;;) {
        size_t n = call_head(s_tx, 30u, DJLINK_NFS_PROG, 2u, 16u);
        memcpy(&s_tx[n], dir, 32);
        n += 32;
        wr32(s_tx, &n, cookie);
        wr32(s_tx, &n, count_budget);
        reply_t r = rpc(client, s_tx, n);
        (*calls)++;
        if (r.status != 0u) {
            return (size_t)-1;
        }
        size_t pos = 28;
        for (;;) {
            const uint32_t follows = djlink_rd32(&s_rx[pos]);
            pos += 4;
            if (!follows) {
                break;
            }
            pos += 4;                                       /* fileid */
            const uint32_t nl = djlink_rd32(&s_rx[pos]);
            pos += 4;
            if (nl >= 2u && s_rx[pos + 1] == 0u) {
                *saw_utf16 = true;
                for (uint32_t i = 0; i + 1 < nl && i / 2 < 63; i += 2) {
                    names[total][i / 2] = (char)s_rx[pos + i];
                }
                names[total][nl / 2] = '\0';
            } else {
                memcpy(names[total], &s_rx[pos], nl < 63 ? nl : 63);
                names[total][nl < 63 ? nl : 63] = '\0';
            }
            pos += (nl + 3u) & ~3u;
            cookie = djlink_rd32(&s_rx[pos]);
            pos += 4;
            total++;
        }
        const uint32_t eof = djlink_rd32(&s_rx[pos]);
        if (eof) {
            return total;
        }
        if (*calls > 100) {
            return (size_t)-2;
        }
    }
}

static void test_readdir(void)
{
    uint8_t root[32] = { 0 }, many[32];
    CHECK(lookup_str(root, "many", many).status == 0u);
    static char names[200][64];
    int calls = 0;
    bool u16 = false;
    /* 120 entries, a 1 KB budget: several calls, every name once, in order. */
    size_t n = readdir_all(many, CLIENT_B, 1024u, names, &calls, &u16);
    CHECK(n == 120u && calls == 4 && !u16);        /* 32 bytes an entry, 1 KB a call */
    int seen[120] = { 0 };
    for (size_t i = 0; i < n; i++) {
        int k = -1;
        if (sscanf(names[i], "track_%d.mp3", &k) == 1 && k >= 0 && k < 120) {
            seen[k]++;
        }
    }
    int missing = 0, dup = 0;
    for (int i = 0; i < 120; i++) {
        missing += seen[i] == 0;
        dup += seen[i] > 1;
    }
    CHECK(missing == 0 && dup == 0);
    /* A big budget: one call. */
    n = readdir_all(many, CLIENT_B, 8000u, names, &calls, &u16);
    CHECK(n == 120u && calls <= 2);
    /* The root, with the export's own entries (no "." / ".."). */
    n = readdir_all(root, CLIENT_B, 8000u, names, &calls, &u16);
    CHECK(n == 3u);
    bool has_pio = false;
    for (size_t i = 0; i < n; i++) {
        has_pio |= strcmp(names[i], "PIONEER") == 0;
        CHECK(strcmp(names[i], ".") != 0 && strcmp(names[i], "..") != 0);
    }
    CHECK(has_pio);
    /* A client that spoke UTF-16LE (a CDJ's LOOKUP) gets UTF-16LE names; another one still bytes. */
    static const uint8_t many16[] = { 'm', 0, 'a', 0, 'n', 0, 'y', 0 };
    CHECK(lookup(CLIENT_A, root, many16, sizeof(many16), many).status == 0u);
    u16 = false;
    n = readdir_all(many, CLIENT_A, 8000u, names, &calls, &u16);
    CHECK(n == 120u && u16);
    u16 = false;
    n = readdir_all(many, CLIENT_B, 8000u, names, &calls, &u16);
    CHECK(n == 120u && !u16);
    /* A file is not a directory. */
    uint8_t cont[32], file[32];
    CHECK(lookup_str(root, "Contents", cont).status == 0u);
    CHECK(lookup_str(cont, "empty.bin", file).status == 0u);
    CHECK(readdir_all(file, CLIENT_B, 8000u, names, &calls, &u16) == (size_t)-1);
    CHECK(s_rx[0] == s_rx[0]);
}

static void test_write_procedures_refused(void)
{
    uint8_t root[32] = { 0 };
    static const uint32_t procs[] = { 2u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u };
    const uint32_t before = s_srv.refused_writes;
    for (size_t i = 0; i < sizeof(procs) / sizeof(procs[0]); i++) {
        size_t n = call_head(s_tx, 40u + (uint32_t)i, DJLINK_NFS_PROG, 2u, procs[i]);
        memcpy(&s_tx[n], root, 32);
        n += 32;
        put_opaque(s_tx, &n, (const uint8_t *)"evil", 4);
        reply_t r = rpc(CLIENT_A, s_tx, n);
        CHECK(r.accept == 0u && r.status == DJ_LINK_NFSERR_ROFS && r.n == 28u);
    }
    CHECK(s_srv.refused_writes == before + 9u);
    /* Nothing was created. */
    char p[128];
    snprintf(p, sizeof(p), "%s/evil", s_root);
    CHECK(access(p, F_OK) != 0);
    /* STATFS, NULL, READLINK, an unknown procedure, an unknown program. */
    size_t n = call_head(s_tx, 60u, DJLINK_NFS_PROG, 2u, 17u);
    memcpy(&s_tx[n], root, 32);
    n += 32;
    reply_t r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == 0u && r.n == 24u + 24u && djlink_rd32(&s_rx[28]) == DJLINK_NFS_MAXDATA &&
          djlink_rd32(&s_rx[40]) == 0u);                       /* nothing free on a read-only key */
    n = call_head(s_tx, 61u, DJLINK_NFS_PROG, 2u, 0u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.n == 24u && r.accept == 0u);
    n = call_head(s_tx, 62u, DJLINK_NFS_PROG, 2u, 5u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == DJ_LINK_NFSERR_INVAL);
    n = call_head(s_tx, 63u, DJLINK_NFS_PROG, 2u, 99u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == DJLINK_RPC_PROC_UNAVAIL);
    n = call_head(s_tx, 64u, DJLINK_NFS_PROG, 3u, 0u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == 2u && djlink_rd32(&s_rx[24]) == 2u);
    n = call_head(s_tx, 65u, 777777u, 1u, 0u);
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == DJLINK_RPC_PROG_UNAVAIL);
}

static void test_handle_recycling(void)
{
    uint8_t root[32] = { 0 }, many[32], first[32], h[32];
    CHECK(lookup_str(root, "many", many).status == 0u);
    CHECK(lookup_str(many, "track_000.mp3", first).status == 0u);
    /* The same path gets the same handle, and keeps working. */
    CHECK(lookup_str(many, "track_000.mp3", h).status == 0u && memcmp(h, first, 32) == 0);
    /* Look up more files than there are handles: the first one goes stale. */
    for (int i = 1; i < 100; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "track_%03d.mp3", i);
        CHECK(lookup_str(many, nm, h).status == 0u);
    }
    size_t n = call_head(s_tx, 70u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], first, 32);
    n += 32;
    reply_t r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == DJ_LINK_NFSERR_STALE && s_srv.stale > 0u);
    /* A handle used recently survives the churn; looking it up again works. */
    CHECK(lookup_str(many, "track_000.mp3", h).status == 0u);
    n = call_head(s_tx, 71u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], h, 32);
    n += 32;
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == 0u);
}

static void test_unavailable_and_back(void)
{
    uint8_t root[32] = { 0 }, pio[32], fh[32];
    uint32_t st;
    CHECK(lookup_str(root, "PIONEER", pio).status == 0u);
    dj_link_nfsd_set_available(&s_srv, false);
    mnt("/C/", true, fh, &st);
    CHECK(st == DJ_LINK_NFSERR_NOENT);
    CHECK(lookup_str(root, "PIONEER", NULL).status == DJ_LINK_NFSERR_STALE);
    size_t n = call_head(s_tx, 80u, DJLINK_MOUNT_PROG, 1u, 5u);
    reply_t r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.accept == 0u && r.status == 0u && r.n == 28u);   /* an empty export list: unlinked */
    n = call_head(s_tx, 81u, DJLINK_PMAP_PROG, 2u, 3u);
    wr32(s_tx, &n, DJLINK_NFS_PROG); wr32(s_tx, &n, 2u); wr32(s_tx, &n, 17u); wr32(s_tx, &n, 0u);
    size_t rn = dj_link_nfsd_portmap(&s_srv, s_tx, n, s_rx, sizeof(s_rx));
    CHECK(rn == 28u && djlink_rd32(&s_rx[24]) == 0u);
    /* Back: the handles of before are gone, a fresh mount works. */
    dj_link_nfsd_set_available(&s_srv, true);
    n = call_head(s_tx, 82u, DJLINK_NFS_PROG, 2u, 1u);
    memcpy(&s_tx[n], pio, 32);
    n += 32;
    r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == DJ_LINK_NFSERR_STALE);
    mnt("/C/", true, fh, &st);
    CHECK(st == 0u);
    CHECK(lookup_str(fh, "PIONEER", NULL).status == 0u);
}

static void test_backend_idle_close_and_missing_root(void)
{
    /* The cached file and directory close when idle, and a vanished file is an error, not a crash. */
    uint8_t root[32] = { 0 }, cont[32], mp3[32], art[32];
    CHECK(lookup_str(root, "Contents", cont).status == 0u);
    CHECK(lookup_str(cont, "Artist", art).status == 0u);
    CHECK(lookup(CLIENT_A, art, (const uint8_t *)MP3_NAME_UTF8, strlen(MP3_NAME_UTF8), mp3).status == 0u);
    size_t n = call_head(s_tx, 90u, DJLINK_NFS_PROG, 2u, 6u);
    memcpy(&s_tx[n], mp3, 32);
    n += 32;
    wr32(s_tx, &n, 0u); wr32(s_tx, &n, 1024u); wr32(s_tx, &n, 0u);
    reply_t r = rpc(CLIENT_A, s_tx, n);
    CHECK(r.status == 0u && s_fs.fp != NULL);
    dj_link_nfsd_fs_touch(&s_fs, 1000u);
    dj_link_nfsd_fs_idle(&s_fs, 1500u, 2000u);
    CHECK(s_fs.fp != NULL);                                  /* not idle long enough */
    dj_link_nfsd_fs_idle(&s_fs, 3000u, 2000u);
    CHECK(s_fs.fp == NULL && s_fs.dp == NULL);
    CHECK(s_gate_depth == 0);

    char p[256];
    snprintf(p, sizeof(p), "%s/Contents/Artist/" MP3_NAME_UTF8, s_root);
    unlink(p);
    r = rpc(CLIENT_A, s_tx, n);                              /* the handle is valid, the file is gone */
    CHECK(r.status == DJ_LINK_NFSERR_NOENT);
    write_file("Contents/Artist/" MP3_NAME_UTF8, s_mp3, MP3_SIZE);
}

int main(void)
{
    build_fixture();
    server_up();
    test_mount_and_portmap();
    test_export_list();
    test_fetch_like_a_cdj();
    test_fetch_plain_names_and_non_ascii();
    test_fetch_survives_lost_replies();
    test_fetch_with_a_small_reply_buffer();
    test_lookup_getattr_and_names();
    test_read_edges();
    test_readdir();
    test_write_procedures_refused();
    test_handle_recycling();
    test_unavailable_and_back();
    test_backend_idle_close_and_missing_root();
    dj_link_nfsd_fs_close(&s_fs);
    remove_tree(s_root);
    free(s_pdb);
    free(s_mp3);
    if (s_failures) {
        printf("%d dj_link_nfsd check(s) failed\n", s_failures);
        return 1;
    }
    printf("all dj_link_nfsd tests passed\n");
    return 0;
}
