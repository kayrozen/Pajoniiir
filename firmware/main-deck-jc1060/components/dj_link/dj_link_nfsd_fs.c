#include "dj_link_nfsd_fs.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "djlink/nfs.h"

static void gate_in(const dj_link_nfsd_fs_posix_t *p)
{
    if (p->gate_begin) {
        p->gate_begin();
    }
}

static void gate_out(const dj_link_nfsd_fs_posix_t *p)
{
    if (p->gate_end) {
        p->gate_end();
    }
}

static uint32_t fnv32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return h;
}

/* root + "/" + rel into out; false if it does not fit. */
static bool full_path(const dj_link_nfsd_fs_posix_t *p, const char *rel, char *out, size_t cap)
{
    const size_t rl = strlen(p->root);
    const size_t n = strlen(rel);
    if (rl + 1u + n + 1u > cap) {
        return false;
    }
    memcpy(out, p->root, rl);
    out[rl] = '/';
    memcpy(&out[rl + 1u], rel, n + 1u);
    return true;
}

static int errno_to_nfs(int e)
{
    switch (e) {
    case ENOENT:
        return DJ_LINK_NFSERR_NOENT;
    case ENOTDIR:
        return DJ_LINK_NFSERR_NOTDIR;
    case EISDIR:
        return DJ_LINK_NFSERR_ISDIR;
    case EACCES:
    case EPERM:
        return DJ_LINK_NFSERR_ACCES;
    case ENAMETOOLONG:
        return DJ_LINK_NFSERR_NAMETOOLONG;
    default:
        return DJ_LINK_NFSERR_IO;
    }
}

void dj_link_nfsd_fs_init(dj_link_nfsd_fs_posix_t *p, const char *root, void (*gate_begin)(void),
                          void (*gate_end)(void))
{
    memset(p, 0, sizeof(*p));
    strncpy(p->root, root ? root : "", sizeof(p->root) - 1u);
    size_t n = strlen(p->root);
    while (n > 1u && p->root[n - 1u] == '/') {
        p->root[--n] = '\0';
    }
    p->gate_begin = gate_begin;
    p->gate_end = gate_end;
}

void dj_link_nfsd_fs_close(dj_link_nfsd_fs_posix_t *p)
{
    if (p->fp || p->dp) {
        gate_in(p);
        if (p->fp) {
            fclose(p->fp);
        }
        if (p->dp) {
            closedir(p->dp);
        }
        gate_out(p);
    }
    p->fp = NULL;
    p->dp = NULL;
    p->fp_rel[0] = '\0';
    p->dp_rel[0] = '\0';
}

void dj_link_nfsd_fs_touch(dj_link_nfsd_fs_posix_t *p, uint32_t now_ms)
{
    p->last_ms = now_ms;
}

void dj_link_nfsd_fs_idle(dj_link_nfsd_fs_posix_t *p, uint32_t now_ms, uint32_t max_idle_ms)
{
    if ((p->fp || p->dp) && (uint32_t)(now_ms - p->last_ms) >= max_idle_ms) {
        dj_link_nfsd_fs_close(p);
    }
}

static int fs_stat(void *ctx, const char *rel, dj_link_nfsd_stat_t *out)
{
    dj_link_nfsd_fs_posix_t *p = ctx;
    char path[DJ_LINK_NFSD_FS_ROOT_MAX + DJ_LINK_NFSD_PATH_MAX + 2u];
    if (!full_path(p, rel, path, sizeof(path))) {
        return DJ_LINK_NFSERR_NAMETOOLONG;
    }
    struct stat st;
    gate_in(p);
    const int rc = stat(path, &st);
    const int e = errno;
    gate_out(p);
    if (rc != 0) {
        return errno_to_nfs(e);
    }
    if (S_ISDIR(st.st_mode)) {
        out->type = DJLINK_NFS_TYPE_DIR;
        out->size = 4096u;
    } else if (S_ISREG(st.st_mode)) {
        out->type = DJLINK_NFS_TYPE_REG;
        out->size = st.st_size > 0x7fffffff ? 0x7fffffffu : (uint32_t)st.st_size;
    } else {
        return DJ_LINK_NFSERR_NOENT;         /* devices, sockets: not exported */
    }
    out->mtime = (uint32_t)st.st_mtime;
    out->fileid = fnv32(rel);
    if (out->fileid == 0u) {
        out->fileid = 1u;
    }
    return 0;
}

static int fs_read(void *ctx, const char *rel, uint32_t off, uint8_t *dst, uint32_t count,
                   uint32_t *got)
{
    dj_link_nfsd_fs_posix_t *p = ctx;
    *got = 0u;
    int result = 0;
    gate_in(p);
    if (!p->fp || strcmp(p->fp_rel, rel) != 0) {
        if (p->fp) {
            fclose(p->fp);
            p->fp = NULL;
        }
        char path[DJ_LINK_NFSD_FS_ROOT_MAX + DJ_LINK_NFSD_PATH_MAX + 2u];
        if (!full_path(p, rel, path, sizeof(path))) {
            gate_out(p);
            return DJ_LINK_NFSERR_NAMETOOLONG;
        }
        p->fp = fopen(path, "rb");
        if (!p->fp) {
            const int e = errno;
            gate_out(p);
            return errno_to_nfs(e);
        }
        strncpy(p->fp_rel, rel, sizeof(p->fp_rel) - 1u);
        p->fp_rel[sizeof(p->fp_rel) - 1u] = '\0';
        p->fp_pos = 0u;
    }
    if (off != p->fp_pos) {
        if (fseek(p->fp, (long)off, SEEK_SET) != 0) {
            result = DJ_LINK_NFSERR_IO;
        }
        p->fp_pos = off;
    }
    if (result == 0) {
        const size_t n = fread(dst, 1u, count, p->fp);
        if (n < count && ferror(p->fp)) {
            result = DJ_LINK_NFSERR_IO;
            fclose(p->fp);
            p->fp = NULL;
        } else {
            *got = (uint32_t)n;
            p->fp_pos = off + (uint32_t)n;
        }
    }
    gate_out(p);
    return result;
}

static int fs_readdir(void *ctx, const char *rel, uint32_t index, char *name, size_t cap,
                      uint32_t *fileid)
{
    dj_link_nfsd_fs_posix_t *p = ctx;
    int result = 0;
    gate_in(p);
    if (!p->dp || p->dp_next != index || strcmp(p->dp_rel, rel) != 0) {
        if (p->dp) {
            closedir(p->dp);
            p->dp = NULL;
        }
        char path[DJ_LINK_NFSD_FS_ROOT_MAX + DJ_LINK_NFSD_PATH_MAX + 2u];
        if (!full_path(p, rel, path, sizeof(path))) {
            gate_out(p);
            return -(int)DJ_LINK_NFSERR_NAMETOOLONG;
        }
        p->dp = opendir(path);
        if (!p->dp) {
            const int e = errno;
            gate_out(p);
            return -errno_to_nfs(e);
        }
        strncpy(p->dp_rel, rel, sizeof(p->dp_rel) - 1u);
        p->dp_rel[sizeof(p->dp_rel) - 1u] = '\0';
        p->dp_next = 0u;
        /* Skip to `index`: a directory is read in the same order every time. */
        while (p->dp_next < index) {
            struct dirent *e = readdir(p->dp);
            if (!e) {
                break;
            }
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
                strlen(e->d_name) < cap) {
                p->dp_next++;
            }
        }
    }
    for (;;) {
        struct dirent *e = readdir(p->dp);
        if (!e) {
            result = 0;
            closedir(p->dp);
            p->dp = NULL;
            break;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strlen(e->d_name) >= cap) {
            continue;                        /* names the reply cannot hold are not listed */
        }
        strcpy(name, e->d_name);
        *fileid = fnv32(name) ^ fnv32(rel);
        if (*fileid == 0u) {
            *fileid = 1u;
        }
        p->dp_next++;
        result = 1;
        break;
    }
    gate_out(p);
    return result;
}

dj_link_nfsd_fs_t dj_link_nfsd_fs_ops(dj_link_nfsd_fs_posix_t *p)
{
    dj_link_nfsd_fs_t ops = { fs_stat, fs_read, fs_readdir, p };
    return ops;
}
