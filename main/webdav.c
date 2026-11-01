/*
 * webdav.c - WebDAV at /dav over the SD card
 *
 * Adapted from MainasuK/RemoteUSB main/webdav.c
 * Copyright (c) 2026 MainasuK. MIT License: licenses/RemoteUSB-MIT.txt
 *
 * Changes for SilbapRemoteStick: served below /dav instead of the root; RemoteUSB's Basic auth, firmware upload,
 * status page and HTML browser listing are removed (the web middleware does Host/Origin/auth, the SPA is
 * the browser UI); every path goes through dav_rel (wc_norm_path + SD-safe names, so ".silbap*" is unreachable and
 * never listed); every mutation goes through fsops (close-before-mutate; PUT and COPY stream into a temp file and
 * are renamed into place); PROPFIND answers Depth 0/1 only (infinity -> 403) and streams in batches; GET honours a
 * single Range; COPY copies files only; PROPPATCH answers 207 with every property as a no-op success; PUT answers
 * "Expect: 100-continue" itself (esp_http_server does not) and a MOVE replaces its destination without ever deleting
 * it before the source is in place.
 *
 * esp_http_server runs every handler on its one task. The per-request scratch is malloc'd (no PSRAM: nothing big
 * stays allocated while WebDAV is idle), and the SD lock is held for a bounded readdir+stat batch, one read of at
 * most DAV_BUF bytes or one fsops step at a time, never across a network send or receive.
 */
#include "webdav.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "board.h"
#include "esp_log.h"
#include "esp_random.h"
#include "fsops.h"
#include "led.h"
#include "refresh.h"
#include "sdcard.h"
#include "web.h"
#include "webcore.h"
#include "webdav_xml.h"

static const char *TAG = "webdav";

#define DAV_BUF      8192                          /* response batch / file read chunk (< FS_CHUNK under the lock) */
#define RAW_CAP      (WC_PATH_MAX * 3 + 8)         /* a decoded path, or an encoded card path */
#define DEST_CAP     2400                          /* Destination header: http://host/dav/ + 2048-byte URI */
#define ABS_CAP      (sizeof SD_MOUNT + WC_PATH_MAX + 2 + 256)
#define CHILD_CAP    (WC_PATH_MAX + 2 + 256)
#define RECV_RETRIES 5
#define DAV_SCAN     16                            /* directory entries scanned per lock hold (PROPFIND depth 1) */
#define LOCK_SECONDS "3600"
#define DAV_BUSY     (-100)                        /* copy_file: another upload is in flight */

typedef struct {
    char raw[RAW_CAP];
    char rel[WC_PATH_MAX + 1], dst[WC_PATH_MAX + 1];
    char abs[ABS_CAP];
    char child[CHILD_CAP];
    char name[256];
    char bak[WC_PATH_MAX + 1];                         /* MOVE: the parked old destination */
    char hdr[DEST_CAP];
    char buf[DAV_BUF];
} dav_t;

/* ---- small helpers ---- */
static bool send_all(httpd_req_t *req, const char *b, size_t n) {
    int tries = 0;
    while (n) {
        int r = httpd_send(req, b, n);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++tries <= RECV_RETRIES) continue;
        if (r <= 0) return false;
        tries = 0;
        b += r;
        n -= (size_t)r;
    }
    return true;
}
static esp_err_t send_status(httpd_req_t *req, const char *st) {
    /* 204 has no body and no Content-Length (RFC 9110 6.4.1), and every httpd_resp_send* writes one or the other:
     * a body left in the stream is read as the start of the next response on a kept-alive connection. */
    if (!strncmp(st, "204", 3)) {
        static const char r204[] = "HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\n\r\n";
        return send_all(req, r204, sizeof r204 - 1) ? ESP_OK : ESP_FAIL;
    }
    httpd_resp_set_status(req, st);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* HEAD: the headers of the GET, no body (a body would be read as the next response on a kept-alive socket). */
    if (req->method == HTTP_HEAD) return httpd_resp_send(req, NULL, (ssize_t)strlen(st));
    return httpd_resp_sendstr(req, st);
}
/* The request body was not read: answer, then drop the connection so it is not parsed as the next request. */
static esp_err_t reject(httpd_req_t *req, const char *st) {
    send_status(req, st);
    return ESP_FAIL;
}
/* An early error before the body was read: the same rule as api_files.c's upload_reject, applied exactly when bytes
 * are still queued (a body left unread must not be parsed as the next request; with no body the connection is kept,
 * and httpd_req_delete() would purge it anyway). */
static esp_err_t fail_req(httpd_req_t *req, const char *st) {
    return req->content_len ? reject(req, st) : send_status(req, st);
}

static void abs_of(const char *rel, char *out) {
    snprintf(out, ABS_CAP, "%s%s%s", SD_MOUNT, rel[0] ? "/" : "", rel);
}
static int stat_rel(dav_t *w, const char *rel, struct stat *st) {
    abs_of(rel, w->abs);
    sdcard_lock();
    int r = stat(w->abs, st);
    sdcard_unlock();
    return r;
}
/* rel "a/b/c" -> parent "a/b" in par, last segment returned (points into rel). */
static const char *split(const char *rel, char *par) {
    const char *s = strrchr(rel, '/');
    if (!s) { par[0] = 0; return rel; }
    memcpy(par, rel, (size_t)(s - rel));
    par[s - rel] = 0;
    return s + 1;
}
static const char *fs_status(int rc) {
    switch (rc) {
    case FS_EEXIST:     return "412 Precondition Failed";
    case FS_ENOENT:     return "409 Conflict";
    case FS_ENOSPC:     return "507 Insufficient Storage";
    case FS_EINVAL:     return "400 Bad Request";
    case FS_EINTO_SELF: return "403 Forbidden";
    default:            return "500 Internal Server Error";
    }
}
static void upload_led_on(void) { led_set(128, 0, 128); }
static void upload_end(void) { web_upload_release(); refresh_led_reset(); }

/* ---- OPTIONS ---- */
static esp_err_t dav_options(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_hdr(req, "Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- PROPFIND ---- */
typedef struct { size_t len; bool ok; } batch_t;
static void flush_batch(httpd_req_t *req, dav_t *w, batch_t *b) {
    if (b->ok && b->len) b->ok = httpd_resp_send_chunk(req, w->buf, (ssize_t)b->len) == ESP_OK;
    b->len = 0;
}
static void add_entry(httpd_req_t *req, dav_t *w, batch_t *b, const char *rel, const char *name, const struct stat *st) {
    bool dir = S_ISDIR(st->st_mode);
    uint64_t size = dir ? 0 : (uint64_t)st->st_size;
    for (int pass = 0; pass < 2 && b->ok; pass++) {
        int n = dav_propfind_entry(w->buf + b->len, sizeof w->buf - b->len, rel, name, dir, size, (int64_t)st->st_mtime);
        if (n >= 0) { b->len += (size_t)n; return; }
        if (b->len == 0) break;                          /* does not fit even alone: skipped */
        flush_batch(req, w, b);
    }
    if (b->ok) ESP_LOGW(TAG, "propfind entry too long, skipped");
}

static esp_err_t dav_propfind(httpd_req_t *req, dav_t *w) {
    int depth = 1;                                       /* Ruling: no Depth header is treated as 1 (RFC: SHOULD be infinity) */
    char d[16];
    esp_err_t dh = httpd_req_get_hdr_value_str(req, "Depth", d, sizeof d);
    if (dh == ESP_ERR_HTTPD_RESULT_TRUNC) return fail_req(req, "400 Bad Request");   /* not 0, 1 or infinity */
    if (dh == ESP_OK) {
        if (!strcmp(d, "0")) depth = 0;
        else if (!strcmp(d, "1")) depth = 1;
        else if (!strcasecmp(d, "infinity")) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "application/xml; charset=utf-8");
            return httpd_resp_sendstr(req, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                           "<D:error xmlns:D=\"DAV:\"><D:propfind-finite-depth/></D:error>");
        } else return fail_req(req, "400 Bad Request");
    }
    struct stat st;
    if (stat_rel(w, w->rel, &st)) return fail_req(req, "404 Not Found");

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    batch_t b = { 0, true };
    static const char head[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
    memcpy(w->buf, head, sizeof head - 1);
    b.len = sizeof head - 1;
    const char *self = strrchr(w->rel, '/');
    add_entry(req, w, &b, w->rel, self ? self + 1 : w->rel, &st);

    if (S_ISDIR(st.st_mode) && depth == 1) {
        sdcard_lock();
        DIR *dir = opendir(w->abs);
        sdcard_unlock();
        size_t base = strlen(w->abs);
        char safe[256];
        while (dir && b.ok) {
            bool got = false, eof = false;
            struct stat cs;
            int seen = 0;
            sdcard_lock();                               /* at most DAV_SCAN entries per lock hold */
            struct dirent *e;
            while (seen < DAV_SCAN && (e = readdir(dir)) != NULL) {
                seen++;
                const char *n = e->d_name;
                if (!strcmp(n, ".") || !strcmp(n, "..") || !strncasecmp(n, ".silbap", 7)) continue;
                size_t nl = strlen(n);
                if (nl >= sizeof w->name || base + 1 + nl >= sizeof w->abs) continue;
                /* The name must round-trip through dav_rel(), which applies wc_sd_safe_name again: a card name that
                 * rule would rewrite ("a:b.jef", "rose.jef. ") can never be reached by the href we would list, so it
                 * is skipped rather than advertised as a 404. */
                if (!wc_sd_safe_name(n, safe, sizeof safe) || strcmp(safe, n)) continue;
                w->abs[base] = '/';
                memcpy(w->abs + base + 1, n, nl + 1);
                int r = stat(w->abs, &cs);
                w->abs[base] = 0;
                if (r) continue;
                memcpy(w->name, n, nl + 1);
                got = true;
                break;
            }
            if (!got && seen < DAV_SCAN) eof = true;     /* readdir hit the end of the directory */
            sdcard_unlock();
            if (!got) { if (eof) break; else continue; }
            int cl = snprintf(w->child, sizeof w->child, "%s%s%s", w->rel, w->rel[0] ? "/" : "", w->name);
            /* Normalise (and so re-check ".silbap*") into w->raw, and list that: the name from the card is what the
             * href must carry, not the unsanitised concatenation. */
            if (cl < 0 || (size_t)cl >= sizeof w->child || !wc_norm_path(w->child, w->raw, WC_PATH_MAX + 1)) continue;
            add_entry(req, w, &b, w->raw, w->name, &cs);
        }
        if (dir) { sdcard_lock(); closedir(dir); sdcard_unlock(); }
    }
    static const char tail[] = "</D:multistatus>";
    if (b.ok && b.len + sizeof tail > sizeof w->buf) flush_batch(req, w, &b);
    if (b.ok) { memcpy(w->buf + b.len, tail, sizeof tail - 1); b.len += sizeof tail - 1; }
    flush_batch(req, w, &b);
    if (!b.ok) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- GET / HEAD ---- */
static esp_err_t dav_get(httpd_req_t *req, dav_t *w) {
    struct stat st;
    if (stat_rel(w, w->rel, &st)) return fail_req(req, "404 Not Found");
    if (S_ISDIR(st.st_mode)) {
        static const char page[] = "<!doctype html><meta charset=\"utf-8\"><title>WebDAV</title>"
                                   "<p>This is the WebDAV folder: connect to it from a file manager. "
                                   "<a href=\"/\">Open the web file manager</a></p>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        /* HEAD answers with the GET's headers and no body. */
        if (req->method == HTTP_HEAD) return httpd_resp_send(req, NULL, sizeof page - 1);
        return httpd_resp_sendstr(req, page);
    }
    uint64_t size = (uint64_t)st.st_size, first = 0, last = size ? size - 1 : 0;
    char range[64];
    int rk = 0;
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof range) == ESP_OK)
        rk = dav_parse_range(range, size, &first, &last);
    if (rk < 0) {
        char cr[40];
        snprintf(cr, sizeof cr, "bytes */%llu", (unsigned long long)size);
        httpd_resp_set_hdr(req, "Content-Range", cr);
        return fail_req(req, "416 Range Not Satisfiable");
    }
    uint64_t len = size ? last - first + 1 : 0;
    int fd = -1;
    if (req->method == HTTP_GET && len) {
        sdcard_lock();
        fd = open(w->abs, O_RDONLY);
        if (fd >= 0 && first && lseek(fd, (off_t)first, SEEK_SET) != (off_t)first) { close(fd); fd = -1; }
        sdcard_unlock();
        if (fd < 0) return fail_req(req, "500 Internal Server Error");
        /* The open FIL stays valid until close: a coherency re-mount drops FatFs's whole cached state (and does
         * not flush it), so a re-mount in the middle of this stream would invalidate the FIL and truncate the body
         * after Content-Length was sent. Bracket the whole stream; the refresh task skips its re-mount while the
         * counter is non-zero and retries next tick. This is the item that keeps a resume/retry honest too. */
        refresh_activity();
        fs_inflight_enter();
    }
    /* Raw head: httpd_resp_send_chunk would add chunked encoding, and HEAD must carry the real Content-Length. */
    char lm[40], cr[80] = "";
    dav_http_date((int64_t)st.st_mtime, lm, sizeof lm);
    if (rk > 0) snprintf(cr, sizeof cr, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)first,
                         (unsigned long long)last, (unsigned long long)size);
    const char *nm = strrchr(w->rel, '/');
    int hl = snprintf(w->buf, sizeof w->buf,
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nAccept-Ranges: bytes\r\n"
                      "Last-Modified: %s\r\nETag: \"%llx-%llx\"\r\nCache-Control: no-cache\r\n%s\r\n",
                      rk > 0 ? "206 Partial Content" : "200 OK", dav_content_type(nm ? nm + 1 : w->rel),
                      (unsigned long long)len, lm, (unsigned long long)st.st_mtime, (unsigned long long)size, cr);
    bool ok = hl > 0 && (size_t)hl < sizeof w->buf && send_all(req, w->buf, (size_t)hl);
    while (ok && fd >= 0 && len) {
        size_t n = len < sizeof w->buf ? (size_t)len : sizeof w->buf;
        sdcard_lock();
        ssize_t r = read(fd, w->buf, n);
        sdcard_unlock();
        if (r <= 0) { ok = false; break; }               /* card gone or file shrank: the promised length is broken */
        ok = send_all(req, w->buf, (size_t)r);
        len -= (uint64_t)r;
    }
    if (fd >= 0) { sdcard_lock(); close(fd); sdcard_unlock(); fs_inflight_leave(); }
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---- PUT ---- */
static esp_err_t dav_put(httpd_req_t *req, dav_t *w) {
    if (!w->rel[0]) return reject(req, "405 Method Not Allowed");
    /* esp_http_server has no Expect handling of its own: answer 100-continue before the first recv, or the Windows
     * WebClient stalls until it times out. Any other expectation is refused (RFC 9110 10.1.1) with the body unread,
     * so the connection is dropped. */
    char ex[48];
    esp_err_t eh = httpd_req_get_hdr_value_str(req, "Expect", ex, sizeof ex);
    dav_expect_t want = DAV_EXPECT_NONE;
    if (eh == ESP_OK) want = dav_expect(ex);
    else if (eh == ESP_ERR_HTTPD_RESULT_TRUNC) want = DAV_EXPECT_UNSUPPORTED;   /* longer than any token we know */
    if (want == DAV_EXPECT_UNSUPPORTED) return reject(req, "417 Expectation Failed");
    /* Content-Length is required (0 is an empty file); esp_http_server cannot read chunked request bodies. */
    if (httpd_req_get_hdr_value_len(req, "Content-Length") == 0 || httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0)
        return reject(req, "411 Length Required");
    struct stat st;
    if (stat_rel(w, w->rel, &st) == 0 && S_ISDIR(st.st_mode)) return reject(req, "405 Method Not Allowed");
    const char *name = split(w->rel, w->dst);
    if (!web_upload_claim()) {
        httpd_resp_set_hdr(req, "Retry-After", "2");
        return reject(req, "503 Service Unavailable");
    }
    fs_upload_t *u = NULL;
    int rc = fs_upload_begin(w->dst, name, req->content_len, &u);
    if (rc != FS_OK) { web_upload_release(); return reject(req, fs_status(rc)); }
    /* Everything that can be refused without the body has been checked: now the client may send it. */
    if (want == DAV_EXPECT_CONTINUE) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!send_all(req, cont, sizeof cont - 1)) { fs_upload_abort(u); upload_end(); return ESP_FAIL; }
    }
    upload_led_on();
    size_t left = req->content_len;
    int tries = 0;
    while (left) {
        int r = httpd_req_recv(req, w->buf, left < sizeof w->buf ? left : sizeof w->buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++tries <= RECV_RETRIES) continue;
        if (r <= 0) {
            ESP_LOGW(TAG, "PUT recv failed (%d), %u bytes left", r, (unsigned)left);
            fs_upload_abort(u);
            upload_end();
            return ESP_FAIL;
        }
        tries = 0;
        rc = fs_upload_write(u, w->buf, (size_t)r);    /* writes a 16 KiB chunk under the lock when one is full */
        if (rc != FS_OK) { fs_upload_abort(u); upload_end(); return reject(req, fs_status(rc)); }
        left -= (size_t)r;
    }
    bool replaced = false;
    rc = fs_upload_commit_replace(u, 0, &replaced);
    upload_end();
    if (rc != FS_OK) return send_status(req, rc == FS_EEXIST ? "405 Method Not Allowed" : fs_status(rc));
    return send_status(req, replaced ? "204 No Content" : "201 Created");
}

/* ---- DELETE / MKCOL ---- */
static esp_err_t dav_delete(httpd_req_t *req, dav_t *w) {
    if (!w->rel[0]) return send_status(req, "403 Forbidden");
    int rc = fs_delete(w->rel, NULL);                    /* no trash: deleted directly */
    if (rc == FS_ENOENT) return send_status(req, "404 Not Found");
    return send_status(req, rc == FS_OK ? "204 No Content" : "500 Internal Server Error");
}

static esp_err_t dav_mkcol(httpd_req_t *req, dav_t *w) {
    if (req->content_len > 0) return reject(req, "415 Unsupported Media Type");
    if (!w->rel[0]) return send_status(req, "405 Method Not Allowed");
    int rc = fs_mkdir(w->rel);
    if (rc == FS_OK) return send_status(req, "201 Created");
    if (rc == FS_EEXIST) return send_status(req, "405 Method Not Allowed");
    return send_status(req, fs_status(rc));
}

/* ---- MOVE / COPY ---- */
/* Moves src to dst (both card paths, dst free) with fs_move and fs_rename, undoing the first step when the second
 * fails. */
static int move_to(dav_t *w, const char *src, const char *dst) {
    char sp[WC_PATH_MAX + 1], dp[WC_PATH_MAX + 1];
    const char *sn = split(src, sp), *dn = split(dst, dp);
    if (!strcasecmp(sp, dp)) return fs_rename(src, dn);  /* same folder (FAT folds case) */
    if (!strcmp(sn, dn)) return fs_move(src, dp);
    int rc = fs_move(src, dp);
    if (rc == FS_OK) {                                   /* now dp/sn: rename it */
        snprintf(w->child, sizeof w->child, "%s%s%s", dp, dp[0] ? "/" : "", sn);
        rc = fs_rename(w->child, dn);
        if (rc != FS_OK && fs_move(w->child, sp) != FS_OK) ESP_LOGW(TAG, "MOVE: could not undo the move");
    } else if (rc == FS_EEXIST) {                        /* sn is taken in dp: rename first, then move */
        rc = fs_rename(src, dn);
        if (rc != FS_OK) return rc;
        snprintf(w->child, sizeof w->child, "%s%s%s", sp, sp[0] ? "/" : "", dn);
        rc = fs_move(w->child, dp);
        if (rc != FS_OK && fs_rename(w->child, sn) != FS_OK) ESP_LOGW(TAG, "MOVE: could not undo the rename");
    }
    return rc;
}

/* A free-looking name beside the destination: the old destination is parked there while the source moves in. The
 * "~dav-" prefix is SD-safe and makes a leftover from a power cut recognisable. dst is a card path; false when the
 * result does not fit. */
static bool park_name(const char *dst, char *out, size_t sz) {
    const char *slash = strrchr(dst, '/');
    int n = snprintf(out, sz, "%.*s~dav-%08lx", slash ? (int)(slash - dst + 1) : 0, dst,
                     (unsigned long)esp_random());
    return n > 0 && (size_t)n < sz;
}

/* MOVE over an existing dst: the old destination is renamed aside first, the source then moves into the free name,
 * and only once it is in place is the parked copy deleted. A failure before the endpoint puts the destination back
 * (RFC 4918 9.8.5), so nothing is lost. */
static int move_replace(dav_t *w, const char *src, const char *dst) {
    const char *slash = strrchr(dst, '/'), *dn = slash ? slash + 1 : dst;
    bool parked = false;
    for (int i = 0; i < 8 && !parked; i++) {
        if (!park_name(dst, w->bak, sizeof w->bak)) return FS_EINVAL;
        const char *bs = strrchr(w->bak, '/');
        int rc = fs_rename(dst, bs ? bs + 1 : w->bak);
        if (rc == FS_OK) parked = true;
        else if (rc != FS_EEXIST) return rc;             /* a fresh random name should never be taken */
    }
    if (!parked) return FS_EEXIST;
    int rc = move_to(w, src, dst);
    if (rc != FS_OK) {
        if (fs_rename(w->bak, dn) != FS_OK) ESP_LOGW(TAG, "MOVE: could not put the destination back");
        return rc;
    }
    if (fs_delete(w->bak, NULL) != FS_OK) ESP_LOGW(TAG, "MOVE: old destination left at %s", w->bak);
    return FS_OK;
}

/* Copies the file src into dir/name through an upload (temp file, then renamed into place). */
static int copy_file(dav_t *w, const char *src, uint64_t size, const char *dir, const char *name) {
    if (!web_upload_claim()) return DAV_BUSY;
    fs_upload_t *u = NULL;
    int rc = fs_upload_begin(dir, name, size, &u);
    if (rc != FS_OK) { web_upload_release(); return rc; }
    upload_led_on();
    abs_of(src, w->abs);
    sdcard_lock();
    int fd = open(w->abs, O_RDONLY);
    sdcard_unlock();
    if (fd < 0) rc = FS_ENOENT;
    while (rc == FS_OK) {
        sdcard_lock();
        ssize_t r = read(fd, w->buf, sizeof w->buf);
        sdcard_unlock();
        if (r < 0) rc = FS_EIO;
        if (r <= 0) break;
        rc = fs_upload_write(u, w->buf, (size_t)r);
    }
    if (fd >= 0) { sdcard_lock(); close(fd); sdcard_unlock(); }
    if (rc == FS_OK) rc = fs_upload_commit_replace(u, 0, NULL);
    else fs_upload_abort(u);
    upload_end();
    return rc;
}

static esp_err_t dav_copy_move(httpd_req_t *req, dav_t *w, bool move) {
    if (httpd_req_get_hdr_value_str(req, "Destination", w->hdr, sizeof w->hdr) != ESP_OK)
        return fail_req(req, "400 Bad Request");
    switch (dav_uri_path_ex(w->hdr, w->raw, sizeof w->raw)) {
    case DAV_URI_OK: break;
    case DAV_URI_BAD: return fail_req(req, "400 Bad Request");    /* bad escape, NUL, too long: the client's fault */
    default:          return fail_req(req, "502 Bad Gateway");    /* well-formed, but outside /dav */
    }
    switch (dav_rel_status(w->raw, w->dst, sizeof w->dst)) {
    case DAV_PATH_OK: break;
    case DAV_PATH_ABSENT: return fail_req(req, "404 Not Found");   /* hidden: as if it did not exist */
    default:              return fail_req(req, "400 Bad Request");
    }
    if (!w->rel[0] || !w->dst[0]) return fail_req(req, "403 Forbidden");
    char ow[4] = "T";
    httpd_req_get_hdr_value_str(req, "Overwrite", ow, sizeof ow);
    bool overwrite = !(ow[0] == 'F' || ow[0] == 'f');
    struct stat ss, ds;
    if (stat_rel(w, w->rel, &ss)) return fail_req(req, "404 Not Found");
    if (!move && S_ISDIR(ss.st_mode)) return fail_req(req, "501 Not Implemented");   /* COPY: files only */
    if (!strcasecmp(w->rel, w->dst)) {                   /* the same entry on FAT */
        if (!move || !strcmp(w->rel, w->dst)) return fail_req(req, "403 Forbidden");
        const char *dn = split(w->dst, w->child);
        int rc = fs_rename(w->rel, dn);                  /* case-only rename */
        return rc == FS_OK ? send_status(req, "201 Created") : fail_req(req, fs_status(rc));
    }
    size_t sl = strlen(w->rel);
    if (!strncasecmp(w->dst, w->rel, sl) && w->dst[sl] == '/') return fail_req(req, "403 Forbidden");   /* into itself */
    size_t dl = strlen(w->dst);
    /* The destination holds the source: replacing it would delete the source too (and everything else in it). */
    if (!strncasecmp(w->rel, w->dst, dl) && w->rel[dl] == '/') return fail_req(req, "409 Conflict");
    char dp[WC_PATH_MAX + 1];
    const char *dn = split(w->dst, dp);
    if (stat_rel(w, dp, &ds) || !S_ISDIR(ds.st_mode)) return fail_req(req, "409 Conflict");
    bool existed = stat_rel(w, w->dst, &ds) == 0;
    if (existed) {
        if (!overwrite) return fail_req(req, "412 Precondition Failed");
        /* COPY is files only: never delete a collection to put a file in its place (RFC 4918 would allow it, but
         * wiping a folder because of a COPY is not worth it). MOVE may replace a collection, and does so without
         * deleting it first: move_replace parks it and removes it only after the source is in place. */
        if (!move && S_ISDIR(ds.st_mode)) return fail_req(req, "409 Conflict");
    }
    int rc;
    if (!move) rc = copy_file(w, w->rel, (uint64_t)ss.st_size, dp, dn);
    else if (existed) rc = move_replace(w, w->rel, w->dst);
    else rc = move_to(w, w->rel, w->dst);
    if (rc == DAV_BUSY) {
        httpd_resp_set_hdr(req, "Retry-After", "2");
        return fail_req(req, "503 Service Unavailable");
    }
    if (rc != FS_OK) return fail_req(req, fs_status(rc));
    return send_status(req, existed ? "204 No Content" : "201 Created");
}

/* ---- LOCK / UNLOCK / PROPPATCH ---- */
static esp_err_t dav_lock(httpd_req_t *req, dav_t *w) {
    /* Fake locks so the macOS Finder and the Windows Explorer open files for writing; nothing is locked. */
    char token[48], lt[52];
    snprintf(token, sizeof token, "opaquelocktoken:%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(lt, sizeof lt, "<%s>", token);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "Lock-Token", lt);
    httpd_resp_set_hdr(req, "Timeout", "Second-" LOCK_SECONDS);
    int n = snprintf(w->buf, sizeof w->buf,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
                     "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope>"
                     "<D:depth>infinity</D:depth><D:owner/><D:timeout>Second-" LOCK_SECONDS "</D:timeout>"
                     "<D:locktoken><D:href>%s</D:href></D:locktoken>"
                     "</D:activelock></D:lockdiscovery></D:prop>", token);
    return httpd_resp_send(req, w->buf, n);
}

static esp_err_t dav_proppatch(httpd_req_t *req, dav_t *w) {
    struct stat st;
    if (stat_rel(w, w->rel, &st)) return send_status(req, "404 Not Found");
    if (req->content_len >= sizeof w->buf) return reject(req, "413 Payload Too Large");   /* nothing is read */
    int pn = 0;
    if (req->content_len > 0) {
        esp_err_t rc;
        if (!web_read_body(req, w->buf, sizeof w->buf, "err.bad_request", &rc)) return rc;
        pn = dav_proppatch_props(w->buf, w->hdr, sizeof w->hdr);
        if (pn < 0) pn = 0;
    }
    dav_href_encode(w->rel, w->raw, sizeof w->raw);
    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    bool ok = httpd_resp_send_chunk(req, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">"
                                         "<D:response><D:href>" DAV_ROOT "/", HTTPD_RESP_USE_STRLEN) == ESP_OK
              && httpd_resp_send_chunk(req, w->raw, HTTPD_RESP_USE_STRLEN) == ESP_OK
              && httpd_resp_send_chunk(req, S_ISDIR(st.st_mode) && w->rel[0] ? "/</D:href><D:propstat><D:prop>"
                                                                             : "</D:href><D:propstat><D:prop>",
                                       HTTPD_RESP_USE_STRLEN) == ESP_OK
              && (pn == 0 || httpd_resp_send_chunk(req, w->hdr, pn) == ESP_OK)
              && httpd_resp_send_chunk(req, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                                            "</D:multistatus>", HTTPD_RESP_USE_STRLEN) == ESP_OK;
    if (!ok) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- dispatcher ---- */
static esp_err_t dav_handler(httpd_req_t *req) {
    dav_t *w = malloc(sizeof *w);
    if (!w) { httpd_resp_set_hdr(req, "Retry-After", "2"); return reject(req, "503 Service Unavailable"); }
    esp_err_t e;
    switch (dav_uri_path_ex(req->uri, w->raw, sizeof w->raw)) {
    case DAV_URI_OK: break;
    case DAV_URI_BAD:                                        /* not a URI we can read at all */
        e = fail_req(req, "400 Bad Request");
        free(w);
        return e;
    default:                                                 /* outside /dav: as if it did not exist */
        e = fail_req(req, "404 Not Found");
        free(w);
        return e;
    }
    switch (dav_rel_status(w->raw, w->rel, sizeof w->rel)) {
    case DAV_PATH_OK: break;
    case DAV_PATH_ABSENT:                                    /* ".silbap*", "..": as if it did not exist */
        e = fail_req(req, "404 Not Found");
        free(w);
        return e;
    default:                                                 /* over-long or otherwise unusable name */
        e = fail_req(req, "400 Bad Request");
        free(w);
        return e;
    }
    switch (req->method) {
    case HTTP_OPTIONS:   e = dav_options(req); break;
    case HTTP_PROPFIND:  e = dav_propfind(req, w); break;
    case HTTP_GET:
    case HTTP_HEAD:      e = dav_get(req, w); break;
    case HTTP_PUT:       e = dav_put(req, w); break;
    case HTTP_DELETE:    e = dav_delete(req, w); break;
    case HTTP_MKCOL:     e = dav_mkcol(req, w); break;
    case HTTP_MOVE:      e = dav_copy_move(req, w, true); break;
    case HTTP_COPY:      e = dav_copy_move(req, w, false); break;
    case HTTP_LOCK:      e = dav_lock(req, w); break;
    case HTTP_UNLOCK:    e = send_status(req, "204 No Content"); break;
    case HTTP_PROPPATCH: e = dav_proppatch(req, w); break;
    default:
        httpd_resp_set_hdr(req, "Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        e = fail_req(req, "405 Method Not Allowed");
    }
    free(w);
    return e;
}

static esp_err_t root_options(httpd_req_t *req) { return dav_options(req); }

void webdav_register(void) {
    web_route(DAV_ROOT "*", HTTP_ANY, WEB_AUTH, dav_handler);   /* "/dav", "/dav/..." ("/davx" is answered 404) */
    web_route("/", HTTP_OPTIONS, WEB_PUBLIC, root_options);    /* the Windows WebClient probes the server root first */
}
