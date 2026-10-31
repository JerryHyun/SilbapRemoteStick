#include "namemap.h"
#include <string.h>
uint8_t nm_lfn_checksum(const uint8_t sfn[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + sfn[i]);
    return s;
}
bool nm_sfn(const char *lfn, uint32_t round, uint8_t sfn[11]) {
    memset(sfn, ' ', 11);
    const char *dot = strrchr(lfn, '.');
    size_t bl = dot ? (size_t)(dot - lfn) : strlen(lfn), el = dot ? strlen(dot + 1) : 0;
    bool ascii = true;
    for (const char *p = lfn; *p; p++) if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) ascii = false;
    if (round == 0 && ascii && bl >= 1 && bl <= 8 && el <= 3 && (!dot || (el >= 1 && strchr(lfn, '.') == dot))) {
        memcpy(sfn, lfn, bl); if (el) memcpy(sfn + 8, dot + 1, el);
        return true;
    }
    /* every 9 rounds the next tag; the step is spread into the top bits nm_tag4 reads */
    char t[5]; nm_tag4(nm_fnv1a32(lfn, strlen(lfn)) + ((round ? round - 1 : 0) / 9) * 0x9E3779B9u, t);
    size_t k = 0;
    for (const char *p = lfn; *p && k < 2 && p != dot; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) sfn[k++] = (uint8_t)*p;
    while (k < 2) sfn[k++] = '_';
    memcpy(sfn + 2, t, 4); sfn[6] = '~'; sfn[7] = (uint8_t)('1' + (round ? (round - 1) % 9 : 0));
    for (size_t i = 0, j = 8; dot && i < el && j < 11; i++) {
        char c = dot[1 + i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) sfn[j++] = (uint8_t)c;
    }
    return false;
}
