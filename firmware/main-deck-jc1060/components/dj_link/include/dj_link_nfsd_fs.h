#pragma once
//
// dj_link_nfsd_fs (v342) - the POSIX tree behind dj_link_nfsd: a directory of
// the file system (the mounted USB key, "/usb") exported read-only.
//
// Only reads the owner's hooks allow: gate_begin / gate_end wrap every call
// that touches the medium (the firmware passes media_io_gate_begin / _end, the
// mutex the audio decoder takes around its USB reads, so a download never
// holds the key while a deck needs it for longer than one 8 KB read). The last
// file and the last directory stay open for sequential access (a download is
// one READ after another, a listing one READDIR after another); the owner
// closes them when idle or when the key goes (dj_link_nfsd_fs_close).
//
// POSIX only (stat, fopen, opendir): host testable on a temporary directory.
// One thread uses it at a time.
//
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>

#include "dj_link_nfsd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_LINK_NFSD_FS_ROOT_MAX 32u

typedef struct {
    char root[DJ_LINK_NFSD_FS_ROOT_MAX];     /* "/usb": no trailing '/' */
    void (*gate_begin)(void);                /* NULL = none */
    void (*gate_end)(void);

    FILE    *fp;                             /* last file read */
    char     fp_rel[DJ_LINK_NFSD_PATH_MAX];
    uint32_t fp_pos;                         /* where the next sequential read starts */
    DIR     *dp;                             /* last directory listed */
    char     dp_rel[DJ_LINK_NFSD_PATH_MAX];
    uint32_t dp_next;                        /* the entry index the next readdir() returns */
    uint32_t last_ms;                        /* of the last use, set by the owner */
} dj_link_nfsd_fs_posix_t;

void dj_link_nfsd_fs_init(dj_link_nfsd_fs_posix_t *p, const char *root, void (*gate_begin)(void),
                          void (*gate_end)(void));
/* The callbacks table for dj_link_nfsd_init. */
dj_link_nfsd_fs_t dj_link_nfsd_fs_ops(dj_link_nfsd_fs_posix_t *p);
/* Close the cached file and directory. */
void dj_link_nfsd_fs_close(dj_link_nfsd_fs_posix_t *p);
/* Close them when unused for max_idle_ms (the owner's clock, ms). */
void dj_link_nfsd_fs_idle(dj_link_nfsd_fs_posix_t *p, uint32_t now_ms, uint32_t max_idle_ms);
/* Record a use: keeps the cached handles open. */
void dj_link_nfsd_fs_touch(dj_link_nfsd_fs_posix_t *p, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
