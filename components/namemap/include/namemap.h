#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { NM_LANG_EN = 0, NM_LANG_KO, NM_LANG_ZH, NM_LANG_JA } nm_lang_t;
#define NM_LFN_MAX 24            /* machine LFN incl. extension */
#define NM_EXT_MAX 8
/* Version of the name output (nm_romanize, nm_with_suffix, nm_sfn and the generated tables). Bump it whenever
 * any of them can produce a different name for the same input: catalog.bin records it and a mismatch rebuilds.
 * 2: reentrant rewrite + "_" squeeze before the suffix tag, Greek dialytika capitals, half-width katakana. */
#define NM_TABLES_VERSION 2
/* Every function here is reentrant (no static state): plan 2's web server may call them next to the catalog task. */
/* One path component (UTF-8) -> machine LFN, uppercase ASCII, NUL-terminated,
 * at most NM_LFN_MAX chars. out must hold NM_LFN_MAX+1. Returns strlen(out). */
size_t nm_romanize(const char *name, nm_lang_t lang, char *out);
/* lfn + "_" + tag4(tag) before the extension, re-truncated to NM_LFN_MAX. */
size_t nm_with_suffix(const char *lfn, uint32_t tag, char *out);
uint32_t nm_fnv1a32(const void *p, size_t n);
void nm_tag4(uint32_t h, char out[5]);                  /* base32 A-Z2-7, 4 chars */
/* UTF-8 decode one code point; returns bytes used (>=1); invalid byte -> U+FFFD */
int nm_utf8_next(const char *s, uint32_t *cp);
/* Append the RR romanisation of one Hangul syllable (U+AC00..U+D7A3). */
void nm_hangul_rr(uint32_t syllable, char *buf, size_t *len, size_t cap);
/* 8.3 name for a machine LFN; round 0 = exact fit when possible, round k>=1 -> "XY" + tag4 + "~k" */
bool nm_sfn(const char *lfn_ascii, uint32_t round, uint8_t sfn[11]);   /* returns true if lfn fits 8.3 exactly (no LFN entries needed) */
uint8_t nm_lfn_checksum(const uint8_t sfn[11]);
