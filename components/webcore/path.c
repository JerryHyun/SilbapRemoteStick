#include "webcore.h"
#include <stdio.h>
#include <string.h>

static bool starts_silbap(const char *s, size_t len) {
    static const char pre[] = ".silbap";
    if (len < 7) return false;
    for (int i = 0; i < 7; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != pre[i]) return false;
    }
    return true;
}

bool wc_norm_path(const char *rel, char *out, size_t out_sz) {
    if (!rel || !out || out_sz == 0) return false;
    if (*rel == '/') rel++;
    size_t n = 0;
    out[0] = 0;
    while (*rel) {
        const char *end = strchr(rel, '/');
        size_t len = end ? (size_t)(end - rel) : strlen(rel);
        if (len == 0) {
            /* a single trailing slash is fine; anything else is an empty segment */
            if (end && end[1] == 0) break;
            return false;
        }
        if ((len == 1 && rel[0] == '.') || (len == 2 && rel[0] == '.' && rel[1] == '.')) return false;
        if (starts_silbap(rel, len)) return false;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)rel[i];
            if (c < 0x20 || c == 0x7F) return false;
        }
        size_t need = len + (n ? 1 : 0);
        if (n + need > WC_PATH_MAX - 1 || n + need + 1 > out_sz) return false;
        if (n) out[n++] = '/';
        memcpy(out + n, rel, len);
        n += len;
        out[n] = 0;
        if (!end) break;
        rel = end + 1;
    }
    return true;
}

static const char *fullwidth(char c) {
    switch (c) {
    case '\\': return "\xEF\xBC\xBC";
    case '/':  return "\xEF\xBC\x8F";
    case ':':  return "\xEF\xBC\x9A";
    case '*':  return "\xEF\xBC\x8A";
    case '?':  return "\xEF\xBC\x9F";
    case '"':  return "\xEF\xBC\x82";
    case '<':  return "\xEF\xBC\x9C";
    case '>':  return "\xEF\xBC\x9E";
    case '|':  return "\xEF\xBD\x9C";
    default:   return NULL;
    }
}

bool wc_sd_safe_name(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return false;
    size_t n = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        const char *fw = fullwidth((char)c);
        const char *rep = fw;
        char one[2] = {0, 0};
        if (!rep) {
            one[0] = (c < 0x20 || c == 0x7F) ? '_' : (char)c;
            rep = one;
        }
        size_t l = strlen(rep);
        if (n + l > 255 || n + l + 1 > out_sz) return false;
        memcpy(out + n, rep, l);
        n += l;
    }
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return n > 0 && !starts_silbap(out, n);
}

void wc_conflict_name(const char *name, unsigned n, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    size_t len = strlen(name);
    const char *dot = strrchr(name, '.');
    size_t stem = (dot && dot != name) ? (size_t)(dot - name) : len;
    int r = snprintf(out, out_sz, "%.*s (%u)%s", (int)stem, name, n, name + stem);
    if (r < 0) out[0] = 0;
}
