#include "namemap.h"
#include <string.h>
static const char *const L[19] = {"G","KK","N","D","TT","R","M","B","PP","S","SS","","J","JJ","CH","K","T","P","H"};
static const char *const V[21] = {"A","AE","YA","YAE","EO","E","YEO","YE","O","WA","WAE","OE","YO","U","WO","WE","WI","YU","EU","UI","I"};
static const char *const T[28] = {"","K","K","K","N","N","N","T","L","K","M","L","L","L","P","L","M","P","P","T","T","NG","T","T","K","T","P","T"};
static void put(char *buf, size_t *len, size_t cap, const char *s) {
    while (*s && *len + 1 < cap) buf[(*len)++] = *s++;
    buf[*len] = 0;
}
void nm_hangul_rr(uint32_t c, char *buf, size_t *len, size_t cap) {
    uint32_t i = c - 0xAC00;
    put(buf, len, cap, L[i / 588]);
    put(buf, len, cap, V[(i % 588) / 28]);
    put(buf, len, cap, T[i % 28]);
}
