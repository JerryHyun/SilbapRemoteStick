#pragma once
/* Private to the namemap component. */
#include "namemap.h"
/* Kana run state (small tsu, small ya/yu/yo). Lives on the caller's stack for one nm_romanize call, so
 * nm_romanize is reentrant: no static state anywhere in namemap. */
typedef struct {
    int pending_sokuon;         /* small tsu doubles the next consonant */
    const char *prev_kana;      /* romaji of the last emitted kana syllable, NULL otherwise */
} nm_kana_t;
static inline void nm_kana_reset(nm_kana_t *k) { k->pending_sokuon = 0; k->prev_kana = NULL; }
/* Appends the romanisation of one non-ASCII, non-Hangul code point; false if the script is unknown. */
bool nm_emit_script(nm_kana_t *st, uint32_t cp, nm_lang_t lang, char *buf, size_t *len, size_t cap);
