#include "namemap_internal.h"
#include <string.h>
extern const uint16_t nm_han_mandarin[], nm_han_hangul[], nm_han_japanese[];
extern const char nm_pool[], nm_latin_fold[][3];
static void put(char *b, size_t *l, size_t cap, const char *s) { while (*s && *l + 1 < cap) b[(*l)++] = *s++; b[*l] = 0; }

/* Hiragana U+3041..U+3096 (katakana maps by -0x60). Index = cp - 0x3041. */
static const char *const KANA[86] = {
 "A","A","I","I","U","U","E","E","O","O","KA","GA","KI","GI","KU","GU","KE","GE","KO","GO",
 "SA","ZA","SHI","JI","SU","ZU","SE","ZE","SO","ZO","TA","DA","CHI","JI","*TSU","TSU","ZU","TE","DE","TO","DO",
 "NA","NI","NU","NE","NO","HA","BA","PA","HI","BI","PI","FU","BU","PU","HE","BE","PE","HO","BO","PO",
 "MA","MI","MU","ME","MO","*YA","YA","*YU","YU","*YO","YO","RA","RI","RU","RE","RO","WA","WA","WI","WE","WO","N","VU","KA","KE"};
/* Half-width katakana U+FF66..U+FF9F -> the full-width katakana (or combining voicing mark) it stands for. */
static const uint16_t HALFWIDTH[58] = {
 0x30F2,0x30A1,0x30A3,0x30A5,0x30A7,0x30A9,0x30E3,0x30E5,0x30E7,0x30C3,0x30FC,                 /* FF66 ｦ .. FF70 ｰ */
 0x30A2,0x30A4,0x30A6,0x30A8,0x30AA,0x30AB,0x30AD,0x30AF,0x30B1,0x30B3,0x30B5,0x30B7,0x30B9,0x30BB,0x30BD, /* ｱ .. ｿ */
 0x30BF,0x30C1,0x30C4,0x30C6,0x30C8,0x30CA,0x30CB,0x30CC,0x30CD,0x30CE,0x30CF,0x30D2,0x30D5,0x30D8,0x30DB, /* ﾀ .. ﾎ */
 0x30DE,0x30DF,0x30E0,0x30E1,0x30E2,0x30E4,0x30E6,0x30E8,0x30E9,0x30EA,0x30EB,0x30EC,0x30ED,0x30EF,0x30F3, /* ﾏ .. ﾝ */
 0x3099,0x309A};                                                                                /* ﾞ ﾟ */

static void emit_kana(nm_kana_t *st, uint32_t cp, char *b, size_t *l, size_t cap) {
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
    const char *k = KANA[cp - 0x3041];
    if (k[0] == '*' && k[1] == 'T') { st->pending_sokuon = 1; return; }
    if (k[0] == '*') {                  /* small ya/yu/yo: KI+YA -> KYA, SHI+YA -> SHA */
        const char *pk = st->prev_kana;
        size_t pl = pk ? strlen(pk) : 0;
        bool after_i = pl && pk[pl - 1] == 'I' && *l && b[*l - 1] == 'I';
        if (after_i) {
            (*l)--;
            if (!strcmp(pk, "SHI") || !strcmp(pk, "CHI") || !strcmp(pk, "JI")) put(b, l, cap, k + 2);
            else put(b, l, cap, k + 1);
        } else put(b, l, cap, k + 1);
        st->prev_kana = NULL;
        return;
    }
    if (st->pending_sokuon) { char c[2] = { k[0] == 'C' ? 'T' : k[0], 0 }; put(b, l, cap, c); st->pending_sokuon = 0; }
    put(b, l, cap, k);
    st->prev_kana = k;
}
static const char *const CYR[64] = { /* U+0410..U+044F, upper then lower */
 "A","B","V","G","D","E","ZH","Z","I","Y","K","L","M","N","O","P","R","S","T","U","F","KH","TS","CH","SH","SHCH","","Y","","E","YU","YA",
 "A","B","V","G","D","E","ZH","Z","I","Y","K","L","M","N","O","P","R","S","T","U","F","KH","TS","CH","SH","SHCH","","Y","","E","YU","YA"};
static const char *greek(uint32_t cp) {
    static const char *const G[25] = {"A","V","G","D","E","Z","I","TH","I","K","L","M","N","X","O","P","R","","S","T","Y","F","CH","PS","O"};
    switch (cp) { /* tonos forms */
    case 0x386: case 0x3AC: return "A"; case 0x388: case 0x3AD: return "E"; case 0x389: case 0x3AE: return "I";
    case 0x38A: case 0x3AF: case 0x3CA: case 0x390: return "I"; case 0x38C: case 0x3CC: return "O";
    case 0x38E: case 0x3CD: case 0x3CB: case 0x3B0: return "Y"; case 0x38F: case 0x3CE: return "O"; case 0x3C2: return "S";
    case 0x3AA: return "I"; case 0x3AB: return "Y"; }            /* dialytika capitals */
    if (cp >= 0x391 && cp <= 0x3A9) return G[cp - 0x391];
    if (cp >= 0x3B1 && cp <= 0x3C9) return G[cp - 0x3B1];
    return NULL;
}
bool nm_emit_script(nm_kana_t *st, uint32_t cp, nm_lang_t lang, char *b, size_t *l, size_t cap) {
    if (cp >= 0xFF66 && cp <= 0xFF9F) cp = HALFWIDTH[cp - 0xFF66];   /* half-width katakana: as full-width */
    bool kana = (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30F6) || cp == 0x3099 || cp == 0x309A;
    if (!kana) nm_kana_reset(st);                                  /* any non-kana output ends a kana run */
    if ((cp >= 0x300 && cp <= 0x36F)) return true;                   /* combining marks: drop */
    if (cp == 0x3099 || cp == 0x309A) {                              /* kana voicing on previous syllable */
        static const char *const V[][3] = {{"TS","Z","Z"},{"CH","J","J"},{"SH","J","J"},{"K","G","G"},{"S","Z","Z"},{"T","D","D"},{"H","B","P"},{"F","B","P"}};   /* two-letter onsets first */
        for (size_t i = 0; i < sizeof V / sizeof *V; i++) {
            size_t n = strlen(V[i][0]);
            if (*l >= n + 1 && !strncmp(b + *l - n - 1, V[i][0], n)) {
                char vowel = b[*l - 1]; *l -= n + 1; put(b, l, cap, V[i][cp == 0x3099 ? 1 : 2]);
                char vv[2] = { vowel, 0 }; put(b, l, cap, vv); return true;
            }
        }
        return true;
    }
    if (cp == 0x30FC) return true;                                   /* long vowel mark: dropped */
    if ((cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30F6)) { emit_kana(st, cp, b, l, cap); return true; }
    if (cp >= 0x4E00 && cp <= 0x9FFF) {
        uint32_t i = cp - 0x4E00;
        if (lang == NM_LANG_KO && nm_han_hangul[i]) { nm_hangul_rr(nm_han_hangul[i], b, l, cap); return true; }
        if (lang == NM_LANG_JA && nm_han_japanese[i]) {
            const char *s = nm_pool + nm_han_japanese[i];
            while (*s) { uint32_t c; s += nm_utf8_next(s, &c); nm_emit_script(st, c, lang, b, l, cap); }
            return true;
        }
        if (nm_han_mandarin[i]) { put(b, l, cap, nm_pool + nm_han_mandarin[i]); return true; }
        return false;
    }
    if (cp >= 0x410 && cp <= 0x44F) { put(b, l, cap, CYR[cp - 0x410]); return true; }
    if (cp == 0x401 || cp == 0x451) { put(b, l, cap, "E"); return true; }
    if (cp == 0x406 || cp == 0x456) { put(b, l, cap, "I"); return true; }
    if (cp == 0x407 || cp == 0x457) { put(b, l, cap, "YI"); return true; }
    if (cp == 0x404 || cp == 0x454) { put(b, l, cap, "YE"); return true; }
    if (cp == 0x490 || cp == 0x491) { put(b, l, cap, "G"); return true; }
    const char *g = greek(cp); if (g) { put(b, l, cap, g); return true; }
    if (cp >= 0xC0 && cp < 0x250) { put(b, l, cap, nm_latin_fold[cp - 0xC0]); return true; }
    if (cp >= 0x1E00 && cp < 0x1F00) { put(b, l, cap, nm_latin_fold[0x190 + cp - 0x1E00]); return true; }
    if (cp == 0x3000) { put(b, l, cap, "_"); return true; }           /* ideographic space */
    if (cp >= 0xFF01 && cp <= 0xFF5E) {                              /* full-width ASCII */
        char c = (char)(cp - 0xFEE0); if (c >= 'a' && c <= 'z') c -= 32;
        char s[2] = { ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') ? c : '_', 0 };
        put(b, l, cap, s); return true;
    }
    return false;
}
