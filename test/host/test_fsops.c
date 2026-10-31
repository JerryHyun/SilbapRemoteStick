#define _GNU_SOURCE
#include "t.h"
#include "fsops.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int locks, unlocks, closes, changes, acts; static uint64_t free_b = 1 << 30;
static uint32_t probes_at_lock, max_probes_per_hold;   /* fs_stat_probes counted per lock hold */
static void lk(void){locks++; probes_at_lock = fs_stat_probes;}
static const char *gap_watch; static int gap_seen; static int exists(const char *rel);
static void ul(void){unlocks++; if (gap_watch && !exists(gap_watch)) gap_seen++; uint32_t d = fs_stat_probes - probes_at_lock; if (d > max_probes_per_hold) max_probes_per_hold = d;} static void cl(void){closes++;} static void ch(void){changes++;} static void ac(void){acts++;}
static uint64_t fb(void){return free_b;} static int64_t nw(void){return 1761800000;}
static const fs_hooks_t H = { lk, ul, cl, ch, ac, fb, nw };

static char root[256], tmpd[300];
static void rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e; char c[1024];
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(c, sizeof c, "%s/%s", p, e->d_name); rm_rf(c);
        }
        if (d) closedir(d);
        rmdir(p);
    } else unlink(p);
}
static void fresh(void) {
    if (root[0]) rm_rf(root);
    snprintf(root, sizeof root, "/tmp/fsops-XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    char s[300]; snprintf(s, sizeof s, "%s/.silbap", root); mkdir(s, 0775);
    snprintf(tmpd, sizeof tmpd, "%s/.silbap/tmp", root); mkdir(tmpd, 0775);
    fs_init(root, tmpd, &H);
}
static void ap(char *o, size_t n, const char *rel) { snprintf(o, n, "%s/%s", root, rel); }
static void mkf(const char *rel, size_t size) {
    char p[1024]; ap(p, sizeof p, rel);
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); exit(2); }
    for (size_t i = 0; i < size; i++) fputc('a' + (int)(i % 26), f);
    fclose(f);
}
static int exists(const char *rel) { char p[1024]; ap(p, sizeof p, rel); struct stat st; return stat(p, &st) == 0; }
static int exists_exact(const char *rel) {
    char p[1024]; ap(p, sizeof p, rel);
    char *slash = strrchr(p, '/'); *slash = 0;
    DIR *d = opendir(p); struct dirent *e; int found = 0;
    while (d && (e = readdir(d))) if (!strcmp(e->d_name, slash + 1)) found = 1;
    if (d) closedir(d);
    return found;
}
static int count_dir(const char *abs, const char *prefix) {
    DIR *d = opendir(abs); struct dirent *e; int n = 0;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!prefix || !strncmp(e->d_name, prefix, strlen(prefix))) n++;
    }
    if (d) closedir(d);
    return n;
}
static int count_tmp(void) { return count_dir(tmpd, NULL); }
static long long file_mtime(const char *rel) { char p[1024]; ap(p, sizeof p, rel); struct stat st; return stat(p, &st) ? -1 : (long long)st.st_mtime; }
static long long file_size(const char *rel) { char p[1024]; ap(p, sizeof p, rel); struct stat st; return stat(p, &st) ? -1 : (long long)st.st_size; }

/* ---- brief tests ---- */
static void test_close_before_mutate(void) {
    fresh(); mkf("a.jef", 10);
    closes = changes = acts = 0;
    T_EQ_INT(fs_delete("a.jef", NULL), FS_OK); T_EQ_INT(closes, 1); T_EQ_INT(changes, 1);
    mkf("b.jef", 1); T_EQ_INT(fs_rename("b.jef", "c.jef"), FS_OK); T_EQ_INT(closes, 2);
    T_EQ_INT(fs_mkdir("d"), FS_OK); T_EQ_INT(fs_move("c.jef", "d"), FS_OK); T_EQ_INT(closes, 3);  /* mkdir doesn't close */
    T_CHECK(exists("d/c.jef"));
    T_EQ_INT(acts, 4);                                /* delete, rename, mkdir, move: every mutation stamps */
}
/* The activity hook is optional: a host build without one must still work. */
static void test_activity_hook_optional(void) {
    fresh(); fs_hooks_t h = H; h.activity = NULL;
    fs_init(root, tmpd, &h);
    mkf("a.jef", 1);
    T_EQ_INT(fs_delete("a.jef", NULL), FS_OK); T_EQ_INT(fs_mkdir("q"), FS_OK);
}
static void test_upload_abort_leaves_no_file(void) {
    fresh(); fs_upload_t *u; T_EQ_INT(fs_upload_begin("", "x.jef", 100, &u), FS_OK);
    fs_upload_write(u, "hello", 5); fs_upload_abort(u);
    T_CHECK(!exists("x.jef")); T_EQ_INT(count_tmp(), 0);
    T_EQ_INT(fs_upload_begin("", "y.jef", 0, &u), FS_OK); fs_upload_write(u, "z", 1);   /* simulate power loss: no abort */
    fs_cleanup_tmp(); T_EQ_INT(count_tmp(), 0); T_CHECK(!exists("y.jef"));
    fs_upload_abort(u);   /* (test only: release the handle; its temp file is already gone) */
}
static void test_upload_conflict_and_space(void) {
    fresh(); mkf("rose.jef", 1); fs_upload_t *u; char fn[300];
    T_EQ_INT(fs_upload_begin("", "rose.jef", 3, &u), FS_OK); fs_upload_write(u, "abc", 3);
    T_EQ_INT(fs_upload_commit(u, 1761800000, fn, sizeof fn), FS_OK); T_EQ_STR(fn, "rose (2).jef");
    T_EQ_INT(file_mtime("rose (2).jef"), 1761800000);
    free_b = 2 << 20; T_EQ_INT(fs_upload_begin("", "big.dst", 2 << 20, &u), FS_ENOSPC);   /* needs 1 MiB headroom */
    free_b = 1 << 30;
}
static void test_upload_odd_names(void) {
    fresh(); fs_upload_t *u; char fn[300];
    T_EQ_INT(fs_upload_begin("", "a:b?.jef", 1, &u), FS_OK); fs_upload_write(u, "x", 1);
    T_EQ_INT(fs_upload_commit(u, 0, fn, sizeof fn), FS_OK); T_EQ_STR(fn, "a：b？.jef");
    char big[300]; memset(big, 'n', 299); big[299] = 0; T_EQ_INT(fs_upload_begin("", big, 1, &u), FS_EINVAL);
    T_EQ_INT(fs_move("", "x"), FS_EINVAL);
}
static void test_move_into_self(void) {
    fresh(); fs_mkdir("p"); fs_mkdir("p/q"); T_EQ_INT(fs_move("p", "p/q"), FS_EINTO_SELF); T_EQ_INT(fs_move("p", "p"), FS_EINTO_SELF);
}
static void test_case_only_rename(void) {
    fresh(); mkf("rose.jef", 1); T_EQ_INT(fs_rename("rose.jef", "Rose.jef"), FS_OK); T_CHECK(exists_exact("Rose.jef"));
}

/* ---- extra coverage ---- */
static void test_upload_chunks_and_date(void) {
    fresh(); fs_upload_t *u; char fn[300];
    static unsigned char buf[40000];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)(i * 7);
    locks = unlocks = closes = changes = 0;
    T_EQ_INT(fs_upload_begin("", "big.jef", sizeof buf, &u), FS_OK);
    int l0 = locks;
    for (size_t off = 0; off < sizeof buf; off += 1000) T_EQ_INT(fs_upload_write(u, buf + off, 1000), FS_OK);
    T_EQ_INT(locks - l0, 2);                         /* two full 16 KiB chunks written, 7232 bytes still buffered */
    T_EQ_INT(closes, 0); T_EQ_INT(changes, 0);       /* temp writes never close catalog files */
    T_EQ_INT(fs_upload_commit(u, 0, fn, sizeof fn), FS_OK); T_EQ_STR(fn, "big.jef");
    T_EQ_INT(closes, 1); T_EQ_INT(changes, 1); T_EQ_INT(locks, unlocks);
    T_EQ_INT(file_size("big.jef"), (long long)sizeof buf);
    T_EQ_INT(file_mtime("big.jef"), 1761800000);     /* mtime 0 = upload time (hooks->now) */
    char p[1024]; ap(p, sizeof p, "big.jef");
    FILE *f = fopen(p, "rb"); static unsigned char back[40000];
    T_EQ_INT(f ? fread(back, 1, sizeof back, f) : 0, sizeof back); if (f) fclose(f);
    T_CHECK(!memcmp(back, buf, sizeof buf));
    T_EQ_INT(count_tmp(), 0);
}
static void test_upload_commit_replace(void) {
    fresh(); fs_upload_t *u; bool rep = true;
    mkf("rose.jef", 10);
    locks = unlocks = closes = changes = 0;
    T_EQ_INT(fs_upload_begin("", "rose.jef", 5, &u), FS_OK);
    T_EQ_INT(fs_upload_write(u, "hello", 5), FS_OK);
    gap_watch = "rose.jef"; gap_seen = 0;
    T_EQ_INT(fs_upload_commit_replace(u, 1700000000, &rep), FS_OK);
    gap_watch = NULL;
    T_CHECK(rep); T_EQ_INT(gap_seen, 0);            /* delete + rename in one lock hold: never seen missing */
    T_EQ_INT(file_size("rose.jef"), 5); T_EQ_INT(file_mtime("rose.jef"), 1700000000);
    T_EQ_INT(closes, 1); T_EQ_INT(changes, 1); T_EQ_INT(locks, unlocks);
    T_EQ_INT(count_dir(root, "rose"), 1); T_EQ_INT(count_tmp(), 0);   /* no "rose (2).jef" */
    /* a new name: created, not replaced; mtime 0 = now */
    T_EQ_INT(fs_upload_begin("", "new.jef", 0, &u), FS_OK);
    T_EQ_INT(fs_upload_commit_replace(u, 0, &rep), FS_OK);
    T_CHECK(!rep); T_EQ_INT(file_size("new.jef"), 0); T_EQ_INT(file_mtime("new.jef"), 1761800000);
    /* a folder of that name is never replaced */
    T_EQ_INT(fs_mkdir("dir.jef"), FS_OK); mkf("dir.jef/in.jef", 3);
    T_EQ_INT(fs_upload_begin("", "dir.jef", 1, &u), FS_OK); fs_upload_write(u, "x", 1);
    closes = 0;
    T_EQ_INT(fs_upload_commit_replace(u, 0, NULL), FS_EEXIST);
    T_CHECK(exists("dir.jef/in.jef")); T_EQ_INT(count_tmp(), 0);
    /* a dead upload reports its error and cleans up */
    T_EQ_INT(fs_upload_commit_replace(NULL, 0, &rep), FS_EINVAL);
}
static void test_upload_into_folder_and_missing(void) {
    fresh(); fs_upload_t *u; char fn[300];
    T_EQ_INT(fs_mkdir("EMB"), FS_OK);
    T_EQ_INT(fs_upload_begin("EMB", "a.jef", 1, &u), FS_OK); fs_upload_write(u, "x", 1);
    T_EQ_INT(fs_upload_commit(u, 0, fn, sizeof fn), FS_OK); T_CHECK(exists("EMB/a.jef"));
    T_EQ_INT(fs_upload_begin("nope", "a.jef", 1, &u), FS_ENOENT);
    T_EQ_INT(fs_upload_begin("../x", "a.jef", 1, &u), FS_EINVAL);
    T_EQ_INT(fs_upload_begin("", ".silbap", 1, &u), FS_EINVAL);
    free_b = 512 * 1024; T_EQ_INT(fs_upload_begin("", "u.jef", 0, &u), FS_ENOSPC);   /* unknown size: still 1 MiB headroom */
    free_b = 1 << 30;
}
static void test_conflict_many_and_cleanup_keeps(void) {
    fresh(); fs_upload_t *u; char fn[300];
    mkf("r.jef", 1); mkf("r (2).jef", 1); mkf("r (3).jef", 1);
    T_EQ_INT(fs_upload_begin("", "r.jef", 1, &u), FS_OK); fs_upload_write(u, "x", 1);
    T_EQ_INT(fs_upload_commit(u, 0, fn, sizeof fn), FS_OK); T_EQ_STR(fn, "r (4).jef");
    char p[600]; snprintf(p, sizeof p, "%s/catalog.new", tmpd); FILE *f = fopen(p, "w"); fclose(f);
    snprintf(p, sizeof p, "%s/names.new", tmpd); f = fopen(p, "w"); fclose(f);
    fs_upload_t *u2;
    T_EQ_INT(fs_upload_begin("", "s.jef", 0, &u), FS_OK);
    T_EQ_INT(fs_upload_begin("", "t.jef", 0, &u2), FS_OK);   /* two leftovers */
    T_EQ_INT(count_dir(tmpd, "up-"), 2);
    fs_cleanup_tmp(); T_EQ_INT(count_dir(tmpd, "up-"), 0); T_EQ_INT(count_tmp(), 2);   /* catalog.new / names.new stay */
    fs_upload_abort(u); fs_upload_abort(u2);
}
static void test_conflict_probe_holds(void) {
    fresh(); fs_upload_t *u; char fn[300];
    mkf("rose.jef", 1);
    for (int k = 2; k <= 40; k++) { char n[64]; snprintf(n, sizeof n, "rose (%d).jef", k); mkf(n, 1); }   /* 40 names taken */
    T_EQ_INT(fs_upload_begin("", "rose.jef", 1, &u), FS_OK); fs_upload_write(u, "x", 1);
    uint32_t p0 = fs_stat_probes; max_probes_per_hold = 0; closes = 0;
    T_EQ_INT(fs_upload_commit(u, 0, fn, sizeof fn), FS_OK); T_EQ_STR(fn, "rose (41).jef");
    T_CHECK(fs_stat_probes - p0 >= 41);                /* the search really probed */
    T_CHECK(max_probes_per_hold <= 16);                /* USB reads get the card back at least every 16 probes */
    T_EQ_INT(closes, 1); T_EQ_INT(locks, unlocks);
    T_CHECK(exists("rose (41).jef")); T_EQ_INT(count_tmp(), 0);
}
static void test_rename_collision_keeps_both(void) {
    fresh(); mkf("a.jef", 1); mkf("A.jef", 2);         /* case-sensitive host: two entries */
    T_EQ_INT(fs_rename("a.jef", "A.jef"), FS_EEXIST);
    T_EQ_INT(file_size("a.jef"), 1); T_EQ_INT(file_size("A.jef"), 2);
    T_EQ_INT(count_dir(root, ".silbap-mv"), 0);        /* no temp names left in the folder */
}
static void test_rename_move_errors(void) {
    fresh(); mkf("a.jef", 1); mkf("b.jef", 1); fs_mkdir("d"); mkf("d/a.jef", 1);
    closes = changes = 0;
    T_EQ_INT(fs_rename("a.jef", "b.jef"), FS_EEXIST); T_CHECK(exists("a.jef"));
    T_EQ_INT(fs_move("a.jef", "d"), FS_EEXIST); T_CHECK(exists("a.jef"));
    T_EQ_INT(fs_rename("zz.jef", "y.jef"), FS_ENOENT);
    T_EQ_INT(fs_move("a.jef", "nope"), FS_ENOENT);
    T_EQ_INT(fs_move("a.jef", "b.jef"), FS_ENOENT);   /* destination is not a folder */
    T_EQ_INT(fs_rename("a.jef", "..."), FS_EINVAL);
    T_EQ_INT(fs_rename("", "x"), FS_EINVAL);
    T_EQ_INT(fs_delete("", NULL), FS_EINVAL);
    T_EQ_INT(fs_delete("nope", NULL), FS_ENOENT);
    T_EQ_INT(changes, 0);                              /* nothing changed */
    T_EQ_INT(fs_rename("a.jef", "a.jef"), FS_OK);      /* same name: nothing to do */
    T_EQ_INT(fs_move("a.jef", ""), FS_OK);             /* already there */
    T_EQ_INT(changes, 0);
    T_EQ_INT(fs_rename("a.jef", "x:y.jef"), FS_OK); T_CHECK(exists("x：y.jef"));
    T_EQ_INT(fs_mkdir("d"), FS_EEXIST); T_EQ_INT(fs_mkdir("q/r"), FS_ENOENT);
    T_EQ_INT(fs_mkdir("n?"), FS_OK); T_CHECK(exists("n？"));
    T_EQ_INT(fs_move("d", "n？"), FS_OK); T_CHECK(exists("n？/d/a.jef"));
    T_EQ_INT(fs_rename("n？/d", "e"), FS_OK); T_CHECK(exists("n？/e/a.jef"));
    T_EQ_INT(locks, unlocks);
}
static void test_folder_delete_counts(void) {
    fresh(); fs_mkdir("f"); fs_mkdir("f/g"); fs_mkdir("f/g/h"); fs_mkdir("f/empty");
    for (int i = 0; i < 40; i++) { char n[64]; snprintf(n, sizeof n, "f/%02d.jef", i); mkf(n, 3); }
    mkf("f/g/1.jef", 1); mkf("f/g/h/2.jef", 1); mkf("f/g/h/3.jef", 1); mkf("keep.jef", 1);
    uint32_t n = 0; changes = 0;
    T_EQ_INT(fs_delete("f", &n), FS_OK); T_EQ_INT(n, 43); T_CHECK(!exists("f")); T_CHECK(exists("keep.jef"));
    T_EQ_INT(changes, 1); T_EQ_INT(locks, unlocks);
}

int main(void) {
    T_RUN(test_close_before_mutate);
    T_RUN(test_activity_hook_optional);
    T_RUN(test_upload_abort_leaves_no_file);
    T_RUN(test_upload_conflict_and_space);
    T_RUN(test_upload_odd_names);
    T_RUN(test_move_into_self);
    T_RUN(test_case_only_rename);
    T_RUN(test_upload_chunks_and_date);
    T_RUN(test_upload_into_folder_and_missing);
    T_RUN(test_upload_commit_replace);
    T_RUN(test_conflict_many_and_cleanup_keeps);
    T_RUN(test_conflict_probe_holds);
    T_RUN(test_rename_collision_keeps_both);
    T_RUN(test_rename_move_errors);
    T_RUN(test_folder_delete_counts);
    if (root[0]) rm_rf(root);
    T_DONE();
}
