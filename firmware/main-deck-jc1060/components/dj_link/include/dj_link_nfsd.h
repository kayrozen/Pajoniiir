#pragma once
//
// dj_link_nfsd (v342) - the read-only NFSv2 export of our own USB key, so a
// CDJ or vynull that sees our player (media query reply, v247) can fetch
// export.pdb, the analysis files and the audio the way it does from a CDJ.
//
// What a player serves (crate-digger, vynull's nfs package): the portmapper
// answers GETPORT for the MOUNT and NFS programs, MOUNT MNT of "/C/" (the USB
// slot) returns the root file handle, then LOOKUP one path element at a time,
// GETATTR, READ and READDIR. A CDJ names every element in UTF-16LE and may
// rewrite bytes 12..31 of a file handle, so only the first 12 bytes identify
// one. This module speaks that, and plain UTF-8 names to a Linux-style
// client, per client address (the charset of its MNT / LOOKUP decides the
// charset of its READDIR names).
//
// Pure: it never opens a file or a socket. The tree comes through
// dj_link_nfsd_fs_t (a POSIX backend is in dj_link_nfsd_fs.h); the owner
// feeds it datagrams and sends what it returns. Read-only by construction:
// every write procedure answers NFSERR_ROFS, and a path element can only be a
// plain name (no "/", ".", ".." or empty), so the export root is a jail.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_NFSD_PATH_MAX     256u   /* UTF-8 bytes of a relative path, incl. NUL */
#define DJ_LINK_NFSD_HANDLES      32u    /* file handles kept; the oldest is recycled */
#define DJ_LINK_NFSD_CLIENTS      4u     /* charsets remembered */
#define DJ_LINK_NFSD_REPLY_MAX    8448u  /* header + status + attr + 8192 bytes of data */
#define DJ_LINK_NFSD_RPC_MAX      1536u  /* a request is a few hundred bytes (LOOKUP) */
#define DJ_LINK_NFSD_PMAP_PORT    111u

/* nfsstat values this module returns (RFC 1094). */
#define DJ_LINK_NFSERR_NOENT   2u
#define DJ_LINK_NFSERR_IO      5u
#define DJ_LINK_NFSERR_ACCES   13u
#define DJ_LINK_NFSERR_NOTDIR  20u
#define DJ_LINK_NFSERR_ISDIR   21u
#define DJ_LINK_NFSERR_INVAL   22u
#define DJ_LINK_NFSERR_NAMETOOLONG 63u
#define DJ_LINK_NFSERR_ROFS    30u
#define DJ_LINK_NFSERR_STALE   70u

typedef struct {
    uint32_t type;       /* DJLINK_NFS_TYPE_REG or _DIR */
    uint32_t size;       /* bytes; a directory reports 4096 */
    uint32_t mtime;      /* seconds */
    uint32_t fileid;
} dj_link_nfsd_stat_t;

/* The tree. `rel` is UTF-8, '/'-separated, without a leading '/', "" = the
 * export root, never containing "." or "..". Return 0 on success, else an
 * nfsstat. A backend may block on the medium; the owner calls the module from
 * a task that can. */
typedef struct {
    int (*stat)(void *ctx, const char *rel, dj_link_nfsd_stat_t *out);
    /* Up to `count` bytes at `off`; *got < count only at the end of the file. */
    int (*read)(void *ctx, const char *rel, uint32_t off, uint8_t *dst, uint32_t count,
                uint32_t *got);
    /* Entry number `index` (0-based, stable order, no "." or ".."): 1 = filled,
     * 0 = no more, negative = -nfsstat. */
    int (*readdir)(void *ctx, const char *rel, uint32_t index, char *name, size_t cap,
                   uint32_t *fileid);
    void *ctx;
} dj_link_nfsd_fs_t;

typedef struct {
    char     path[DJ_LINK_NFSD_PATH_MAX];
    uint32_t gen;        /* 0 = free */
    uint32_t hash;       /* of path */
    uint32_t used;       /* lru stamp */
} dj_link_nfsd_handle_t;

typedef struct {
    uint32_t addr;       /* client address, 0 = free */
    bool     utf16;
    uint32_t used;
} dj_link_nfsd_client_t;

typedef struct {
    dj_link_nfsd_fs_t fs;
    uint16_t mount_port;          /* what GETPORT answers; may equal nfs_port */
    uint16_t nfs_port;
    uint16_t pmap_port;           /* what GETPORT answers for the portmapper itself */
    bool     available;           /* the key is mounted: MNT and every call work */
    char     export_group[32];    /* access group in EXPORT, "ip/mask" */

    uint32_t clock;               /* lru stamps */
    uint32_t next_gen;
    dj_link_nfsd_handle_t handle[DJ_LINK_NFSD_HANDLES];
    dj_link_nfsd_client_t client[DJ_LINK_NFSD_CLIENTS];

    /* Counters for the log and the tests. */
    uint32_t calls, mounts, lookups, reads, readdirs, refused_writes, errors, stale;
    uint64_t bytes_read;
} dj_link_nfsd_t;

/* group = the "ip/mask" EXPORT lists, NULL = "0.0.0.0/0.0.0.0". */
void dj_link_nfsd_init(dj_link_nfsd_t *s, const dj_link_nfsd_fs_t *fs, uint16_t mount_port,
                       uint16_t nfs_port, uint16_t pmap_port, const char *group);
/* The key came or went: handles are dropped either way; MNT / EXPORT / calls
 * answer as with no export while unavailable. */
void dj_link_nfsd_set_available(dj_link_nfsd_t *s, bool available);

/* One datagram of the portmapper port: the reply length written to out, or 0
 * for no reply (not an RPC call). */
size_t dj_link_nfsd_portmap(dj_link_nfsd_t *s, const uint8_t *in, size_t len, uint8_t *out,
                            size_t cap);
/* One datagram of the MOUNT / NFS port (both programs are served on it).
 * client = the sender's address (host order), for the charset memory. */
size_t dj_link_nfsd_rpc(dj_link_nfsd_t *s, uint32_t client, const uint8_t *in, size_t len,
                        uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
