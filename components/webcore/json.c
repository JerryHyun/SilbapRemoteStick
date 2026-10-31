#include "webcore.h"
#include <stdio.h>
#include <string.h>

static void put(wc_json_t *w, const char *s, size_t n) {
    if (w->overflow) return;
    if (w->len + n + 1 > w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}
static void puts_(wc_json_t *w, const char *s) { put(w, s, strlen(s)); }

static void put_escaped(wc_json_t *w, const char *s) {
    put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': puts_(w, "\\\""); break;
        case '\\': puts_(w, "\\\\"); break;
        case '\n': puts_(w, "\\n"); break;
        case '\r': puts_(w, "\\r"); break;
        case '\t': puts_(w, "\\t"); break;
        default:
            if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); puts_(w, e); }
            else put(w, (const char *)&c, 1);
        }
    }
    put(w, "\"", 1);
}

static void prefix(wc_json_t *w, const char *key) {
    if (w->need_comma) put(w, ",", 1);
    if (key) { put_escaped(w, key); put(w, ":", 1); }
}

void wc_json_init(wc_json_t *w, char *buf, size_t cap) {
    w->buf = buf; w->cap = cap; w->len = 0; w->overflow = false; w->need_comma = false;
    if (cap) buf[0] = 0; else w->overflow = true;
}
void wc_json_obj_open(wc_json_t *w) { prefix(w, NULL); put(w, "{", 1); w->need_comma = false; }
void wc_json_obj_close(wc_json_t *w) { put(w, "}", 1); w->need_comma = true; }
void wc_json_arr_open(wc_json_t *w, const char *key) { prefix(w, key); put(w, "[", 1); w->need_comma = false; }
void wc_json_arr_close(wc_json_t *w) { put(w, "]", 1); w->need_comma = true; }
void wc_json_str(wc_json_t *w, const char *key, const char *val) { prefix(w, key); put_escaped(w, val ? val : ""); w->need_comma = true; }
void wc_json_num(wc_json_t *w, const char *key, int64_t val) {
    char t[24]; snprintf(t, sizeof t, "%lld", (long long)val);
    prefix(w, key); puts_(w, t); w->need_comma = true;
}
void wc_json_bool(wc_json_t *w, const char *key, bool val) { prefix(w, key); puts_(w, val ? "true" : "false"); w->need_comma = true; }
