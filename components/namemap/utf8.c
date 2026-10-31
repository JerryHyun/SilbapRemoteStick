#include "namemap.h"
int nm_utf8_next(const char *s, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int n = (u[0] >= 0xF0 && u[0] < 0xF8) ? 4 : (u[0] >= 0xE0 && u[0] < 0xF0) ? 3 : (u[0] >= 0xC2 && u[0] < 0xE0) ? 2 : 0;
    if (!n) { *cp = 0xFFFD; return 1; }
    uint32_t c = u[0] & (0x3F >> (n - 1));
    for (int i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        c = c << 6 | (u[i] & 0x3F);
    }
    *cp = c;
    return n;
}
uint32_t nm_fnv1a32(const void *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) { h ^= ((const unsigned char *)p)[i]; h *= 0x01000193u; }
    return h;
}
void nm_tag4(uint32_t h, char out[5]) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    for (int i = 0; i < 4; i++) out[i] = a[(h >> (27 - 5 * i)) & 31];
    out[4] = 0;
}
