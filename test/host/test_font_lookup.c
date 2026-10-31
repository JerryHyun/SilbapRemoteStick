#include "t.h"
#include "font_lookup.h"
#include "lcd.h"
#include <string.h>

/* The plan's Step 1 cases. */
static void test_lookup(void) {
    T_CHECK(font_rows('A') != NULL);
    T_CHECK(font_rows('A') != font_rows('B'));
    /* Stage 2: the Hangul blocks are in the table. */
    T_CHECK(font_rows(0xAC00) != font_rows('?'));   /* 가 now has its own glyph */
    T_CHECK(font_rows(0xAC01) != font_rows(0xAC00));
    T_CHECK(font_rows(0xD7A3) != font_rows('?'));   /* last syllable */
    T_CHECK(font_rows(0x3131) != font_rows('?'));   /* compatibility jamo */
    T_CHECK(font_rows(0x10FFFF) == font_rows('?'));
}

static void test_utf8(void) {
    const char *p = "\xEC\x9E\xA5" "A";              /* 장A */
    /* The plan wrote U+AC00 here; EC 9E A5 is actually U+C7A5 (장), while 가 is
     * EA B0 80.  Assert the bytes, not the plan's typo. */
    T_EQ_INT(font_utf8_next(&p), 0xC7A5);
    T_EQ_INT(font_utf8_next(&p), 'A');
    p = "\xEA\xB0\x80";                              /* 가, U+AC00 */
    T_EQ_INT(font_utf8_next(&p), 0xAC00);
    p = "\xFF";
    T_EQ_INT(font_utf8_next(&p), 0xFFFD);
}

static void test_ellipsize(void) {
    char b[64];
    T_EQ_INT(font_ellipsize("MyHome", 6, b, sizeof b), 6); T_EQ_STR(b, "MyHome");
    /* 6 cells = 5 characters + U+2026: the ellipsis must fill the width it was
     * given.  (The plan's "MyHo..." is 5 cells and leaves a column blank.) */
    T_EQ_INT(font_ellipsize("MyHomeNetwork", 6, b, sizeof b), 6); T_EQ_STR(b, "MyHom\xE2\x80\xA6");
    T_EQ_INT(font_ellipsize("\xEC\x9E\xA5\xEB\xAF\xB8\xEC\x9D\x98\xEC\xA7\x91", 3, b, sizeof b), 3);
    T_EQ_STR(b, "\xEC\x9E\xA5\xEB\xAF\xB8\xE2\x80\xA6");   /* never a split sequence */
}

/* Stage 2: the Hangul glyphs must be real bitmaps, not blank cells.  A wrong
 * table offset would still make font_rows(0xAC00) != font_rows('?'), so check
 * that each glyph has ink and that the jamo and syllables are distinct shapes.
 */
static void test_hangul_glyphs(void) {
    static const uint32_t cps[] = {0xAC00, 0xAC01, 0xD7A3, 0x3131, 0x3132};
    for (unsigned i = 0; i < sizeof cps / sizeof cps[0]; i++) {
        const uint16_t *g = font_rows(cps[i]);
        int ink = 0;
        for (int r = 0; r < 16; r++) {
            ink += g[r] != 0;
        }
        T_CHECK(ink > 0);                    /* the glyph has pixels */
        /* Hangul is full-width in Unifont: the right half is in use, so the
         * low byte is not uniformly zero the way a half-width ASCII row is. */
        if (cps[i] == 0xAC00) {
            int low = 0;
            for (int r = 0; r < 16; r++) {
                low += (g[r] & 0xFF) != 0;
            }
            T_CHECK(low > 0);
        }
    }
    /* A syllable inside the run is not the same shape as another one. */
    T_CHECK(memcmp(font_rows(0xAC00), font_rows(0xAC01), 16 * sizeof(uint16_t)) != 0);
    /* The fallback region is still exactly U+003F's glyph. */
    T_CHECK(memcmp(font_rows(0x10FFFF), font_rows('?'), 16 * sizeof(uint16_t)) == 0);
}

/* Edges the painter will hit. */
static void test_lookup_edges(void) {
    const uint16_t *q = font_rows('?');
    T_CHECK(q != NULL);
    T_CHECK(q == font_rows(0x10FFFF));               /* the fallback itself */
    T_CHECK(q == font_rows(0xE000));                 /* private use, unassigned */
    /* space is an all-zero cell: nothing painted, but a cell wide */
    for (int i = 0; i < 16; i++) {
        T_EQ_INT(font_rows(0x20)[i], 0);
    }
    /* the ellipsis and the middle dot are inside the sparse runs */
    T_CHECK(font_rows(0x2026) != q);
    T_CHECK(font_rows(0xB7) != q);
    T_CHECK(font_rows(0x2010) != q);
    T_EQ_INT(font_rows(0x25CF) != q, 1);             /* the status bullet */
}

static void test_utf8_edges(void) {
    const char *p;

    p = "\xC3\xA9";                                  /* é, 2-byte */
    T_EQ_INT(font_utf8_next(&p), 0xE9);
    T_EQ_INT(*p, 0);

    p = "\xE2\x82\xAC";                              /* €, 3-byte */
    T_EQ_INT(font_utf8_next(&p), 0x20AC);
    T_EQ_INT(*p, 0);

    p = "\x80";                                      /* lone continuation byte */
    T_EQ_INT(font_utf8_next(&p), 0xFFFD);
    T_EQ_INT(*p, 0);

    p = "\xE2\x82";                                  /* truncated 3-byte */
    T_EQ_INT(font_utf8_next(&p), 0xFFFD);
    T_EQ_INT(*p, 0);                                 /* advanced past both bytes */

    p = "\xE2" "A";                                  /* lead byte then ASCII */
    T_EQ_INT(font_utf8_next(&p), 0xFFFD);
    T_EQ_INT(*p, 'A');

    p = "\xC0\x80";                                  /* overlong NUL */
    T_EQ_INT(font_utf8_next(&p), 0xFFFD);
    T_EQ_INT(*p, 0);

    p = "\xED\xA0\x80";                              /* UTF-16 surrogate */
    T_EQ_INT(font_utf8_next(&p), 0xFFFD);
    T_EQ_INT(*p, 0);

    p = "";                                          /* the terminator itself */
    T_EQ_INT(font_utf8_next(&p), 0);
    T_EQ_INT(*p, 0);
}

static void test_ellipsize_edges(void) {
    char b[64] = "garbage";

    T_EQ_INT(font_ellipsize("", 6, b, sizeof b), 0);
    T_EQ_STR(b, "");

    memset(b, 'x', sizeof b);
    T_EQ_INT(font_ellipsize("abc", 0, b, sizeof b), 0);
    T_EQ_STR(b, "");

    /* one cell: only the ellipsis fits, so show that it was cut -- never a
     * bare first character that would read as the whole name */
    T_EQ_INT(font_ellipsize("abc", 1, b, sizeof b), 1);
    T_EQ_STR(b, "\xE2\x80\xA6");
    /* a single-codepoint string is not truncated at max_cells 1 */
    T_EQ_INT(font_ellipsize("a", 1, b, sizeof b), 1);
    T_EQ_STR(b, "a");

    /* exactly max_cells: no ellipsis */
    T_EQ_INT(font_ellipsize("abc", 3, b, sizeof b), 3);
    T_EQ_STR(b, "abc");

    /* an ellipsis that cannot fit is dropped; the kept cells still fit */
    T_EQ_INT(font_ellipsize("abcdef", 4, b, 4), 3);  /* "abc" + NUL */
    T_EQ_STR(b, "abc");

    /* a 3-byte sequence that would straddle the end is not written */
    T_EQ_INT(font_ellipsize("\xEC\x9E\xA5\xEB\xAF\xB8", 2, b, 5), 1);  /* room for 장 + NUL */
    T_EQ_STR(b, "\xEC\x9E\xA5");

    T_EQ_INT(font_ellipsize("abc", 5, NULL, 0), 0);  /* no buffer is a no-op */
    T_EQ_INT(font_ellipsize("abc", 5, b, 0), 0);
}

static void test_width(void) {
    T_EQ_INT(font_text_width(""), 0);
    T_EQ_INT(font_text_width("abc"), 3);
    T_EQ_INT(font_text_width("\xEC\x9E\xA5\xEB\xAF\xB8"), 2);   /* two codepoints */
    T_EQ_INT(font_text_width("\x80"), 1);                        /* invalid byte, one cell */
    T_EQ_INT(font_text_width("MyHomeNetwork"), 13);
}

int main(void) {
    T_RUN(test_lookup);
    T_RUN(test_hangul_glyphs);
    T_RUN(test_lookup_edges);
    T_RUN(test_utf8);
    T_RUN(test_utf8_edges);
    T_RUN(test_ellipsize);
    T_RUN(test_ellipsize_edges);
    T_RUN(test_width);
    T_DONE();
}
