#include "namemap_internal.h"
#include <string.h>
#define RAW_CAP 512
/* Reentrant: all state is on the stack (raw: RAW_CAP bytes) or in the caller's buffers. */
/* Decodes the next code point at *p, composing conjoining jamo L V [T] into a syllable (macOS NFD names). */
static uint32_t next_cp(const char **p) {
    uint32_t c, v, t;
    *p += nm_utf8_next(*p, &c);
    if (c >= 0x1100 && c <= 0x1112 && **p) {
        int k = nm_utf8_next(*p, &v);
        if (v >= 0x1161 && v <= 0x1175) {
            *p += k;
            c = 0xAC00 + ((c - 0x1100) * 21 + (v - 0x1161)) * 28;
            if (**p) { k = nm_utf8_next(*p, &t); if (t >= 0x11A8 && t <= 0x11C2) { *p += k; c += t - 0x11A7; } }
        }
    }
    return c;
}
static void emit_ascii(uint32_t c, char *buf, size_t *len) {
    if (*len + 1 >= RAW_CAP) return;
    if (c >= 'a' && c <= 'z') c -= 32;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.') buf[(*len)++] = (char)c;
    else buf[(*len)++] = '_';           /* space, '_' and every other symbol */
    buf[*len] = 0;
}
static void squeeze(char *s) {          /* collapse "_" runs, trim "_-" at both ends */
    char *w = s;
    for (char *r = s; *r; r++) { if (*r == '_' && w > s && w[-1] == '_') continue; *w++ = *r; }
    *w = 0;
    size_t a = strspn(s, "_-"); memmove(s, s + a, strlen(s + a) + 1);
    size_t n = strlen(s); while (n && (s[n - 1] == '_' || s[n - 1] == '-')) s[--n] = 0;
}
static const char *const RESERVED[] = {"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"};
static void assemble(char *base, char *ext, char *out) {
    size_t el = strlen(ext);
    size_t room = NM_LFN_MAX - (el ? el + 1 : 0);
    if (strlen(base) > room) { base[room] = 0; squeeze(base); }
    if (!*base) strcpy(base, "DESIGN");
    for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++)
        if (!strcmp(base, RESERVED[i])) { if (strlen(base) < room) strcat(base, "_"); break; }
    strcpy(out, base);
    if (el) { strcat(out, "."); strcat(out, ext); }
}
size_t nm_romanize(const char *name, nm_lang_t lang, char *out) {
    nm_kana_t st; nm_kana_reset(&st);
    char raw[RAW_CAP] = "";
    size_t len = 0;
    for (const char *p = name; *p;) {
        uint32_t c = next_cp(&p);
        if (c < 0x80) { nm_kana_reset(&st); emit_ascii(c, raw, &len); }
        else if (c >= 0xAC00 && c <= 0xD7A3) { nm_kana_reset(&st); nm_hangul_rr(c, raw, &len, RAW_CAP); }
        else if (!nm_emit_script(&st, c, lang, raw, &len, RAW_CAP)) { /* unknown script: dropped */ }
    }
    char *dot = strrchr(raw, '.');
    char ext[NM_EXT_MAX + 1] = "";
    if (dot) {
        *dot = 0;
        strncat(ext, dot + 1, NM_EXT_MAX);
        for (char *p = ext; *p; p++) if (*p == '.') *p = '_';
        squeeze(ext);
    }
    for (char *p = raw; *p; p++) if (*p == '.') *p = '_';
    squeeze(raw);
    assemble(raw, ext, out);
    return strlen(out);
}
size_t nm_with_suffix(const char *lfn, uint32_t tag, char *out) {
    char base[NM_LFN_MAX + 6], ext[NM_EXT_MAX + 1] = "", t[5];
    /* lfn is an nm_romanize result (<= NM_LFN_MAX, extension <= NM_EXT_MAX); both copies are bounded anyway */
    const char *dot = strrchr(lfn, '.');
    size_t bl = dot ? (size_t)(dot - lfn) : strlen(lfn);
    if (bl > NM_LFN_MAX) bl = NM_LFN_MAX;
    memcpy(base, lfn, bl); base[bl] = 0;
    if (dot) { strncat(ext, dot + 1, NM_EXT_MAX); }
    nm_tag4(tag, t);
    size_t el = strlen(ext), room = NM_LFN_MAX - 5 - (el ? el + 1 : 0);
    if (strlen(base) > room) base[room] = 0;
    size_t n = strlen(base);                       /* no "__" or "-_" before the tag */
    while (n && (base[n - 1] == '_' || base[n - 1] == '-')) base[--n] = 0;
    strcat(base, "_"); strcat(base, t);
    strcpy(out, base);
    if (el) { strcat(out, "."); strcat(out, ext); }
    return strlen(out);
}
