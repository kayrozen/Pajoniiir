#include "dj_link_nfsd.h"

#include <string.h>

#include "djlink/nfs.h"
#include "djlink/packet.h"

/* ---- programs and procedures -------------------------------------------- */

#define PMAP_PROC_NULL     0u
#define PMAP_PROC_GETPORT  3u
#define MOUNT_PROC_NULL    0u
#define MOUNT_PROC_MNT     1u
#define MOUNT_PROC_UMNT    3u
#define MOUNT_PROC_UMNTALL 4u
#define MOUNT_PROC_EXPORT  5u
#define NFS_PROC_NULL      0u
#define NFS_PROC_GETATTR   1u
#define NFS_PROC_SETATTR   2u
#define NFS_PROC_ROOT      3u
#define NFS_PROC_LOOKUP    4u
#define NFS_PROC_READLINK  5u
#define NFS_PROC_READ      6u
#define NFS_PROC_WRITECACHE 7u
#define NFS_PROC_WRITE     8u
#define NFS_PROC_CREATE    9u
#define NFS_PROC_REMOVE    10u
#define NFS_PROC_RENAME    11u
#define NFS_PROC_LINK      12u
#define NFS_PROC_SYMLINK   13u
#define NFS_PROC_MKDIR     14u
#define NFS_PROC_RMDIR     15u
#define NFS_PROC_READDIR   16u
#define NFS_PROC_STATFS    17u

#define RPC_PROG_MISMATCH  2u

#define FH 32u

/* ---- XDR helpers --------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    bool err;
} wr_t;

static void w32(wr_t *w, uint32_t v)
{
    if (w->err || w->cap - w->pos < 4u) {
        w->err = true;
        return;
    }
    djlink_wr32(&w->buf[w->pos], v);
    w->pos += 4u;
}

static void wbytes(wr_t *w, const uint8_t *p, size_t n)
{
    const size_t padded = (n + 3u) & ~(size_t)3u;
    if (w->err || w->cap - w->pos < padded) {
        w->err = true;
        return;
    }
    memcpy(&w->buf[w->pos], p, n);
    memset(&w->buf[w->pos + n], 0, padded - n);
    w->pos += padded;
}

static void wstring(wr_t *w, const uint8_t *p, size_t n)
{
    w32(w, (uint32_t)n);
    wbytes(w, p, n);
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    bool err;
} rd_t;

static uint32_t r32(rd_t *r)
{
    if (r->err || r->len - r->pos < 4u) {
        r->err = true;
        return 0u;
    }
    const uint32_t v = djlink_rd32(&r->buf[r->pos]);
    r->pos += 4u;
    return v;
}

/* A variable-length opaque, `max` bytes at most. */
static const uint8_t *ropaque(rd_t *r, size_t max, size_t *n)
{
    const uint32_t len = r32(r);
    const size_t padded = ((size_t)len + 3u) & ~(size_t)3u;
    if (r->err || len > max || r->len - r->pos < padded) {
        r->err = true;
        *n = 0u;
        return NULL;
    }
    const uint8_t *p = &r->buf[r->pos];
    r->pos += padded;
    *n = len;
    return p;
}

static const uint8_t *rfh(rd_t *r)
{
    if (r->err || r->len - r->pos < FH) {
        r->err = true;
        return NULL;
    }
    const uint8_t *p = &r->buf[r->pos];
    r->pos += FH;
    return p;
}

/* ---- names -------------------------------------------------------------- */

/* A CDJ names every element in UTF-16LE: an even number of bytes whose odd
 * ones are all 0 for the characters of a file system that is Latin-1 / ASCII.
 * (A UTF-8 name never holds a NUL.) Names outside the BMP's low plane carry a
 * non-zero high byte, so they are recognised by their own surrogates too. */
static bool looks_utf16le(const uint8_t *p, size_t n)
{
    if (n < 2u || (n & 1u) != 0u) {
        return false;
    }
    bool high_zero = true;
    for (size_t i = 1; i < n; i += 2u) {
        if (p[i] != 0u) {
            high_zero = false;
            break;
        }
    }
    if (high_zero) {
        return true;
    }
    /* Non-Latin names: no NUL in a UTF-8 name, so any NUL byte means UTF-16. */
    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0u) {
            return true;
        }
    }
    return false;
}

/* UTF-16LE -> UTF-8. Returns the length, or -1 (bad surrogate pair, no room, NUL). */
static int utf16le_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i + 1u < n; i += 2u) {
        uint32_t c = (uint32_t)in[i] | ((uint32_t)in[i + 1u] << 8);
        if (c >= 0xD800u && c <= 0xDBFFu) {
            if (i + 3u >= n) {
                return -1;
            }
            const uint32_t lo = (uint32_t)in[i + 2u] | ((uint32_t)in[i + 3u] << 8);
            if (lo < 0xDC00u || lo > 0xDFFFu) {
                return -1;
            }
            c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
            i += 2u;
        } else if (c >= 0xDC00u && c <= 0xDFFFu) {
            return -1;
        }
        if (c == 0u) {
            return -1;
        }
        const size_t need = c < 0x80u ? 1u : c < 0x800u ? 2u : c < 0x10000u ? 3u : 4u;
        if (cap - o < need + 1u) {
            return -1;
        }
        if (need == 1u) {
            out[o++] = (char)c;
        } else if (need == 2u) {
            out[o++] = (char)(0xC0u | (c >> 6));
            out[o++] = (char)(0x80u | (c & 0x3Fu));
        } else if (need == 3u) {
            out[o++] = (char)(0xE0u | (c >> 12));
            out[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            out[o++] = (char)(0xF0u | (c >> 18));
            out[o++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
            out[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* UTF-8 -> UTF-16LE. Returns the byte count, or -1. */
static int utf8_to_utf16le(const char *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        const uint8_t b = (uint8_t)in[i];
        uint32_t c;
        size_t take;
        if (b < 0x80u) {
            c = b; take = 1u;
        } else if ((b & 0xE0u) == 0xC0u) {
            c = b & 0x1Fu; take = 2u;
        } else if ((b & 0xF0u) == 0xE0u) {
            c = b & 0x0Fu; take = 3u;
        } else if ((b & 0xF8u) == 0xF0u) {
            c = b & 0x07u; take = 4u;
        } else {
            return -1;
        }
        if (n - i < take) {
            return -1;
        }
        for (size_t k = 1; k < take; k++) {
            if (((uint8_t)in[i + k] & 0xC0u) != 0x80u) {
                return -1;
            }
            c = (c << 6) | ((uint8_t)in[i + k] & 0x3Fu);
        }
        i += take;
        const size_t units = c >= 0x10000u ? 2u : 1u;
        if (cap - o < units * 2u) {
            return -1;
        }
        if (units == 1u) {
            out[o++] = (uint8_t)c;
            out[o++] = (uint8_t)(c >> 8);
        } else {
            c -= 0x10000u;
            const uint32_t hi = 0xD800u + (c >> 10);
            const uint32_t lo = 0xDC00u + (c & 0x3FFu);
            out[o++] = (uint8_t)hi;
            out[o++] = (uint8_t)(hi >> 8);
            out[o++] = (uint8_t)lo;
            out[o++] = (uint8_t)(lo >> 8);
        }
    }
    return (int)o;
}

/* The name a client sent as UTF-8; *utf16 says how it was written. */
static int decode_name(const uint8_t *raw, size_t n, char *out, size_t cap, bool *utf16)
{
    *utf16 = looks_utf16le(raw, n);
    if (*utf16) {
        return utf16le_to_utf8(raw, n, out, cap);
    }
    if (n + 1u > cap) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == 0u) {
            return -1;
        }
    }
    memcpy(out, raw, n);
    out[n] = '\0';
    return (int)n;
}

/* One plain path element: not empty, not "." or "..", no separator. */
static bool plain_element(const char *s, size_t n)
{
    if (n == 0u || (n == 1u && s[0] == '.') || (n == 2u && s[0] == '.' && s[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '/' || s[i] == '\\') {
            return false;
        }
    }
    return true;
}

/* ---- handles and clients ------------------------------------------------- */

static uint32_t fnv32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return h;
}

void dj_link_nfsd_init(dj_link_nfsd_t *s, const dj_link_nfsd_fs_t *fs, uint16_t mount_port,
                       uint16_t nfs_port, uint16_t pmap_port, const char *group)
{
    memset(s, 0, sizeof(*s));
    if (fs) {
        s->fs = *fs;
    }
    s->mount_port = mount_port;
    s->nfs_port = nfs_port;
    s->pmap_port = pmap_port ? pmap_port : DJ_LINK_NFSD_PMAP_PORT;
    strncpy(s->export_group, group ? group : "0.0.0.0/0.0.0.0", sizeof(s->export_group) - 1u);
    s->next_gen = 1u;
}

void dj_link_nfsd_set_available(dj_link_nfsd_t *s, bool available)
{
    if (s->available != available) {
        memset(s->handle, 0, sizeof(s->handle));
    }
    s->available = available;
}

static void put_handle(const dj_link_nfsd_handle_t *table, uint32_t slot, uint8_t out[FH])
{
    memset(out, 0, FH);
    djlink_wr32(&out[0], slot + 1u);
    djlink_wr32(&out[4], table[slot].gen);
    djlink_wr32(&out[8], table[slot].hash);
}

/* The handle of `path`; "" is the root, all zero (what a rekordbox answers).
 * A new path takes a free slot, else the least recently used one: its old
 * handle then answers STALE and the client looks the file up again. */
static void handle_for(dj_link_nfsd_t *s, const char *path, uint8_t out[FH])
{
    if (path[0] == '\0') {
        memset(out, 0, FH);
        return;
    }
    const uint32_t h = fnv32(path);
    uint32_t free_slot = DJ_LINK_NFSD_HANDLES;
    uint32_t lru = 0u;
    s->clock++;
    for (uint32_t i = 0; i < DJ_LINK_NFSD_HANDLES; i++) {
        dj_link_nfsd_handle_t *e = &s->handle[i];
        if (e->gen == 0u) {
            if (free_slot == DJ_LINK_NFSD_HANDLES) {
                free_slot = i;
            }
            continue;
        }
        if (e->hash == h && strcmp(e->path, path) == 0) {
            e->used = s->clock;
            put_handle(s->handle, i, out);
            return;
        }
        if ((int32_t)(e->used - s->handle[lru].used) < 0 || s->handle[lru].gen == 0u) {
            lru = i;
        }
    }
    const uint32_t pick = free_slot != DJ_LINK_NFSD_HANDLES ? free_slot : lru;
    dj_link_nfsd_handle_t *e = &s->handle[pick];
    const size_t n = strlen(path);                 /* callers keep it under PATH_MAX */
    memcpy(e->path, path, n + 1u);
    e->hash = h;
    e->gen = s->next_gen++;
    if (s->next_gen == 0u) {
        s->next_gen = 1u;
    }
    e->used = s->clock;
    put_handle(s->handle, pick, out);
}

/* The path a handle names: 0 = ok (*path set), else an nfsstat. Only the first
 * 12 bytes count: a CDJ rewrites the rest. */
static uint32_t resolve(dj_link_nfsd_t *s, const uint8_t *fh, const char **path)
{
    static const char root[] = "";
    const uint32_t idx = djlink_rd32(&fh[0]);
    if (idx == 0u && djlink_rd32(&fh[4]) == 0u && djlink_rd32(&fh[8]) == 0u) {
        *path = root;
        return 0u;
    }
    if (idx == 0u || idx > DJ_LINK_NFSD_HANDLES) {
        s->stale++;
        return DJ_LINK_NFSERR_STALE;
    }
    dj_link_nfsd_handle_t *h = &s->handle[idx - 1u];
    if (h->gen == 0u || h->gen != djlink_rd32(&fh[4]) || h->hash != djlink_rd32(&fh[8])) {
        s->stale++;
        return DJ_LINK_NFSERR_STALE;
    }
    h->used = ++s->clock;
    *path = h->path;
    return 0u;
}

static bool client_utf16(dj_link_nfsd_t *s, uint32_t addr)
{
    for (uint32_t i = 0; i < DJ_LINK_NFSD_CLIENTS; i++) {
        if (s->client[i].addr == addr && addr != 0u) {
            return s->client[i].utf16;
        }
    }
    return false;
}

static void client_note(dj_link_nfsd_t *s, uint32_t addr, bool utf16)
{
    if (addr == 0u) {
        return;
    }
    uint32_t pick = 0u;
    s->clock++;
    for (uint32_t i = 0; i < DJ_LINK_NFSD_CLIENTS; i++) {
        if (s->client[i].addr == addr) {
            pick = i;
            goto set;
        }
        if (s->client[i].addr == 0u ||
            (s->client[pick].addr != 0u && (int32_t)(s->client[i].used - s->client[pick].used) < 0)) {
            pick = i;
        }
    }
set:
    s->client[pick].addr = addr;
    s->client[pick].utf16 = utf16;
    s->client[pick].used = s->clock;
}

/* ---- attributes ---------------------------------------------------------- */

static void put_attr(wr_t *w, const dj_link_nfsd_stat_t *st)
{
    djlink_nfs_fattr_t a;
    memset(&a, 0, sizeof(a));
    const bool dir = st->type == DJLINK_NFS_TYPE_DIR;
    a.type = st->type;
    a.mode = dir ? 0x416Du /* 040555 */ : 0x8124u /* 0100444 */;
    a.nlink = dir ? 2u : 1u;
    a.size = st->size;
    a.blocksize = 512u;
    a.blocks = (st->size + 511u) / 512u;
    a.fsid = 2u;
    a.fileid = st->fileid;
    a.atime_s = a.mtime_s = a.ctime_s = st->mtime;
    if (w->err || w->cap - w->pos < DJLINK_NFS_FATTR_LEN) {
        w->err = true;
        return;
    }
    djlink_nfs_fattr_encode(&a, &w->buf[w->pos]);
    w->pos += DJLINK_NFS_FATTR_LEN;
}

static uint32_t stat_path(dj_link_nfsd_t *s, const char *rel, dj_link_nfsd_stat_t *st)
{
    if (!s->fs.stat) {
        return DJ_LINK_NFSERR_IO;
    }
    memset(st, 0, sizeof(*st));
    const int rc = s->fs.stat(s->fs.ctx, rel, st);
    return rc < 0 ? (uint32_t)(-rc) : (uint32_t)rc;
}

/* ---- replies ------------------------------------------------------------- */

static size_t start_reply(wr_t *w, uint32_t xid, uint32_t accept_stat)
{
    const int n = djlink_rpc_reply_build(w->buf, w->cap, xid, accept_stat, NULL, 0);
    if (n < 0) {
        w->err = true;
        return 0u;
    }
    w->pos = (size_t)n;
    return (size_t)n;
}

static size_t finish(wr_t *w)
{
    return w->err ? 0u : w->pos;
}

/* A reply that is only an nfsstat (and nothing after it). */
static size_t status_only(wr_t *w, uint32_t xid, uint32_t status)
{
    start_reply(w, xid, DJLINK_RPC_SUCCESS);
    w32(w, status);
    return finish(w);
}

static size_t reject(wr_t *w, uint32_t xid, uint32_t accept_stat)
{
    start_reply(w, xid, accept_stat);
    return finish(w);
}

/* ---- portmapper ---------------------------------------------------------- */

size_t dj_link_nfsd_portmap(dj_link_nfsd_t *s, const uint8_t *in, size_t len, uint8_t *out,
                            size_t cap)
{
    djlink_rpc_call_t c;
    if (djlink_rpc_call_parse(in, len, &c) != DJLINK_OK) {
        return 0u;
    }
    wr_t w = { out, cap, 0u, false };
    if (c.prog != DJLINK_PMAP_PROG) {
        return reject(&w, c.xid, DJLINK_RPC_PROG_UNAVAIL);
    }
    if (c.vers != DJLINK_PMAP_VERS) {
        start_reply(&w, c.xid, RPC_PROG_MISMATCH);
        w32(&w, DJLINK_PMAP_VERS);
        w32(&w, DJLINK_PMAP_VERS);
        return finish(&w);
    }
    if (c.proc == PMAP_PROC_NULL) {
        return reject(&w, c.xid, DJLINK_RPC_SUCCESS);
    }
    if (c.proc != PMAP_PROC_GETPORT) {
        return reject(&w, c.xid, DJLINK_RPC_PROC_UNAVAIL);
    }
    rd_t r = { c.args, c.args_len, 0u, false };
    const uint32_t prog = r32(&r);
    const uint32_t vers = r32(&r);
    const uint32_t prot = r32(&r);
    (void)r32(&r);                       /* port: ignored in a GETPORT */
    if (r.err) {
        return reject(&w, c.xid, DJLINK_RPC_GARBAGE_ARGS);
    }
    uint32_t port = 0u;                  /* 0 = not registered */
    if (prot == DJLINK_IPPROTO_UDP) {
        if (s->available && prog == DJLINK_MOUNT_PROG && vers == DJLINK_MOUNT_VERS) {
            port = s->mount_port;
        } else if (s->available && prog == DJLINK_NFS_PROG && vers == DJLINK_NFS_VERS) {
            port = s->nfs_port;
        } else if (prog == DJLINK_PMAP_PROG && vers == DJLINK_PMAP_VERS) {
            port = s->pmap_port;
        }
    }
    start_reply(&w, c.xid, DJLINK_RPC_SUCCESS);
    w32(&w, port);
    return finish(&w);
}

/* ---- MOUNT --------------------------------------------------------------- */

/* "/C/" is the USB slot export; the empty path and "C" with or without
 * slashes mean the same. Anything else (the SD slot "/B/", ...) is not
 * exported. */
static bool export_is_usb(const char *p)
{
    while (*p == '/') {
        p++;
    }
    if (p[0] == '\0') {
        return true;
    }
    if (p[0] != 'C' && p[0] != 'c') {
        return false;
    }
    p++;
    while (*p == '/') {
        p++;
    }
    return *p == '\0';
}

static size_t mount_proc(dj_link_nfsd_t *s, uint32_t client, const djlink_rpc_call_t *c,
                         wr_t *w)
{
    if (c->vers != DJLINK_MOUNT_VERS) {
        start_reply(w, c->xid, RPC_PROG_MISMATCH);
        w32(w, DJLINK_MOUNT_VERS);
        w32(w, DJLINK_MOUNT_VERS);
        return finish(w);
    }
    switch (c->proc) {
    case MOUNT_PROC_NULL:
    case MOUNT_PROC_UMNT:
    case MOUNT_PROC_UMNTALL:
        return reject(w, c->xid, DJLINK_RPC_SUCCESS);
    case MOUNT_PROC_MNT: {
        rd_t r = { c->args, c->args_len, 0u, false };
        size_t n = 0;
        const uint8_t *raw = ropaque(&r, DJLINK_NFS_PATH_MAX * 2u, &n);
        if (r.err) {
            return reject(w, c->xid, DJLINK_RPC_GARBAGE_ARGS);
        }
        char path[DJLINK_NFS_PATH_MAX + 1u];
        bool utf16 = false;
        if (decode_name(raw, n, path, sizeof(path), &utf16) < 0) {
            return status_only(w, c->xid, DJ_LINK_NFSERR_NOENT);
        }
        client_note(s, client, utf16);
        s->mounts++;
        if (!s->available || !export_is_usb(path)) {
            return status_only(w, c->xid, DJ_LINK_NFSERR_NOENT);
        }
        start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
        w32(w, 0u);
        uint8_t fh[FH];
        memset(fh, 0, sizeof(fh));
        wbytes(w, fh, FH);
        return finish(w);
    }
    case MOUNT_PROC_EXPORT: {
        start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
        if (!s->available) {
            w32(w, 0u);                  /* no exports: what a player unlinked answers */
            return finish(w);
        }
        w32(w, 1u);
        const bool u16 = client_utf16(s, client);
        static const char name[] = "/C/";
        if (u16) {
            uint8_t enc[16];
            const int e = utf8_to_utf16le(name, 3u, enc, sizeof(enc));
            wstring(w, enc, e < 0 ? 0u : (size_t)e);
        } else {
            wstring(w, (const uint8_t *)name, 3u);
        }
        w32(w, 1u);                      /* one group */
        wstring(w, (const uint8_t *)s->export_group, strlen(s->export_group));
        w32(w, 0u);                      /* end of groups */
        w32(w, 0u);                      /* end of exports */
        return finish(w);
    }
    default:
        return reject(w, c->xid, DJLINK_RPC_PROC_UNAVAIL);
    }
}

/* ---- NFS ----------------------------------------------------------------- */

static size_t nfs_getattr(dj_link_nfsd_t *s, const djlink_rpc_call_t *c, wr_t *w)
{
    rd_t r = { c->args, c->args_len, 0u, false };
    const uint8_t *fh = rfh(&r);
    if (r.err) {
        return reject(w, c->xid, DJLINK_RPC_GARBAGE_ARGS);
    }
    const char *path;
    uint32_t st = resolve(s, fh, &path);
    dj_link_nfsd_stat_t a;
    if (st == 0u) {
        st = stat_path(s, path, &a);
    }
    if (st != 0u) {
        s->errors++;
        return status_only(w, c->xid, st);
    }
    start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
    w32(w, 0u);
    put_attr(w, &a);
    return finish(w);
}

static size_t nfs_lookup(dj_link_nfsd_t *s, uint32_t client, const djlink_rpc_call_t *c,
                         wr_t *w)
{
    rd_t r = { c->args, c->args_len, 0u, false };
    const uint8_t *fh = rfh(&r);
    size_t n = 0;
    const uint8_t *raw = ropaque(&r, DJLINK_NFS_NAME_MAX, &n);
    if (r.err) {
        return reject(w, c->xid, DJLINK_RPC_GARBAGE_ARGS);
    }
    s->lookups++;
    const char *dir;
    uint32_t st = resolve(s, fh, &dir);
    if (st != 0u) {
        return status_only(w, c->xid, st);
    }
    char name[DJ_LINK_NFSD_PATH_MAX];
    bool utf16 = false;
    const int nl = decode_name(raw, n, name, sizeof(name), &utf16);
    client_note(s, client, utf16);
    if (nl < 0 || !plain_element(name, (size_t)nl)) {
        s->errors++;
        return status_only(w, c->xid, nl < 0 ? DJ_LINK_NFSERR_NAMETOOLONG : DJ_LINK_NFSERR_NOENT);
    }
    dj_link_nfsd_stat_t d;
    st = stat_path(s, dir, &d);
    if (st != 0u || d.type != DJLINK_NFS_TYPE_DIR) {
        s->errors++;
        return status_only(w, c->xid, st != 0u ? st : DJ_LINK_NFSERR_NOTDIR);
    }
    char child[DJ_LINK_NFSD_PATH_MAX];
    const size_t dl = strlen(dir);
    if (dl + (dl ? 1u : 0u) + (size_t)nl + 1u > sizeof(child)) {
        s->errors++;
        return status_only(w, c->xid, DJ_LINK_NFSERR_NAMETOOLONG);
    }
    memcpy(child, dir, dl);
    if (dl) {
        child[dl] = '/';
    }
    memcpy(&child[dl + (dl ? 1u : 0u)], name, (size_t)nl + 1u);
    dj_link_nfsd_stat_t a;
    st = stat_path(s, child, &a);
    if (st != 0u) {
        s->errors++;
        return status_only(w, c->xid, st);
    }
    uint8_t cfh[FH];
    handle_for(s, child, cfh);
    start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
    w32(w, 0u);
    wbytes(w, cfh, FH);
    put_attr(w, &a);
    return finish(w);
}

static size_t nfs_read(dj_link_nfsd_t *s, const djlink_rpc_call_t *c, wr_t *w)
{
    rd_t r = { c->args, c->args_len, 0u, false };
    const uint8_t *fh = rfh(&r);
    const uint32_t off = r32(&r);
    uint32_t count = r32(&r);
    (void)r32(&r);                       /* totalcount, unused */
    if (r.err) {
        return reject(w, c->xid, DJLINK_RPC_GARBAGE_ARGS);
    }
    s->reads++;
    const char *path;
    uint32_t st = resolve(s, fh, &path);
    dj_link_nfsd_stat_t a;
    if (st == 0u) {
        st = stat_path(s, path, &a);
    }
    if (st == 0u && a.type == DJLINK_NFS_TYPE_DIR) {
        st = DJ_LINK_NFSERR_ISDIR;
    }
    if (st != 0u) {
        s->errors++;
        return status_only(w, c->xid, st);
    }
    /* status + attributes + length word, then the data. */
    const size_t head = w->pos + 4u + DJLINK_NFS_FATTR_LEN + 4u;
    size_t room = w->cap > head ? w->cap - head : 0u;
    room &= ~(size_t)3u;
    if (count > DJLINK_NFS_MAXDATA) {
        count = DJLINK_NFS_MAXDATA;
    }
    if (count > room) {
        count = (uint32_t)room;
    }
    if (off >= a.size) {
        count = 0u;
    } else if (count > a.size - off) {
        count = a.size - off;
    }
    start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
    w32(w, 0u);
    put_attr(w, &a);
    const size_t len_at = w->pos;
    w32(w, 0u);
    uint32_t got = 0u;
    if (count > 0u && !w->err) {
        int rc = DJ_LINK_NFSERR_IO;
        if (s->fs.read) {
            rc = s->fs.read(s->fs.ctx, path, off, &w->buf[w->pos], count, &got);
        }
        if (rc != 0 || got > count) {
            s->errors++;
            *w = (wr_t){ w->buf, w->cap, 0u, false };       /* start over: status only */
            return status_only(w, c->xid, rc < 0 ? (uint32_t)(-rc) : rc ? (uint32_t)rc
                                                                          : DJ_LINK_NFSERR_IO);
        }
    }
    djlink_wr32(&w->buf[len_at], got);
    const size_t padded = ((size_t)got + 3u) & ~(size_t)3u;
    memset(&w->buf[w->pos + got], 0, padded - got);
    w->pos += padded;
    s->bytes_read += got;
    return finish(w);
}

static size_t nfs_readdir(dj_link_nfsd_t *s, uint32_t client, const djlink_rpc_call_t *c,
                          wr_t *w)
{
    rd_t r = { c->args, c->args_len, 0u, false };
    const uint8_t *fh = rfh(&r);
    uint32_t cookie = r32(&r);
    uint32_t count = r32(&r);
    if (r.err) {
        return reject(w, c->xid, DJLINK_RPC_GARBAGE_ARGS);
    }
    s->readdirs++;
    const char *path;
    uint32_t st = resolve(s, fh, &path);
    dj_link_nfsd_stat_t d;
    if (st == 0u) {
        st = stat_path(s, path, &d);
    }
    if (st == 0u && d.type != DJLINK_NFS_TYPE_DIR) {
        st = DJ_LINK_NFSERR_NOTDIR;
    }
    if (st == 0u && !s->fs.readdir) {
        st = DJ_LINK_NFSERR_IO;
    }
    if (st != 0u) {
        s->errors++;
        return status_only(w, c->xid, st);
    }
    /* The client's budget, kept inside one reply (and a UDP datagram). */
    size_t budget = count;
    if (budget < 1024u || budget > 8000u) {
        budget = 8000u;
    }
    if (budget > w->cap - w->pos - 16u) {
        budget = w->cap - w->pos - 16u;
    }
    const bool u16 = client_utf16(s, client);
    start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
    w32(w, 0u);
    size_t used = 0u;
    bool eof = true;
    for (uint32_t i = cookie;; i++) {
        char name[DJ_LINK_NFSD_PATH_MAX];
        uint32_t fid = 0u;
        const int rc = s->fs.readdir(s->fs.ctx, path, i, name, sizeof(name), &fid);
        if (rc < 0) {
            s->errors++;
            *w = (wr_t){ w->buf, w->cap, 0u, false };
            return status_only(w, c->xid, (uint32_t)(-rc));
        }
        if (rc == 0) {
            break;
        }
        uint8_t enc[DJ_LINK_NFSD_PATH_MAX * 2u];
        size_t el;
        if (u16) {
            const int e = utf8_to_utf16le(name, strlen(name), enc, sizeof(enc));
            if (e < 0) {
                continue;                /* a name the charset cannot carry */
            }
            el = (size_t)e;
        } else {
            el = strlen(name);
            memcpy(enc, name, el);
        }
        /* value-follows, fileid, string (length + padded bytes), cookie. */
        const size_t need = 4u + 4u + 4u + ((el + 3u) & ~(size_t)3u) + 4u;
        if (i > cookie && used + need > budget) {
            eof = false;
            break;
        }
        w32(w, 1u);
        w32(w, fid ? fid : i + 1u);
        wstring(w, enc, el);
        w32(w, i + 1u);
        used += need;
    }
    w32(w, 0u);
    w32(w, eof ? 1u : 0u);
    return finish(w);
}

static size_t nfs_statfs(const djlink_rpc_call_t *c, wr_t *w)
{
    start_reply(w, c->xid, DJLINK_RPC_SUCCESS);
    w32(w, 0u);
    w32(w, DJLINK_NFS_MAXDATA);          /* tsize: the largest transfer we serve */
    w32(w, 512u);                        /* bsize */
    w32(w, 32000000u);                   /* blocks */
    w32(w, 0u);                          /* bfree: read-only, nothing to give */
    w32(w, 0u);                          /* bavail */
    return finish(w);
}

static size_t nfs_proc(dj_link_nfsd_t *s, uint32_t client, const djlink_rpc_call_t *c, wr_t *w)
{
    if (c->vers != DJLINK_NFS_VERS) {
        start_reply(w, c->xid, RPC_PROG_MISMATCH);
        w32(w, DJLINK_NFS_VERS);
        w32(w, DJLINK_NFS_VERS);
        return finish(w);
    }
    if (!s->available && c->proc != NFS_PROC_NULL) {
        return status_only(w, c->xid, DJ_LINK_NFSERR_STALE);
    }
    switch (c->proc) {
    case NFS_PROC_NULL:
    case NFS_PROC_ROOT:
    case NFS_PROC_WRITECACHE:
        return reject(w, c->xid, DJLINK_RPC_SUCCESS);
    case NFS_PROC_GETATTR:
        return nfs_getattr(s, c, w);
    case NFS_PROC_LOOKUP:
        return nfs_lookup(s, client, c, w);
    case NFS_PROC_READ:
        return nfs_read(s, c, w);
    case NFS_PROC_READDIR:
        return nfs_readdir(s, client, c, w);
    case NFS_PROC_STATFS:
        return nfs_statfs(c, w);
    case NFS_PROC_READLINK:
        return status_only(w, c->xid, DJ_LINK_NFSERR_INVAL);   /* no symlinks on FAT */
    case NFS_PROC_SETATTR:
    case NFS_PROC_WRITE:
    case NFS_PROC_CREATE:
    case NFS_PROC_REMOVE:
    case NFS_PROC_RENAME:
    case NFS_PROC_LINK:
    case NFS_PROC_SYMLINK:
    case NFS_PROC_MKDIR:
    case NFS_PROC_RMDIR:
        s->refused_writes++;
        return status_only(w, c->xid, DJ_LINK_NFSERR_ROFS);
    default:
        return reject(w, c->xid, DJLINK_RPC_PROC_UNAVAIL);
    }
}

size_t dj_link_nfsd_rpc(dj_link_nfsd_t *s, uint32_t client, const uint8_t *in, size_t len,
                        uint8_t *out, size_t cap)
{
    if (!s || !in || !out || len > DJ_LINK_NFSD_RPC_MAX) {
        return 0u;
    }
    djlink_rpc_call_t c;
    if (djlink_rpc_call_parse(in, len, &c) != DJLINK_OK) {
        return 0u;
    }
    s->calls++;
    wr_t w = { out, cap, 0u, false };
    if (c.prog == DJLINK_MOUNT_PROG) {
        return mount_proc(s, client, &c, &w);
    }
    if (c.prog == DJLINK_NFS_PROG) {
        return nfs_proc(s, client, &c, &w);
    }
    return reject(&w, c.xid, DJLINK_RPC_PROG_UNAVAIL);
}
