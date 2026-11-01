/* Files REST API: list / count / mkdir / rename / move / delete / upload. All paths are relative to SD_MOUNT and go
 * through wc_norm_path; names a client chooses go through wc_sd_safe_name first so the final name can be returned.
 * esp_http_server runs every handler on its one task, so the static buffers below are never used concurrently. */
#include "api_files.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "api_list_core.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "fsops.h"
#include "fs_names.h"
#include "led.h"
#include "refresh.h"
#include "sdcard.h"
#include "timekeep.h"
#include "web.h"
#include "webcore.h"

static const char *TAG = "api_files";
static settings_t *s_set;

#define NAME_BUF   300             /* a decoded name: <= 255 bytes plus slack to detect "too long" */
#define BODY_MAX   2048
#define QUERY_MAX  640
#define ENTRY_BUF  3300            /* worst case: 255-byte name and machine name, every byte escaped as \u00XX */

static char s_body[BODY_MAX];
static char s_ent[ENTRY_BUF];
static char s_cpath[sizeof SD_MOUNT + WC_PATH_MAX + 2 + NAME_BUF];

/* ---- helpers ---- */
static esp_err_t fail(httpd_req_t *req, int st, const char *key) { return web_error(req, st, key); }

/* Maps an fsops result to a response. einval: the error key for FS_EINVAL (bad_name or bad_path depending on call). */
static esp_err_t fs_fail(httpd_req_t *req, int rc, const char *einval) {
    switch (rc) {
    case FS_EEXIST:     return fail(req, 409, "err.exists");
    case FS_ENOENT:     return fail(req, 404, "err.not_found");
    case FS_ENOSPC:     return fail(req, 507, "err.no_space");
    case FS_EINVAL:     return fail(req, 400, einval);
    case FS_EINTO_SELF: return fail(req, 409, "err.into_self");
    default:            return fail(req, 500, "err.io");
    }
}

static esp_err_t s_body_rc;
static bool read_body(httpd_req_t *req) { return web_read_body(req, s_body, sizeof s_body, "err.bad_path", &s_body_rc); }

/* Normalises a raw (decoded) path into out (WC_PATH_MAX+1). */
static bool norm(const char *raw, char *out) { return wc_norm_path(raw, out, WC_PATH_MAX + 1); }

/* Joins dir + "/" + name ("" dir = root) and normalises; false when the result is rejected (e.g. ".silbap*"). */
static bool join_norm(const char *dir, const char *name, char *out) {
    char t[WC_PATH_MAX + NAME_BUF + 2];
    int n = snprintf(t, sizeof t, "%s%s%s", dir, dir[0] ? "/" : "", name);
    if (n < 0 || (size_t)n >= sizeof t) return false;
    return norm(t, out);
}

static void full_path(const char *rel, char *out, size_t sz) {
    snprintf(out, sz, "%s%s%s", SD_MOUNT, rel[0] ? "/" : "", rel);
}

static bool query_str(httpd_req_t *req, char *q, size_t sz) {
    q[0] = 0;
    esp_err_t e = httpd_req_get_url_query_str(req, q, sz);
    return e == ESP_OK || e == ESP_ERR_NOT_FOUND;
}

/* Normalised path from a decoded raw value; sends 400 bad_path and returns false when invalid, or absent while
 * required. An absent optional path is the root. */
static bool take_path(httpd_req_t *req, api_get_t g, const char *raw, bool required, char *rel) {
    if (g == API_INVALID || (g == API_ABSENT && required)) { fail(req, 400, "err.bad_path"); return false; }
    if (g == API_ABSENT) raw = "";
    if (!norm(raw, rel)) { fail(req, 400, "err.bad_path"); return false; }
    return true;
}
static bool query_path(httpd_req_t *req, const char *q, const char *key, bool required, char *rel) {
    char raw[WC_PATH_MAX + 4];
    api_get_t g = api_query_get(q, key, raw, sizeof raw);
    return take_path(req, g, raw, required, rel);
}
static bool body_path(httpd_req_t *req, const char *key, bool required, char *rel) {
    char raw[WC_PATH_MAX + 4];
    api_get_t g = api_json_get_str(s_body, key, raw, sizeof raw);
    return take_path(req, g, raw, required, rel);
}

static int path_comps(const char *rel) {
    if (!rel[0]) return 0;
    int n = 1;
    for (; *rel; rel++) if (*rel == '/') n++;
    return n;
}

typedef struct { char name[NAME_BUF]; bool dir; uint64_t size; int64_t mtime; } ent_t;

/* One readdir step under the SD lock (the lock is never held while sending). 1 = entry, 0 = end. */
static int dir_next(DIR *d, const char *full_dir, ent_t *e) {
    int rc = 0;
    sdcard_lock();
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (strlen(de->d_name) >= sizeof e->name) continue;
        strcpy(e->name, de->d_name);
        char p[sizeof s_cpath];
        snprintf(p, sizeof p, "%s/%s", full_dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        e->dir = S_ISDIR(st.st_mode);
        e->size = e->dir ? 0 : (uint64_t)st.st_size;
        e->mtime = (int64_t)st.st_mtime;
        rc = 1;
        break;
    }
    sdcard_unlock();
    return rc;
}

/* ---- GET /api/list ---- */
static esp_err_t list_get(httpd_req_t *req) {
    char q[QUERY_MAX], rel[WC_PATH_MAX + 1], hid[8] = "";
    if (!query_str(req, q, sizeof q)) return fail(req, 400, "err.bad_path");
    if (!query_path(req, q, "path", false, rel)) return ESP_OK;
    if (api_query_get(q, "hidden", hid, sizeof hid) != API_OK) hid[0] = 0;
    bool show_hidden = hid[0] == '1';
    char *full = s_cpath;
    full_path(rel, full, sizeof s_cpath);
    sdcard_lock();
    DIR *d = opendir(full);
    sdcard_unlock();
    if (!d) return fail(req, errno == ENOENT || errno == ENOTDIR ? 404 : 500, errno == ENOENT || errno == ENOTDIR ? "err.not_found" : "err.io");

    refresh_status_t rs;
    refresh_status(&rs);
    int depth = path_comps(rel) + 1;
    bool rename = s_set->rename;
    nm_lang_t lang = s_set->lang;

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    wc_json_t w;
    char *head = s_ent;   /* free until the first entry is built */
    wc_json_init(&w, head, sizeof s_ent);
    wc_json_obj_open(&w);
    wc_json_str(&w, "path", rel);
    wc_json_arr_open(&w, "entries");
    bool ok = !w.overflow && httpd_resp_send_chunk(req, head, (ssize_t)w.len) == ESP_OK;
    bool first = true;
    ent_t *e = malloc(sizeof *e);
    if (!e) ok = false;
    while (ok && dir_next(d, full, e)) {
        if (!strncmp(e->name, ".silbap", 7)) continue;   /* never exposed */
        if (!show_hidden && fs_hidden_name(e->name)) continue;
        wc_json_init(&w, s_ent + 1, sizeof s_ent - 1);
        wc_json_obj_open(&w);
        api_list_entry_fields(e->name, e->dir, e->size, depth, rename, lang, &w);
        wc_json_num(&w, "mtime", e->mtime);
        if (rs.last_wall && e->mtime > rs.last_wall) wc_json_bool(&w, "pending", true);
        wc_json_obj_close(&w);
        if (w.overflow) continue;
        s_ent[0] = ',';
        const char *p = first ? s_ent + 1 : s_ent;
        ok = httpd_resp_send_chunk(req, p, (ssize_t)(w.len + (first ? 0 : 1))) == ESP_OK;
        first = false;
    }
    free(e);
    sdcard_lock();
    closedir(d);
    sdcard_unlock();
    if (!ok) { httpd_resp_send_chunk(req, NULL, 0); return ESP_FAIL; }
    uint64_t total = 0, fr = 0;
    sdcard_lock();
    if (esp_vfs_fat_info(SD_MOUNT, &total, &fr) != ESP_OK) total = fr = 0;
    sdcard_unlock();
    char tail[120];
    int n = snprintf(tail, sizeof tail, "],\"free\":%llu,\"total\":%llu,\"hidden\":%u}", (unsigned long long)fr,
                     (unsigned long long)total, 0u);
    if (httpd_resp_send_chunk(req, tail, n) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- GET /api/count ----
 * Counts exactly what /api/list shows (the same rules refresh.c's header walk uses): "." and ".." skipped, names
 * starting with ".silbap" skipped (never exposed), fs_hidden_name() names skipped. Without them the delete
 * confirmation would promise to remove entries the listing never showed. */
typedef struct { uint32_t files, dirs; } counts_t;
#define COUNT_MAX_DEPTH 24
enum { CNT_OK = 0, CNT_DEEP, CNT_IO };
static int count_dir(size_t len, int depth, counts_t *c) {
    if (depth > COUNT_MAX_DEPTH) return CNT_DEEP;
    DIR *d;
    sdcard_lock();
    d = opendir(s_cpath);
    sdcard_unlock();
    if (!d) return CNT_IO;
    int ok = CNT_OK;
    ent_t *e = malloc(sizeof *e);
    if (!e) ok = CNT_IO;
    while (ok == CNT_OK && dir_next(d, s_cpath, e)) {
        if (!strncmp(e->name, ".silbap", 7)) continue;   /* never exposed */
        if (fs_hidden_name(e->name)) continue;           /* the listing hides these too */
        if (e->dir) {
            c->dirs++;
            size_t nl = strlen(e->name);
            if (len + 1 + nl >= sizeof s_cpath) { ok = CNT_DEEP; break; }
            s_cpath[len] = '/';
            memcpy(s_cpath + len + 1, e->name, nl + 1);
            ok = count_dir(len + 1 + nl, depth + 1, c);
            s_cpath[len] = 0;
        } else c->files++;
    }
    free(e);
    sdcard_lock();
    closedir(d);
    sdcard_unlock();
    return ok;
}

static esp_err_t count_get(httpd_req_t *req) {
    char q[QUERY_MAX], rel[WC_PATH_MAX + 1];
    if (!query_str(req, q, sizeof q)) return fail(req, 400, "err.bad_path");
    if (!query_path(req, q, "path", false, rel)) return ESP_OK;
    full_path(rel, s_cpath, sizeof s_cpath);
    struct stat st;
    sdcard_lock();
    int sr = stat(s_cpath, &st);
    sdcard_unlock();
    if (sr != 0) return fail(req, 404, "err.not_found");
    counts_t c = {0, 0};
    if (S_ISDIR(st.st_mode)) {
        int cr = count_dir(strlen(s_cpath), 0, &c);
        if (cr == CNT_DEEP) return fail(req, 400, "err.too_deep");
        if (cr != CNT_OK) return fail(req, 500, "err.io");
    } else c.files = 1;
    char b[64];
    snprintf(b, sizeof b, "{\"files\":%u,\"dirs\":%u}", (unsigned)c.files, (unsigned)c.dirs);
    return web_json(req, 200, b);
}

/* ---- POST bodies ---- */
/* The reply carries a name of up to NAME_BUF bytes with every byte escaped as \u00XX (6 bytes). It is static for the
 * same reason as s_up below: esp_http_server runs every handler on its one task, so no two replies exist at once.
 * As a local this frame was 1888 bytes, added on top of the caller's (upload_post's own frame was 7264). */
static char s_name_buf[NAME_BUF * 6 + 32];

static esp_err_t name_reply(httpd_req_t *req, const char *name, bool existed) {
    wc_json_t w;
    wc_json_init(&w, s_name_buf, sizeof s_name_buf);
    wc_json_obj_open(&w);
    wc_json_str(&w, "name", name);
    if (existed) wc_json_bool(&w, "existed", true);
    wc_json_obj_close(&w);
    if (w.overflow) return fail(req, 500, "err.io");
    return web_json(req, 200, s_name_buf);
}

static esp_err_t mkdir_post(httpd_req_t *req) {
    if (!read_body(req)) return s_body_rc;
    char name[NAME_BUF], safe[NAME_BUF], dir[WC_PATH_MAX + 1], rel[WC_PATH_MAX + 1];
    if (!body_path(req, "path", false, dir)) return ESP_OK;
    if (api_json_get_str(s_body, "name", name, sizeof name) != API_OK || !wc_sd_safe_name(name, safe, sizeof safe)
        || !join_norm(dir, safe, rel))
        return fail(req, 400, "err.bad_name");
    int rc = fs_mkdir(rel);
    if (rc == FS_EEXIST) {   /* an existing folder is fine: the client uploads into it under the sanitized name */
        struct stat st;
        full_path(rel, s_cpath, sizeof s_cpath);
        sdcard_lock();
        int sr = stat(s_cpath, &st);
        sdcard_unlock();
        if (sr == 0 && S_ISDIR(st.st_mode)) return name_reply(req, safe, true);
    }
    if (rc != FS_OK) return fs_fail(req, rc, "err.bad_name");
    return name_reply(req, safe, false);
}

static esp_err_t rename_post(httpd_req_t *req) {
    if (!read_body(req)) return s_body_rc;
    char name[NAME_BUF], safe[NAME_BUF], rel[WC_PATH_MAX + 1], chk[WC_PATH_MAX + 1], dir[WC_PATH_MAX + 1];
    if (!body_path(req, "path", true, rel)) return ESP_OK;
    if (!rel[0]) return fail(req, 400, "err.bad_path");
    if (api_json_get_str(s_body, "name", name, sizeof name) != API_OK || !wc_sd_safe_name(name, safe, sizeof safe))
        return fail(req, 400, "err.bad_name");
    strcpy(dir, rel);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0; else dir[0] = 0;
    if (!join_norm(dir, safe, chk)) return fail(req, 400, "err.bad_name");   /* chk is only a validity check (.silbap*) */
    int rc = fs_rename(rel, safe);
    if (rc != FS_OK) return fs_fail(req, rc, "err.bad_name");
    return name_reply(req, safe, false);
}

static esp_err_t move_post(httpd_req_t *req) {
    if (!read_body(req)) return s_body_rc;
    char rel[WC_PATH_MAX + 1], to[WC_PATH_MAX + 1];
    if (!body_path(req, "path", true, rel)) return ESP_OK;
    if (!rel[0]) return fail(req, 400, "err.bad_path");
    if (!body_path(req, "to", true, to)) return ESP_OK;   /* "" (root) must be sent explicitly */
    int rc = fs_move(rel, to);
    if (rc != FS_OK) return fs_fail(req, rc, "err.bad_path");
    return web_json(req, 200, "{}");
}

static esp_err_t delete_post(httpd_req_t *req) {
    if (!read_body(req)) return s_body_rc;
    char rel[WC_PATH_MAX + 1];
    if (!body_path(req, "path", true, rel)) return ESP_OK;
    if (!rel[0]) return fail(req, 400, "err.bad_path");
    uint32_t n = 0;
    int rc = fs_delete(rel, &n);
    if (rc != FS_OK) return fs_fail(req, rc, "err.bad_path");
    char b[40];
    snprintf(b, sizeof b, "{\"files\":%u}", (unsigned)n);
    return web_json(req, 200, b);
}

/* ---- POST /api/upload ---- */
static void upload_done(void) {
    web_upload_release();
    refresh_led_reset();      /* the refresh task repaints the LED within a tick */
}

/* The body is not (fully) read on an early error: answer, then drop the connection so the leftover bytes are not
 * parsed as the next request. */
static esp_err_t upload_reject(httpd_req_t *req, int st, const char *key) {
    web_error(req, st, key);
    return ESP_FAIL;
}

/* Upload scratch. It used to be one 7264-byte frame, and name_reply() put another 1888 on top: ~7.5 KiB live in the
 * 8 KiB httpd task, so the response was written from a smashed frame and every POST /api/upload died with no answer
 * (http=000) after the file was already stored (the SPA then retried and the card got "name (2)"). Static is safe
 * because esp_http_server runs every handler on its one task (see the file header) and because the 4096-byte receive
 * chunk is the bulk of it: on the heap it would be a permanent ~10 KiB reservation on a no-PSRAM part, and malloc'd
 * per request the way webdav's dav_t is it would fragment the heap for the largest block this firmware asks for. */
static struct {
    char q[QUERY_MAX];
    char dir[WC_PATH_MAX + 1];
    char name[NAME_BUF];
    char safe[NAME_BUF];
    char full[WC_PATH_MAX + 1];
    char final_name[NAME_BUF];
    char tmp[24];
    char buf[4096];
} s_up;

static esp_err_t upload_post(httpd_req_t *req) {
    if (!query_str(req, s_up.q, sizeof s_up.q)) return upload_reject(req, 400, "err.bad_path");
    {
        char raw[WC_PATH_MAX + 4];
        api_get_t g = api_query_get(s_up.q, "path", raw, sizeof raw);
        if (g == API_INVALID) return upload_reject(req, 400, "err.bad_path");
        if (g == API_ABSENT) raw[0] = 0;
        if (!norm(raw, s_up.dir)) return upload_reject(req, 400, "err.bad_path");
    }
    if (api_query_get(s_up.q, "name", s_up.name, sizeof s_up.name) != API_OK
        || !wc_sd_safe_name(s_up.name, s_up.safe, sizeof s_up.safe)
        || !join_norm(s_up.dir, s_up.safe, s_up.full))
        return upload_reject(req, 400, "err.bad_name");
    /* Content-Length is required (an explicit 0 is an empty file); chunked bodies are not supported. */
    if (httpd_req_get_hdr_value_len(req, "Content-Length") == 0 || httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0)
        return upload_reject(req, 411, "err.length");

    if (httpd_req_get_hdr_value_str(req, "X-Client-Time", s_up.tmp, sizeof s_up.tmp) == ESP_OK) {
        char *end;
        long long ms = strtoll(s_up.tmp, &end, 10);
        if (end != s_up.tmp && *end == 0 && ms > 0) {
            int tz = s_set->tz_min;
            char tzs[12];
            if (httpd_req_get_hdr_value_str(req, "X-Client-Tz", tzs, sizeof tzs) == ESP_OK) {
                long v = strtol(tzs, &end, 10);
                if (end != tzs && *end == 0 && v >= -840 && v <= 840) tz = (int)v;
            }
            timekeep_client_hint(ms, tz);
        }
    }

    if (!web_upload_claim()) return upload_reject(req, 409, "err.busy");   /* one upload handle (~16.5 KiB) at a time */
    fs_upload_t *u = NULL;
    int rc = fs_upload_begin(s_up.dir, s_up.safe, req->content_len, &u);
    if (rc != FS_OK) {
        web_upload_release();
        if (rc == FS_ENOSPC) return upload_reject(req, 507, "err.no_space");
        if (rc == FS_ENOENT) return upload_reject(req, 404, "err.not_found");
        if (rc == FS_EINVAL) return upload_reject(req, 400, "err.bad_name");
        return upload_reject(req, 500, "err.io");
    }
    led_set(128, 0, 128);
    size_t left = req->content_len;
    while (left) {
        int r = httpd_req_recv(req, s_up.buf, left < sizeof s_up.buf ? left : sizeof s_up.buf);
        if (r <= 0) {                       /* error or timeout: the client is gone */
            ESP_LOGW(TAG, "upload recv failed (%d), %u bytes left", r, (unsigned)left);
            fs_upload_abort(u);
            upload_done();
            if (r == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
            return ESP_FAIL;
        }
        rc = fs_upload_write(u, s_up.buf, (size_t)r);
        if (rc != FS_OK) {
            fs_upload_abort(u);
            upload_done();
            return upload_reject(req, rc == FS_ENOSPC ? 507 : 500, rc == FS_ENOSPC ? "err.no_space" : "err.io");
        }
        left -= (size_t)r;
    }
    rc = fs_upload_commit(u, 0, s_up.final_name, sizeof s_up.final_name);
    upload_done();
    if (rc != FS_OK) return fs_fail(req, rc, "err.bad_name");
    return name_reply(req, s_up.final_name, false);
}

void api_files_register(settings_t *s) {
    s_set = s;
    web_route("/api/list", HTTP_GET, WEB_AUTH, list_get);
    web_route("/api/count", HTTP_GET, WEB_AUTH, count_get);
    web_route("/api/mkdir", HTTP_POST, WEB_AUTH, mkdir_post);
    web_route("/api/rename", HTTP_POST, WEB_AUTH, rename_post);
    web_route("/api/move", HTTP_POST, WEB_AUTH, move_post);
    web_route("/api/delete", HTTP_POST, WEB_AUTH, delete_post);
    web_route("/api/upload", HTTP_POST, WEB_AUTH, upload_post);
}
