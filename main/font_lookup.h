/*
 * font_lookup.h - codepoint -> glyph rows, UTF-8 walking and codepoint-safe
 * ellipsis.  Pure C: no ESP-IDF includes, so it compiles on the host.
 *
 * The glyph table itself is generated into main/font_data.c; font_range_t and
 * the table's extern declarations live in lcd.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The 16 rows of cp's glyph (bit 15 = leftmost pixel), or the '?' glyph's rows
 * when cp is outside every range.  Never NULL. */
const uint16_t *font_rows(uint32_t cp);

/* Decodes one UTF-8 codepoint and advances *p past it.  At the terminating NUL
 * it returns 0 and does not advance.  An invalid byte (including a lone
 * continuation byte) yields 0xFFFD and advances one byte; a truncated or
 * otherwise malformed sequence yields 0xFFFD and advances past the bytes it
 * consumed.  Never reads past the NUL. */
uint32_t font_utf8_next(const char **p);

/* Glyph cells needed: one per codepoint (an invalid byte counts as one). */
int font_text_width(const char *s);

/* Copies at most max_cells codepoints of s into out and NUL-terminates.
 *
 * When s is longer than max_cells, the last of the max_cells cells is reserved
 * for '...' (U+2026) and only max_cells-1 characters are kept, so the result
 * fits the requested width and says it was cut.  max_cells == 1 therefore
 * truncates to the ellipsis alone; max_cells <= 0 writes "" and returns 0.
 *
 * If out_sz cannot hold the result, as many whole codepoints as fit are
 * written (the ellipsis is dropped if it does not fit), out is always
 * NUL-terminated, and the number of cells actually written is returned.
 * A UTF-8 sequence is never split.  out_sz == 0 or out == NULL writes nothing
 * and returns 0.  Returns the cell count written. */
int font_ellipsize(const char *s, int max_cells, char *out, size_t out_sz);

/* Pixels one glyph advances the pen: 8 for a half-width glyph (Unifont 8x16,
 * stored left-aligned in the cell with the low byte zero) and 16 for a
 * full-width one.  The width comes from the glyph data, never from the
 * codepoint's block. */
int font_glyph_advance(uint32_t cp);

/* Pixels a whole string occupies: the sum of font_glyph_advance() over its
 * codepoints (an invalid byte counts as one '?' cell). */
int font_text_px(const char *s);

/* The same cut measured in pixels, for a variable-width column: keep whole
 * codepoints while their advances fit in max_px, and when the string does not
 * fit reserve the ellipsis's own advance (16 px) plus the ellipsis itself.
 * max_px <= 0 writes "" and returns 0.  Returns the pixels the result occupies
 * (0 when not even the ellipsis fits).  A UTF-8 sequence is never split. */
int font_ellipsize_px(const char *s, int max_px, char *out, size_t out_sz);
