/*
 * font_lookup.c - codepoint -> glyph rows, UTF-8 walking and codepoint-safe
 * ellipsis.  See font_lookup.h for the contracts.
 */
#include "font_lookup.h"

#include <stdbool.h>
#include <string.h>

#include "lcd.h"

#define GLYPH_ROWS 16u

/* Linear walk: the table has a handful of ranges, in ascending order.  Returns
 * NULL when cp is outside every range. */
static const uint16_t *range_rows(uint32_t cp) {
    for (unsigned i = 0; i < font_range_count; i++) {
        const font_range_t *r = &font_ranges[i];
        if (cp >= r->first && cp <= r->last) {
            return &font_glyphs[(r->offset + (cp - r->first)) * GLYPH_ROWS];
        }
    }
    return NULL;
}

const uint16_t *font_rows(uint32_t cp) {
    const uint16_t *rows = range_rows(cp);
    if (rows) {
        return rows;
    }
    rows = range_rows('?');
    if (rows) {
        return rows;
    }
    /* The generator asserts that '?' exists; keep the never-NULL promise even
     * for a broken table. */
    return font_glyphs;
}

uint32_t font_utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned char b = s[0];

    if (b == 0) {
        return 0;                       /* do not step past the terminator */
    }
    if (b < 0x80) {
        *p = (const char *)(s + 1);
        return b;
    }

    int need;
    uint32_t cp, min;
    if ((b & 0xE0) == 0xC0) {
        need = 1; cp = b & 0x1F; min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        need = 2; cp = b & 0x0F; min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        need = 3; cp = b & 0x07; min = 0x10000;
    } else {
        *p = (const char *)(s + 1);     /* continuation or 0xF8..0xFF */
        return 0xFFFD;
    }

    const unsigned char *q = s + 1;
    int consumed = 1;
    for (int i = 0; i < need; i++) {
        if (*q < 0x80 || *q > 0xBF) {
            break;                      /* NUL also stops here */
        }
        cp = (cp << 6) | (*q & 0x3F);
        q++;
        consumed++;
    }
    if (consumed != need + 1 || cp < min || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *p = (const char *)(s + consumed);
        return 0xFFFD;
    }
    *p = (const char *)q;
    return cp;
}

int font_text_width(const char *s) {
    int n = 0;
    const char *p = s;
    while (*p) {
        (void)font_utf8_next(&p);
        n++;
    }
    return n;
}

/* Byte length of the codepoint at s (s is not NUL). */
static size_t utf8_len(const char *s) {
    const char *p = s;
    (void)font_utf8_next(&p);
    return (size_t)(p - s);
}

int font_ellipsize(const char *s, int max_cells, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!s || max_cells <= 0) {
        return 0;
    }

    int total = font_text_width(s);
    int keep = total;
    bool cut = false;
    if (total > max_cells) {
        keep = max_cells - 1;           /* max_cells >= 1 here */
        cut = true;
    }

    size_t used = 0;
    int cells = 0;
    const char *p = s;
    while (cells < keep) {
        size_t len = utf8_len(p);
        if (used + len > out_sz - 1) {
            goto done;                  /* never split a sequence */
        }
        memcpy(out + used, p, len);
        used += len;
        p += len;
        cells++;
    }
    if (cut) {
        static const char ellipsis[] = "\xE2\x80\xA6";   /* U+2026 */
        size_t len = sizeof ellipsis - 1;
        if (used + len <= out_sz - 1) {
            memcpy(out + used, ellipsis, len);
            used += len;
            cells++;
        }
    }

done:
    out[used] = '\0';
    return cells;
}

int font_glyph_advance(uint32_t cp) {
    /* Unifont 8x16 glyphs are stored left-aligned in the 16x16 cell with every
     * low byte zero; full-width ones ink at least one of those low bits.  Read
     * the data, not the codepoint's block: that is what makes a full-width
     * Hangul syllable and a half-width ASCII letter both right, and it also
     * catches the few 16x16 jamo that are drawn narrow. */
    const uint16_t *rows = font_rows(cp);
    for (int r = 0; r < 16; r++) {
        if ((rows[r] & 0x00FFu) != 0) {
            return 16;
        }
    }
    return 8;
}

int font_text_px(const char *s) {
    int px = 0;
    const char *p = s;
    while (p && *p) {
        px += font_glyph_advance(font_utf8_next(&p));
    }
    return px;
}

#define ELLIPSIS "\xE2\x80\xA6"     /* U+2026 */
#define ELLIPSIS_PX 16

int font_ellipsize_px(const char *s, int max_px, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!s || max_px <= 0) {
        return 0;
    }

    bool cut = font_text_px(s) > max_px;
    int budget = cut ? max_px - ELLIPSIS_PX : max_px;   /* room kept for the cut mark */

    size_t used = 0;
    int px = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        int adv = font_glyph_advance(font_utf8_next(&q));
        if (px + adv > budget) {
            break;                      /* the rest of the string does not fit */
        }
        size_t len = (size_t)(q - p);
        if (used + len > out_sz - 1) {
            break;                      /* never split a sequence */
        }
        memcpy(out + used, p, len);
        used += len;
        px += adv;
        p = q;
    }

    if (cut && px + ELLIPSIS_PX <= max_px && used + sizeof ELLIPSIS - 1 <= out_sz - 1) {
        memcpy(out + used, ELLIPSIS, sizeof ELLIPSIS - 1);
        used += sizeof ELLIPSIS - 1;
        px += ELLIPSIS_PX;
    }
    out[used] = '\0';
    return px;
}
