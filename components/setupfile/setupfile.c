#include "setupfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decode to UTF-8. *buf is malloc'd (or NULL when the input is used in place via *p). */
static int decode(const uint8_t *d, size_t n, const uint8_t **p, size_t *pn, uint8_t **buf) {
    *buf = NULL;
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
        bool le = d[0] == 0xFF;
        uint8_t *o = malloc(n * 2 + 4);
        if (!o) return -1;
        size_t k = 0;
        for (size_t i = 2; i + 1 < n; i += 2) {
            uint32_t c = le ? (uint32_t)(d[i] | d[i + 1] << 8) : (uint32_t)(d[i] << 8 | d[i + 1]);
            if (c >= 0xD800 && c < 0xDC00) {
                uint32_t lo = 0;
                if (i + 3 < n) lo = le ? (uint32_t)(d[i + 2] | d[i + 3] << 8) : (uint32_t)(d[i + 2] << 8 | d[i + 3]);
                if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
                else c = 0xFFFD;
            } else if (c >= 0xDC00 && c < 0xE000) c = 0xFFFD;
            if (c < 0x80) o[k++] = (uint8_t)c;
            else if (c < 0x800) { o[k++] = (uint8_t)(0xC0 | c >> 6); o[k++] = (uint8_t)(0x80 | (c & 63)); }
            else if (c < 0x10000) { o[k++] = (uint8_t)(0xE0 | c >> 12); o[k++] = (uint8_t)(0x80 | ((c >> 6) & 63)); o[k++] = (uint8_t)(0x80 | (c & 63)); }
            else { o[k++] = (uint8_t)(0xF0 | c >> 18); o[k++] = (uint8_t)(0x80 | ((c >> 12) & 63)); o[k++] = (uint8_t)(0x80 | ((c >> 6) & 63)); o[k++] = (uint8_t)(0x80 | (c & 63)); }
        }
        *buf = o; *p = o; *pn = k;
        return 0;
    }
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) { d += 3; n -= 3; }
    *p = d; *pn = n;
    return 0;
}

typedef struct { char *o; size_t sz, n; bool ovf; } wr_t;
static void wr(wr_t *w, const char *s, size_t len) {
    if (!w->o || w->ovf) return;
    if (w->n + len + 1 > w->sz) { w->ovf = true; return; }
    memcpy(w->o + w->n, s, len); w->n += len; w->o[w->n] = 0;
}
static void wrs(wr_t *w, const char *s) { wr(w, s, strlen(s)); }

static void copy_trunc(char *dst, size_t dsz, const char *s, size_t n) {
    if (n >= dsz) n = dsz - 1;
    memcpy(dst, s, n); dst[n] = 0;
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool ieq(const char *s, size_t n, const char *lit) {
    if (strlen(lit) != n) return false;
    for (size_t i = 0; i < n; i++) if (lc(s[i]) != lit[i]) return false;
    return true;
}

enum { K_UNKNOWN, K_SSID, K_WPW, K_WEBPW, K_RENAME, K_LANG };

static void note(unsigned *cnt, char *first, const char *k, size_t kn) {
    if (!(*cnt)++) copy_trunc(first, 32, k, kn);
}

/* Shared walk: fills cfg; if w != NULL also writes the masked body and warnings. */
static int walk(const uint8_t *data, size_t len, sf_config_t *c, wr_t *w, const char *iso) {
    const uint8_t *p; size_t n; uint8_t *buf;
    if (decode(data, len, &p, &n, &buf)) return -1;
    memset(c, 0, sizeof *c);
    const char *s = (const char *)p;
    char warn[4096]; size_t wn = 0; /* collected warnings */
    warn[0] = 0;
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        size_t le = e;
        if (le > i && s[le - 1] == '\r') le--;
        const char *line = s + i; size_t ll = le - i;
        i = e + 1;
        size_t a = 0;
        while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
        if (a == ll) { if (w) wr(w, "\n", 1); continue; }              /* blank */
        if (line[a] == '#') { if (w) { wr(w, line, ll); wr(w, "\n", 1); } continue; }
        const char *eq = memchr(line + a, '=', ll - a);
        if (!eq) continue;                                            /* not key=value */
        const char *ks = line + a, *ke = eq;
        while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        size_t kn = (size_t)(ke - ks);
        const char *v = eq + 1; size_t vn = (size_t)(line + ll - v);
        int key = K_UNKNOWN;
        if (ieq(ks, kn, "wifi_ssid")) key = K_SSID;
        else if (ieq(ks, kn, "wifi_password")) key = K_WPW;
        else if (ieq(ks, kn, "web_password")) key = K_WEBPW;
        else if (ieq(ks, kn, "rename")) key = K_RENAME;
        else if (ieq(ks, kn, "language")) key = K_LANG;
        bool bad = false;
        if (key == K_SSID) {
            if (vn >= 1 && vn <= 32) { copy_trunc(c->ssid, sizeof c->ssid, v, vn); c->has_ssid = true; } else bad = true;
        } else if (key == K_WPW) {
            if (vn == 0 || (vn >= 8 && vn <= 64)) { copy_trunc(c->wifi_pw, sizeof c->wifi_pw, v, vn); c->has_wifi_pw = true; } else bad = true;
        } else if (key == K_WEBPW) {
            if (vn <= 64) { copy_trunc(c->web_pw, sizeof c->web_pw, v, vn); c->has_web_pw = true; } else bad = true;
        } else if (key == K_RENAME || key == K_LANG) {
            while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
            while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
            if (key == K_RENAME) {
                if (ieq(v, vn, "on") || ieq(v, vn, "true") || ieq(v, vn, "1")) { c->rename = true; c->has_rename = true; }
                else if (ieq(v, vn, "off") || ieq(v, vn, "false") || ieq(v, vn, "0")) { c->rename = false; c->has_rename = true; }
                else bad = true;
            } else {
                if (ieq(v, vn, "ko") || ieq(v, vn, "en") || ieq(v, vn, "zh") || ieq(v, vn, "ja")) {
                    c->lang[0] = lc(v[0]); c->lang[1] = lc(v[1]); c->lang[2] = 0; c->has_lang = true;
                } else bad = true;
            }
        }
        char kb[40]; copy_trunc(kb, sizeof kb, ks, kn);
        if (key == K_UNKNOWN) {
            note(&c->unknown_count, c->unknown_first, ks, kn);
            if (w && wn + 80 < sizeof warn) wn += (size_t)snprintf(warn + wn, sizeof warn - wn, "# warning: unknown key '%s' ignored\n", kb);
        } else if (bad) {
            note(&c->bad_count, c->bad_first, ks, kn);
            if (w && wn + 80 < sizeof warn) wn += (size_t)snprintf(warn + wn, sizeof warn - wn, "# warning: invalid value for '%s' ignored\n", kb);
        }
        if (w) {
            wr(w, ks, kn); wr(w, "=", 1);
            if ((key == K_WPW || key == K_WEBPW || key == K_UNKNOWN) && vn) wrs(w, "****"); else wr(w, v, vn);
            wr(w, "\n", 1);
        }
    }
    if (w) {
        wrs(w, "# applied "); wrs(w, iso ? iso : ""); wrs(w, "\n");
        wrs(w, warn);
    }
    free(buf);
    return 0;
}

int sf_parse(const uint8_t *data, size_t len, sf_config_t *out) {
    return walk(data, len, out, NULL, NULL);
}

int sf_masked(const uint8_t *data, size_t len, const char *iso_time, char *out, size_t out_sz) {
    if (!out || !out_sz) return -1;
    out[0] = 0;
    wr_t w = { out, out_sz, 0, false };
    sf_config_t c;
    if (walk(data, len, &c, &w, iso_time) || w.ovf) return -1;
    return (int)w.n;
}
