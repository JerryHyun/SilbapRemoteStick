/* Pure WebDAV helpers. See webdav_xml.h. Written for SilbapRemoteStick; the PROPFIND entry layout, the date format
 * and the content-type idea come from MainasuK/RemoteUSB main/webdav.c (MIT, Copyright (c) 2026 MainasuK).
 * Everything writes through a cursor straight into the caller's buffer: no large locals (the HTTP task stack is
 * 8 KiB). */
#include "webdav_xml.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "api_list_core.h"   /* api_url_decode */
#include "webcore.h"

typedef struct { char *b; size_t cap, len; bool ovf; } cur_t;

static void cur_init(cur_t *c, char *b, size_t cap) {
    c->b = b; c->cap = cap; c->len = 0; c->ovf = cap == 0;
    if (cap) b[0] = 0;
}
static void put_n(cur_t *c, const char *s, size_t n) {
    if (c->ovf) return;
    if (c->len + n + 1 > c->cap) { c->ovf = true; return; }
    memcpy(c->b + c->len, s, n);
    c->len += n;
    c->b[c->len] = 0;
}
static void put(cur_t *c, const char *s) { put_n(c, s, strlen(s)); }
static void putf(cur_t *c, const char *fmt, ...) {
    if (c->ovf) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->b + c->len, c->cap - c->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= c->cap - c->len) { c->ovf = true; c->b[c->len] = 0; return; }
    c->len += (size_t)n;
}
static bool unreserved(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.'
           || ch == '_' || ch == '~';
}
static void put_enc(cur_t *c, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s && !c->ovf; s++) {
        unsigned char ch = (unsigned char)*s;
        if (unreserved(ch) || ch == '/') put_n(c, s, 1);
        else { char e[3] = { '%', hex[ch >> 4], hex[ch & 15] }; put_n(c, e, 3); }
    }
}
static void put_esc_n(cur_t *c, const char *s, size_t n) {
    for (size_t i = 0; i < n && !c->ovf; i++) {
        switch (s[i]) {
        case '&':  put(c, "&amp;"); break;
        case '<':  put(c, "&lt;"); break;
        case '>':  put(c, "&gt;"); break;
        case '"':  put(c, "&quot;"); break;
        case '\'': put(c, "&apos;"); break;
        default:   put_n(c, s + i, 1);
        }
    }
}
static void put_esc(cur_t *c, const char *s) { put_esc_n(c, s, strlen(s)); }
/* Ends a whole-result writer: on overflow the output is "" and false is returned. */
static bool cur_done(cur_t *c) {
    if (c->ovf && c->cap) c->b[0] = 0;
    return !c->ovf;
}

bool dav_href_encode(const char *path, char *out, size_t sz) {
    cur_t c; cur_init(&c, out, sz);
    put_enc(&c, path ? path : "");
    return cur_done(&c);
}

bool xml_escape(const char *in, char *out, size_t sz) {
    cur_t c; cur_init(&c, out, sz);
    put_esc(&c, in ? in : "");
    return cur_done(&c);
}

static void gm(int64_t t, struct tm *tm) {
    time_t tt = (time_t)(t < 0 ? 0 : t);
    gmtime_r(&tt, tm);
}
void dav_http_date(int64_t t, char *out, size_t sz) {
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct tm tm;
    gm(t, &tm);
    snprintf(out, sz, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[tm.tm_wday % 7], tm.tm_mday, mo[tm.tm_mon % 12],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
void dav_iso_date(int64_t t, char *out, size_t sz) {
    struct tm tm;
    gm(t, &tm);
    snprintf(out, sz, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
}

const char *dav_content_type(const char *name) {
    static const struct { const char *ext, *type; } map[] = {
        { "txt", "text/plain; charset=utf-8" }, { "csv", "text/csv; charset=utf-8" },
        { "htm", "text/html; charset=utf-8" },  { "html", "text/html; charset=utf-8" },
        { "json", "application/json" },        { "xml", "text/xml; charset=utf-8" },
        { "svg", "image/svg+xml" },            { "png", "image/png" },
        { "jpg", "image/jpeg" },               { "jpeg", "image/jpeg" },
        { "gif", "image/gif" },                { "webp", "image/webp" },
        { "bmp", "image/bmp" },                { "pdf", "application/pdf" },
        { "zip", "application/zip" },
    };
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot)
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcasecmp(dot + 1, map[i].ext)) return map[i].type;
    return "application/octet-stream";   /* embroidery formats included */
}

int dav_propfind_entry(char *out, size_t sz, const char *rel, const char *name, bool dir, uint64_t size, int64_t mtime) {
    char lm[40], cd[32];
    dav_http_date(mtime, lm, sizeof lm);
    dav_iso_date(mtime, cd, sizeof cd);
    cur_t c; cur_init(&c, out, sz);
    put(&c, "<D:response><D:href>" DAV_ROOT "/");
    put_enc(&c, rel);
    if (dir && rel[0]) put(&c, "/");
    put(&c, "</D:href><D:propstat><D:prop><D:displayname>");
    put_esc(&c, name);
    put(&c, "</D:displayname>");
    if (dir) put(&c, "<D:resourcetype><D:collection/></D:resourcetype>");
    else putf(&c, "<D:resourcetype/><D:getcontentlength>%llu</D:getcontentlength><D:getcontenttype>%s</D:getcontenttype>",
              (unsigned long long)size, dav_content_type(name));
    putf(&c, "<D:creationdate>%s</D:creationdate><D:getlastmodified>%s</D:getlastmodified>"
             "<D:getetag>\"%llx-%llx\"</D:getetag>", cd, lm, (unsigned long long)mtime, (unsigned long long)size);
    put(&c, "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype>"
            "</D:lockentry></D:supportedlock>"
            "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
    return cur_done(&c) ? (int)c.len : -1;
}

/* Decimal digits at *p into *v; false without digits or on overflow. */
static bool num(const char **p, uint64_t *v) {
    const char *s = *p;
    uint64_t x = 0;
    if (*s < '0' || *s > '9') return false;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (x > (UINT64_MAX - 9) / 10) return false;
        x = x * 10 + (uint64_t)(*s - '0');
    }
    *p = s;
    *v = x;
    return true;
}
static const char *skip_sp(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

int dav_parse_range(const char *hdr, uint64_t size, uint64_t *first, uint64_t *last) {
    if (!hdr || strncasecmp(hdr, "bytes=", 6) || strchr(hdr, ',')) return 0;
    const char *p = skip_sp(hdr + 6);
    uint64_t a, b = 0;
    if (*p == '-') {                                     /* suffix: the last b bytes */
        p++;
        if (!num(&p, &b) || *skip_sp(p)) return 0;
        if (b == 0 || size == 0) return -1;
        *first = b >= size ? 0 : size - b;
        *last = size - 1;
        return 1;
    }
    if (!num(&p, &a) || *p++ != '-') return 0;
    bool open = true;
    if (*p >= '0' && *p <= '9') { if (!num(&p, &b)) return 0; open = false; }
    if (*skip_sp(p)) return 0;
    if (!open && b < a) return 0;
    if (a >= size) return -1;
    *first = a;
    *last = open || b >= size ? size - 1 : b;
    return 1;
}

bool dav_uri_path(const char *uri, char *out, size_t sz) {
    return dav_uri_path_ex(uri, out, sz) == DAV_URI_OK;
}

dav_uri_t dav_uri_path_ex(const char *uri, char *out, size_t sz) {
    if (!uri || !out || !sz) return DAV_URI_BAD;
    out[0] = 0;
    const char *p = uri;
    if (*p != '/') {                                     /* absolute URL: skip scheme://authority */
        const char *s = strstr(p, "://");
        if (!s) return DAV_URI_BAD;
        p = strchr(s + 3, '/');
        if (!p) return DAV_URI_NOT_DAV;                  /* "http://host": not a DAV path */
    }
    size_t n = strcspn(p, "?#");
    size_t rl = sizeof DAV_ROOT - 1;
    if (n < rl || memcmp(p, DAV_ROOT, rl) || (n > rl && p[rl] != '/')) return DAV_URI_NOT_DAV;
    return api_url_decode(p + rl, n - rl, out, sz) ? DAV_URI_OK : DAV_URI_BAD;
}

/* ".silbap*" (case-insensitive), as in wc_norm_path. */
static bool seg_silbap(const char *s, size_t len) {
    static const char pre[] = ".silbap";
    if (len < 7) return false;
    for (size_t i = 0; i < 7; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != pre[i]) return false;
    }
    return true;
}

dav_path_t dav_rel_status(const char *raw, char *out, size_t sz) {
    char a[WC_PATH_MAX + 1], seg[256];
    if (!raw || !out || !sz) return DAV_PATH_BAD;
    size_t n = 0;
    out[0] = 0;
    const char *p = *raw == '/' ? raw + 1 : raw;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 0) {
            if (e && e[1] == 0) break;                   /* one trailing slash is fine */
            return DAV_PATH_BAD;                         /* "//": malformed, not hidden */
        }
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return DAV_PATH_ABSENT;
        if (seg_silbap(p, len)) return DAV_PATH_ABSENT;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c < 0x20 || c == 0x7F) return DAV_PATH_BAD;
        }
        if (len >= sizeof a) return DAV_PATH_BAD;
        memcpy(a, p, len);
        a[len] = 0;
        if (!wc_sd_safe_name(a, seg, sizeof seg)) return DAV_PATH_BAD;   /* > 255 bytes, or trims to nothing */
        size_t l = strlen(seg);
        size_t need = n + (n ? 1 : 0) + l;
        if (need > WC_PATH_MAX - 1 || need + 1 > sz) return DAV_PATH_BAD;
        if (n) out[n++] = '/';
        memcpy(out + n, seg, l + 1);
        n += l;
        if (!e) break;
        p = e + 1;
    }
    /* The result goes through wc_norm_path once more: it is the authority on the final path, and it re-checks every
     * ".silbap*" segment after the name cleanup changed the bytes. Anything it still refuses is unreachable, so it
     * is answered 404, never 400. Normalising into a and copying back keeps the buffers small. */
    if (!wc_norm_path(out, a, sizeof a)) return DAV_PATH_ABSENT;
    size_t m = strlen(a);
    if (m + 1 > sz) return DAV_PATH_BAD;
    memmove(out, a, m + 1);
    return DAV_PATH_OK;
}

bool dav_rel(const char *raw, char *out, size_t sz) {
    return dav_rel_status(raw, out, sz) == DAV_PATH_OK;
}

dav_expect_t dav_expect(const char *hdr) {
    if (!hdr) return DAV_EXPECT_NONE;
    bool any = false, bad = false;
    const char *p = hdr;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ',' && *p != ';') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if ((size_t)(e - s) == 12 && !strncasecmp(s, "100-continue", 12)) any = true;
        else bad = true;                                 /* an empty list element is not an expectation */
        while (*p && *p != ',') p++;                     /* drop any parameters */
    }
    if (bad) return DAV_EXPECT_UNSUPPORTED;
    return any ? DAV_EXPECT_CONTINUE : DAV_EXPECT_NONE;
}

/* ---- PROPPATCH echo ---- */
typedef struct { const char *pre; size_t pl; const char *uri; size_t ul; } ns_t;
#define NS_MAX 12

static bool name_ch(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.'
           || ch == '_' || ch >= 0x80;
}
/* The URI is copied from XML source text, so an entity the client wrote ("&amp;") is already escaped and must stay
 * as it is; a bare '&', '<', '>' or '"' (our parser is lenient) is escaped so the 207 body stays well-formed. */
static bool entity_at(const char *s, size_t left) {
    static const char *const named[] = { "amp;", "lt;", "gt;", "quot;", "apos;" };
    if (left < 4 || s[0] != '&') return false;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        size_t l = strlen(named[i]);
        if (left >= l + 1 && !memcmp(s + 1, named[i], l)) return true;
    }
    if (s[1] != '#') return false;
    size_t i = 2;
    if (i < left && (s[i] == 'x' || s[i] == 'X')) {
        i++;
        size_t d = i;
        for (; i < left; i++) {
            char h = s[i];
            if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) break;
        }
        return i > d && i < left && s[i] == ';';
    }
    size_t d = i;
    while (i < left && s[i] >= '0' && s[i] <= '9') i++;
    return i > d && i < left && s[i] == ';';
}
static void put_uri(cur_t *c, const char *s, size_t n) {
    for (size_t i = 0; i < n && !c->ovf;) {
        if (s[i] == '&' && entity_at(s + i, n - i)) {
            size_t j = i + 1;
            while (j < n && s[j] != ';') j++;
            put_n(c, s + i, j - i + 1);
            i = j + 1;
            continue;
        }
        put_esc_n(c, s + i, 1);
        i++;
    }
}

static bool ws(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
static const char *skip_ws(const char *p) { while (ws(*p)) p++; return p; }

int dav_proppatch_props(const char *body, char *out, size_t sz) {
    cur_t c; cur_init(&c, out, sz);
    ns_t ns[NS_MAX];
    int nns = 0, depth = 0, prop_depth = -1;
    const char *p = body ? body : "";
    while ((p = strchr(p, '<')) != NULL) {
        if (!strncmp(p, "<?", 2)) { p = strstr(p, "?>"); if (!p) break; p += 2; continue; }
        if (!strncmp(p, "<!--", 4)) { p = strstr(p, "-->"); if (!p) break; p += 3; continue; }
        if (!strncmp(p, "<![CDATA[", 9)) { p = strstr(p, "]]>"); if (!p) break; p += 3; continue; }
        if (p[1] == '!') { p = strchr(p, '>'); if (!p) break; p++; continue; }
        if (p[1] == '/') {
            p = strchr(p, '>');
            if (!p) break;
            p++;
            if (depth > 0) depth--;
            if (depth == prop_depth) prop_depth = -1;
            continue;
        }
        /* start tag: qname, attributes, then '>' or "/>"; q = NULL when it is truncated or malformed */
        const char *q = p + 1, *qn = q;
        while (*q && (name_ch((unsigned char)*q) || *q == ':')) q++;
        size_t ql = (size_t)(q - qn);
        const char *colon = memchr(qn, ':', ql);
        const char *loc = colon ? colon + 1 : qn;
        size_t ll = (size_t)(qn + ql - loc), pl = colon ? (size_t)(colon - qn) : 0;
        bool self = false, ok = ql > 0 && ll > 0;
        for (;;) {
            q = skip_ws(q);
            if (*q == '>') { q++; break; }
            if (q[0] == '/' && q[1] == '>') { self = true; q += 2; break; }
            const char *an = q;
            while (*q && *q != '=' && *q != '>' && *q != '/' && !ws(*q)) q++;
            size_t al = (size_t)(q - an);
            q = skip_ws(q);
            if (*q != '=' || al == 0) { q = NULL; break; }
            q = skip_ws(q + 1);
            char quote = *q;
            if (quote != '"' && quote != '\'') { q = NULL; break; }
            const char *v = ++q;
            q = strchr(q, quote);
            if (!q) break;
            size_t vl = (size_t)(q - v);
            q++;
            if (nns < NS_MAX && al >= 5 && !strncmp(an, "xmlns", 5) && (al == 5 || an[5] == ':'))
                ns[nns++] = (ns_t){ an + (al > 5 ? 6 : 5), al > 5 ? al - 6 : 0, v, vl };
        }
        if (!q) break;
        p = q;
        if (ok && prop_depth >= 0 && depth == prop_depth + 1) {
            const ns_t *f = NULL;
            for (int i = nns - 1; i >= 0 && !f; i--)
                if (ns[i].pl == pl && !strncmp(ns[i].pre, qn, pl)) f = &ns[i];
            if (f && f->ul) {
                put(&c, "<z:"); put_n(&c, loc, ll); put(&c, " xmlns:z=\"");
                put_uri(&c, f->uri, f->ul);
                put(&c, "\"/>");
            } else {
                put(&c, "<"); put_n(&c, loc, ll); put(&c, " xmlns=\"\"/>");
            }
        }
        if (ok && prop_depth < 0 && ll == 4 && !strncmp(loc, "prop", 4) && !self) prop_depth = depth;
        if (!self) depth++;
    }
    return cur_done(&c) ? (int)c.len : -1;
}
