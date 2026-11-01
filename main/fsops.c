/* fsops: every SD mutation from the web API and WebDAV. See fsops.h for the contract.
 * FatFs facts this relies on (ESP-IDF v5.5.2, components/fatfs):
 * - rename never replaces: f_rename returns FR_EXIST when the new name is taken by another object (src/ff.c
 *   f_rename), mapped to EEXIST by vfs/vfs_fat.c fresult_to_errno; so every target is probed with stat first,
 *   under the lock, and conflicts are resolved here;
 * - name lookups are case-insensitive, so stat("Rose.jef") finds "rose.jef". A case-only rename is one rename():
 *   vfs/vfs_fat.c vfs_fat_rename (lines 873-886) passes it straight to f_rename, which treats a new name that
 *   resolves to the same object as free (src/ff.c f_rename, FAT branch lines 5233-5241: FR_NO_FILE -> register the
 *   new entry, drop the old one) and returns FR_EXIST only for another object. vfs_fat_stat leaves st_ino 0
 *   (update_stat_struct memsets the struct), so fs_rename lets f_rename decide on FAT and compares inodes on a
 *   POSIX host, where rename would replace the target;
 * - utime() is implemented (vfs/vfs_fat.c vfs_fat_utime -> f_utime: local time, 2 s steps, 1980 or later);
 * - readdir fills d_type (vfs_fat_readdir_r: DT_DIR / DT_REG); opendir takes no file slot (ff_memalloc), and an
 *   upload keeps exactly one fd open (max_files is 6). */
#include "fsops.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "webcore.h"
#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

#define FS_PATH_CAP 1024              /* root + '/' + rel (< WC_PATH_MAX) + '/' + name (<= 255) + " (999)" */
#define NAME_CAP    256
#define BATCH       16                /* directory entries read per lock section */

static char s_root[64], s_tmp[128];
static fs_hooks_t s_h;
static bool s_ready;
uint32_t fs_stat_probes;

/* Web mutations between their first and last step (see fsops.h). Only this file's task structure matters: every
 * increment has exactly one decrement on every exit path, so the count cannot drift. */
static uint32_t s_inflight;

void fs_inflight_enter(void) { s_inflight++; }
void fs_inflight_leave(void) { if (s_inflight) s_inflight--; }
uint32_t fs_inflight(void) { return s_inflight; }

static void lock(void)      { if (s_h.lock) s_h.lock(); }
static void unlock(void)    { if (s_h.unlock) s_h.unlock(); }
static void close_cat(void) { if (s_h.close_catalog_files) s_h.close_catalog_files(); }
static void changed(void)   { if (s_h.changed) s_h.changed(); }
/* Never under the lock: refresh only stamps a counter and may re-mount the volume when the bus goes quiet. */
static void activity(void)  { if (s_h.activity) s_h.activity(); }
static int64_t now_s(void)  { return s_h.now ? s_h.now() : (int64_t)time(NULL); }

static uint32_t rnd(void) {
#ifdef ESP_PLATFORM
    return esp_random();
#else
    static uint32_t seq;
    uint32_t x = (uint32_t)time(NULL) ^ (++seq * 2654435761u) ^ (uint32_t)getpid();
    x ^= x >> 15; x *= 0x2c1b3c6dU; x ^= x >> 12;
    return x;
#endif
}

static int map_errno(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR:      return FS_ENOENT;
    case EEXIST:                    return FS_EEXIST;
    case ENOSPC:                    return FS_ENOSPC;
    case EINVAL: case ENAMETOOLONG: return FS_EINVAL;
    default:                        return FS_EIO;
    }
}

void fs_init(const char *root, const char *tmp_dir, const fs_hooks_t *h) {
    s_ready = root && tmp_dir && h && strlen(root) < sizeof s_root && strlen(tmp_dir) < sizeof s_tmp;
    if (!s_ready) return;
    strcpy(s_root, root);
    strcpy(s_tmp, tmp_dir);
    s_h = *h;
}

/* Scratch for one operation (heap: the HTTP task stacks are small). rel/dst have one spare byte for split(). */
typedef struct {
    char rel[WC_PATH_MAX + 1], dst[WC_PATH_MAX + 1];
    char name[NAME_CAP + 16];
    char a[FS_PATH_CAP], b[FS_PATH_CAP], t[FS_PATH_CAP];
} work_t;

/* out = root[/rel][/name]; false when it does not fit. */
static bool absp(char *out, const char *rel, const char *name) {
    int n = snprintf(out, FS_PATH_CAP, "%s%s%s%s%s", s_root, *rel ? "/" : "", rel, name ? "/" : "", name ? name : "");
    return n > 0 && n < FS_PATH_CAP;
}
/* Splits a normalised non-empty rel in place: returns the last segment; rel keeps the parent ("" = root). */
static const char *split(char *rel) {
    char *s = strrchr(rel, '/');
    if (!s) { memmove(rel + 1, rel, strlen(rel) + 1); rel[0] = 0; return rel + 1; }
    *s = 0;
    return s + 1;
}
static bool is_dir(const char *abs) { struct stat st; return stat(abs, &st) == 0 && S_ISDIR(st.st_mode); }

int fs_mkdir(const char *rel) {
    if (!s_ready) return FS_EIO;
    fs_inflight_enter();
    work_t *w = malloc(sizeof *w);
    if (!w) { fs_inflight_leave(); return FS_EIO; }
    int rc = FS_EINVAL;
    if (!wc_norm_path(rel, w->rel, sizeof w->rel - 1) || !w->rel[0]) goto out;
    const char *base = split(w->rel);
    if (!wc_sd_safe_name(base, w->name, sizeof w->name) || !absp(w->a, w->rel, w->name)) goto out;
    activity();                                          /* the machine and the web share the volume */
    lock();
    rc = mkdir(w->a, 0775) ? map_errno(errno) : FS_OK;   /* EEXIST (also a case variant on FAT), ENOENT without parent */
    unlock();
    if (rc == FS_OK) changed();                          /* the folder shows up on the disk; nothing served is touched */
out:
    fs_inflight_leave();
    free(w);
    return rc;
}

int fs_rename(const char *rel, const char *new_name) {
    if (!s_ready) return FS_EIO;
    fs_inflight_enter();
    work_t *w = malloc(sizeof *w);
    if (!w) { fs_inflight_leave(); return FS_EIO; }
    int rc = FS_EINVAL;
    if (!wc_norm_path(rel, w->rel, sizeof w->rel - 1) || !w->rel[0]) goto out;
    if (!new_name || !wc_sd_safe_name(new_name, w->name, sizeof w->name)) goto out;
    if (!absp(w->a, w->rel, NULL)) goto out;
    const char *base = split(w->rel);
    bool same = !strcmp(base, w->name);
    if (!absp(w->b, w->rel, w->name)) goto out;
    activity();
    lock();
    struct stat sa, sb;
    if (stat(w->a, &sa)) rc = FS_ENOENT;
    else if (same) rc = FS_OK;                           /* nothing to do */
    else if (stat(w->b, &sb) == 0 && sa.st_ino != 0 && (sa.st_ino != sb.st_ino || sa.st_dev != sb.st_dev))
        rc = FS_EEXIST;                                  /* POSIX host: another entry, which rename would replace */
    else { close_cat(); rc = rename(w->a, w->b) ? map_errno(errno) : FS_OK; }   /* FAT (st_ino 0): f_rename decides */
    unlock();
    if (rc == FS_OK && !same) changed();
out:
    fs_inflight_leave();
    free(w);
    return rc;
}

int fs_move(const char *rel, const char *dst_dir_rel) {
    if (!s_ready) return FS_EIO;
    fs_inflight_enter();
    work_t *w = malloc(sizeof *w);
    if (!w) { fs_inflight_leave(); return FS_EIO; }
    int rc = FS_EINVAL;
    if (!wc_norm_path(rel, w->rel, sizeof w->rel - 1) || !w->rel[0]) goto out;
    if (!dst_dir_rel || !wc_norm_path(dst_dir_rel, w->dst, sizeof w->dst - 1)) goto out;
    size_t rl = strlen(w->rel);
    if (!strcmp(w->dst, w->rel) || (!strncmp(w->dst, w->rel, rl) && w->dst[rl] == '/')) { rc = FS_EINTO_SELF; goto out; }
    if (!absp(w->a, w->rel, NULL) || !absp(w->t, w->dst, NULL)) goto out;
    const char *base = split(w->rel);
    bool same = !strcmp(w->rel, w->dst);                 /* already in that folder */
    if (!absp(w->b, w->dst, base)) goto out;
    activity();
    lock();
    struct stat st;
    if (stat(w->a, &st) || !is_dir(w->t)) rc = FS_ENOENT;
    else if (same) rc = FS_OK;
    else if (stat(w->b, &st) == 0) rc = FS_EEXIST;
    else { close_cat(); rc = rename(w->a, w->b) ? map_errno(errno) : FS_OK; }
    unlock();
    if (rc == FS_OK && !same) changed();
out:
    fs_inflight_leave();
    free(w);
    return rc;
}

/* Deletes everything below p (an absolute folder path in a FS_PATH_CAP buffer) and p itself, bottom-up without
 * recursion: read up to BATCH names in one lock section, delete the files one lock section each (catalog files
 * closed every time: the USB side may have opened one in between), descend into the first subfolder, rmdir a
 * folder once it reads empty and climb back. Stops with FS_EIO when a pass makes no progress. */
static int rm_tree(char *p, size_t base_len, uint32_t *files, bool *any) {
    char (*nm)[NAME_CAP] = malloc(BATCH * NAME_CAP);
    if (!nm) return FS_EIO;
    unsigned char type[BATCH];
    int rc = FS_OK;
    for (;;) {
        int n = 0;
        lock();
        DIR *d = opendir(p);
        if (!d) { rc = map_errno(errno); unlock(); break; }
        struct dirent *e;
        while (n < BATCH && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t l = strlen(e->d_name);
            if (l >= NAME_CAP) continue;
            memcpy(nm[n], e->d_name, l + 1);
            type[n++] = e->d_type;
        }
        closedir(d);
        unlock();
        size_t len = strlen(p);
        if (n == 0) {
            lock(); close_cat();
            int r = rmdir(p), er = errno;
            unlock();
            if (r) { rc = map_errno(er); if (rc == FS_ENOENT || rc == FS_EEXIST) rc = FS_EIO; break; }
            *any = true;
            if (len <= base_len) break;
            *strrchr(p, '/') = 0;                        /* climb back to the parent */
            continue;
        }
        bool progress = false;
        int sub = -1;
        for (int i = 0; i < n; i++) {
            if (len + 1 + strlen(nm[i]) >= FS_PATH_CAP) continue;
            p[len] = '/'; strcpy(p + len + 1, nm[i]);
            bool dir = type[i] == DT_DIR;
            if (type[i] == DT_UNKNOWN) { lock(); dir = is_dir(p); unlock(); }
            if (dir) { if (sub < 0) sub = i; }
            else {
                lock(); close_cat();
                int r = unlink(p);
                unlock();
                if (!r) { (*files)++; progress = *any = true; }
            }
            p[len] = 0;
        }
        if (sub >= 0) { p[len] = '/'; strcpy(p + len + 1, nm[sub]); continue; }
        if (!progress) { rc = FS_EIO; break; }
    }
    free(nm);
    return rc;
}

int fs_delete(const char *rel, uint32_t *removed_files) {
    uint32_t files = 0;
    if (removed_files) *removed_files = 0;
    if (!s_ready) return FS_EIO;
    fs_inflight_enter();
    work_t *w = malloc(sizeof *w);
    if (!w) { fs_inflight_leave(); return FS_EIO; }
    int rc = FS_EINVAL;
    bool any = false;
    if (!wc_norm_path(rel, w->rel, sizeof w->rel - 1) || !w->rel[0] || !absp(w->a, w->rel, NULL)) goto out;
    activity();
    lock();
    struct stat st;
    if (stat(w->a, &st)) { rc = FS_ENOENT; unlock(); goto out; }
    if (!S_ISDIR(st.st_mode)) {
        close_cat();
        rc = unlink(w->a) ? map_errno(errno) : FS_OK;
        unlock();
        if (rc == FS_OK) { files = 1; any = true; }
        goto out;
    }
    unlock();                                            /* a folder: one lock section per entry */
    rc = rm_tree(w->a, strlen(w->a), &files, &any);
out:
    if (any) changed();
    if (removed_files) *removed_files = files;
    fs_inflight_leave();
    free(w);
    return rc;
}

/* ---- uploads ---- */
struct fs_upload {
    int fd, err;
    size_t n;                                            /* bytes waiting in buf */
    char dir[WC_PATH_MAX];
    char name[NAME_CAP];
    char tmp[sizeof s_tmp + 16];
    uint8_t buf[FS_CHUNK];
};

int fs_upload_begin(const char *dir_rel, const char *name, uint64_t size_hint, fs_upload_t **out) {
    if (!out) return FS_EINVAL;
    *out = NULL;
    if (!s_ready) return FS_EIO;
    fs_inflight_enter();                                  /* the temp file stays open until commit/abort */
    fs_upload_t *u = malloc(sizeof *u);
    work_t *w = malloc(sizeof *w);
    int rc = FS_EIO;
    if (!u || !w) goto fail;
    rc = FS_EINVAL;
    if (!dir_rel || !wc_norm_path(dir_rel, u->dir, sizeof u->dir)) goto fail;
    if (!name || !wc_sd_safe_name(name, u->name, sizeof u->name)) goto fail;
    wc_conflict_name(u->name, FS_MAX_CONFLICT, w->name, sizeof w->name);   /* the longest name commit may try */
    if (!absp(w->a, u->dir, w->name) || !absp(w->b, u->dir, NULL)) goto fail;
    u->fd = -1; u->err = FS_OK; u->n = 0;
    activity();
    lock();
    uint64_t fr = s_h.free_bytes ? s_h.free_bytes() : UINT64_MAX;
    if (!is_dir(w->b)) rc = FS_ENOENT;
    else if (fr < FS_HEADROOM || size_hint > fr - FS_HEADROOM) rc = FS_ENOSPC;
    else {
        rc = FS_EIO;
        for (int i = 0; i < 8; i++) {                    /* O_EXCL: never share a temp file with another upload */
            snprintf(u->tmp, sizeof u->tmp, "%s/up-%08lx", s_tmp, (unsigned long)rnd());
            u->fd = open(u->tmp, O_CREAT | O_EXCL | O_WRONLY | O_TRUNC, 0664);
            if (u->fd >= 0) { rc = FS_OK; break; }
            if (errno != EEXIST) { rc = errno == ENOSPC ? FS_ENOSPC : FS_EIO; break; }
        }
    }
    unlock();
    if (rc) goto fail;
    free(w);
    *out = u;
    return FS_OK;
fail:
    fs_inflight_leave();
    free(u);
    free(w);
    return rc;
}

/* Writes what is buffered (at most FS_CHUNK) to the temp file in one lock section. */
static int flush(fs_upload_t *u) {
    if (!u->n || u->err) return u->err;
    size_t done = 0;
    lock();
    while (done < u->n) {
        ssize_t r = write(u->fd, u->buf + done, u->n - done);
        if (r < 0) { if (errno == EINTR) continue; u->err = errno == ENOSPC ? FS_ENOSPC : FS_EIO; break; }
        if (r == 0) { u->err = FS_ENOSPC; break; }       /* FatFs reports a full card as a short write */
        done += (size_t)r;
    }
    unlock();
    u->n = 0;
    return u->err;
}

int fs_upload_write(fs_upload_t *u, const void *data, size_t len) {
    if (!u) return FS_EINVAL;
    activity();                                          /* a long upload keeps the re-mount away */
    const uint8_t *p = data;
    while (len && !u->err) {
        size_t k = FS_CHUNK - u->n < len ? FS_CHUNK - u->n : len;
        memcpy(u->buf + u->n, p, k);
        u->n += k; p += k; len -= k;
        if (u->n == FS_CHUNK) flush(u);
    }
    return u->err;
}

#define PROBES_PER_HOLD 16
#define RECHECKS        4
/* Under the lock: w->name / w->b = candidate k ("name", "name (k)"); FS_OK when free, FS_EEXIST when taken. */
static int cand(work_t *w, const fs_upload_t *u, unsigned k) {
    if (k == 1) strcpy(w->name, u->name);
    else wc_conflict_name(u->name, k, w->name, sizeof w->name);
    if (!absp(w->b, u->dir, w->name)) return FS_EINVAL;
    fs_stat_probes++;
    struct stat st;
    return stat(w->b, &st) == 0 ? FS_EEXIST : FS_OK;
}

void fs_upload_abort(fs_upload_t *u) {
    if (!u) return;
    activity();
    lock();
    if (u->fd >= 0) close(u->fd);
    unlink(u->tmp);
    unlock();
    free(u);
    fs_inflight_leave();                                 /* the matching enter was in fs_upload_begin */
}

/* Writes what is buffered and closes the temp file (FatFs writes the size into the directory entry on close). */
static int finish_tmp(fs_upload_t *u, int rc) {
    if (rc == FS_OK) rc = flush(u);
    lock();
    if (close(u->fd) && rc == FS_OK) rc = FS_EIO;
    u->fd = -1;
    unlock();
    return rc;
}

static void set_mtime(const char *p, int64_t mtime) {
    struct utimbuf tb = { .actime = (time_t)mtime, .modtime = (time_t)mtime };
    (void)utime(p, &tb);                                 /* a wrong date is not worth failing the upload */
}

int fs_upload_commit(fs_upload_t *u, int64_t mtime, char *final_name, size_t fn_sz) {
    if (!u) return FS_EINVAL;
    if (final_name && fn_sz) final_name[0] = 0;
    activity();
    work_t *w = malloc(sizeof *w);
    int rc = finish_tmp(u, w ? FS_OK : FS_EIO);
    if (rc) goto fail;
    if (!mtime) mtime = now_s();
    /* First free "name (k)": probed PROBES_PER_HOLD names per lock hold (each stat is a linear FAT directory scan,
     * so a folder full of "(n)" siblings must not stall READ10s), then one hold for close -> re-check a few ->
     * rename -> utime. A name taken in between (another commit) only moves the search on. */
    rc = FS_EEXIST;
    for (unsigned k = 1; k <= FS_MAX_CONFLICT && rc == FS_EEXIST;) {
        lock();
        for (unsigned end = k + PROBES_PER_HOLD; k < end && k <= FS_MAX_CONFLICT; k++) {
            rc = cand(w, u, k);
            if (rc != FS_EEXIST) break;
        }
        unlock();
        if (rc != FS_OK) continue;                       /* EEXIST: next batch; EINVAL: loop ends */
        lock();
        close_cat();
        rc = FS_EEXIST;
        for (unsigned end = k + RECHECKS; k < end && k <= FS_MAX_CONFLICT; k++) {
            rc = cand(w, u, k);
            if (rc != FS_EEXIST) break;
        }
        if (rc == FS_OK) {
            rc = rename(u->tmp, w->b) ? map_errno(errno) : FS_OK;
            if (rc == FS_OK) set_mtime(w->b, mtime);
            unlock();
            break;
        }
        unlock();
    }
    if (rc) goto fail;
    changed();
    if (final_name && fn_sz) snprintf(final_name, fn_sz, "%s", w->name);
    free(u);
    free(w);
    fs_inflight_leave();
    return FS_OK;
fail:
    fs_upload_abort(u);                                  /* fd already closed: removes the temp file */
    free(w);
    return rc;
}

int fs_upload_commit_replace(fs_upload_t *u, int64_t mtime, bool *replaced) {
    if (replaced) *replaced = false;
    if (!u) return FS_EINVAL;
    activity();
    work_t *w = malloc(sizeof *w);
    int rc = finish_tmp(u, w ? FS_OK : FS_EIO);
    if (rc == FS_OK && (!absp(w->b, u->dir, u->name) || snprintf(w->t, sizeof w->t, "%s-old", u->tmp) >= (int)sizeof w->t))
        rc = FS_EINVAL;
    if (rc) goto fail;
    if (!mtime) mtime = now_s();
    bool had = false;
    lock();
    close_cat();
    struct stat st;
    if (stat(w->b, &st) == 0) {
        if (S_ISDIR(st.st_mode)) rc = FS_EEXIST;
        else if (rename(w->b, w->t)) rc = map_errno(errno);   /* aside, so a failed rename can put it back */
        else had = true;
    }
    if (rc == FS_OK) {
        rc = rename(u->tmp, w->b) ? map_errno(errno) : FS_OK;
        if (rc == FS_OK) {
            set_mtime(w->b, mtime);
            if (had) (void)unlink(w->t);                 /* a leftover "up-*-old" goes at the next boot */
        } else if (had && rename(w->t, w->b) == 0) {
            had = false;                                 /* put back: the destination is as it was */
        }
    }
    unlock();
    /* changed() only when the volume is what the caller asked for: a failed put-back leaves the destination missing
     * (the old bytes are in the parked up-*-old, removed by fs_cleanup_tmp / prepare_sd at the next boot), and
     * telling the machine to re-read a directory the web just broke would make it show a vanished file. */
    if (rc == FS_OK) changed();
    if (rc) goto fail;
    if (replaced) *replaced = had;
    free(u);
    free(w);
    fs_inflight_leave();
    return FS_OK;
fail:
    fs_upload_abort(u);
    free(w);
    return rc;
}

void fs_cleanup_tmp(void) {
    if (!s_ready) return;
    char (*nm)[NAME_CAP] = malloc(BATCH * NAME_CAP);
    char *p = malloc(FS_PATH_CAP);
    if (!nm || !p) { free(nm); free(p); return; }
    activity();
    for (;;) {                                           /* one batch per lock section; stop when nothing goes */
        int n = 0, gone = 0;
        lock();
        DIR *d = opendir(s_tmp);
        if (d) {
            struct dirent *e;
            while (n < BATCH && (e = readdir(d)))
                if (!strncmp(e->d_name, "up-", 3) && strlen(e->d_name) < NAME_CAP) strcpy(nm[n++], e->d_name);
            closedir(d);
        }
        for (int i = 0; i < n; i++) {
            snprintf(p, FS_PATH_CAP, "%s/%s", s_tmp, nm[i]);
            if (!unlink(p)) gone++;
        }
        unlock();
        if (!n || !gone) break;
    }
    free(nm);
    free(p);
}
