#include "webcore.h"
#include <string.h>

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool wc_host_ok(const char *host, const char *const *allowed) {
    if (!host || !allowed) return false;
    size_t len = strlen(host);
    const char *colon = strchr(host, ':');
    if (colon) {
        if (strcmp(colon, ":80") != 0) return false;
        len = (size_t)(colon - host);
    }
    if (len > 0 && host[len - 1] == '.') len--;   /* FQDN form "silbapstick.local." */
    if (len == 0) return false;
    for (; *allowed; allowed++) {
        const char *a = *allowed;
        if (strlen(a) != len) continue;
        size_t i = 0;
        while (i < len && lc(host[i]) == lc(a[i])) i++;
        if (i == len) return true;
    }
    return false;
}

bool wc_origin_ok(const char *origin, const char *const *allowed) {
    if (!origin) return true;
    if (strncmp(origin, "http://", 7) != 0) return false;
    return wc_host_ok(origin + 7, allowed);
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool wc_basic_password(const char *hdr, char *pass, size_t pass_sz) {
    if (!hdr || !pass || pass_sz == 0) return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    const char *p = hdr + 6;
    size_t len = strlen(p);
    if (len == 0 || len % 4 != 0 || len > 400) return false;
    uint8_t dec[304];
    size_t n = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = p[i + k];
            if (c == '=') {
                /* padding only in the last quad, last 1-2 positions */
                if (i + 4 != len || k < 2) return false;
                pad++;
                v[k] = 0;
            } else {
                if (pad) return false;
                v[k] = b64val(c);
                if (v[k] < 0) return false;
            }
        }
        uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        if (n + 3 > sizeof dec) return false;
        dec[n++] = (uint8_t)(w >> 16);
        if (pad < 2) dec[n++] = (uint8_t)(w >> 8);
        if (pad < 1) dec[n++] = (uint8_t)w;
    }
    const uint8_t *colon = memchr(dec, ':', n);
    if (!colon) return false;
    size_t pl = n - (size_t)(colon + 1 - dec);
    if (pl + 1 > pass_sz) return false;
    if (memchr(colon + 1, 0, pl)) return false;
    memcpy(pass, colon + 1, pl);
    pass[pl] = 0;
    return true;
}

void wc_pw_hash(const char *pw, const uint8_t salt[16], uint8_t out[32]) {
    uint8_t buf[16 + 128];
    size_t l = strlen(pw);
    if (l > 128) l = 128;
    memcpy(buf, salt, 16);
    memcpy(buf + 16, pw, l);
    wc_sha256(buf, 16 + l, out);
}

bool wc_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
