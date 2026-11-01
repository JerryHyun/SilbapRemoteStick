/*
 * lcd_view.h - what the panel should say and which regions changed.
 *
 * Pure C, libc only: no ESP-IDF includes, so this compiles and is tested on
 * the host.  The painter (lcd_ui.c) owns the pixels; this module owns the model
 * and the binding layout rectangles.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Glyph geometry (font_glyph_advance, font_text_px, font_ellipsize_px) is
 * declared in font_lookup.h: it reads the glyph data, so it lives with it.
 * Re-exported here because the layout is what the painter needs it for. */
#include "font_lookup.h"

/* The 160x80 panel, binding layout (spec, "Layout" and "Decisions"):
 *
 *   x 4..155    the full-width column: 152 px of the panel's 160
 *   y  4..19    line 1  http://<ip>          (lcd_view_addr_line)
 *   y 28..43    line 2  SSID + signal bars
 *   y 52..67    line 3  status words + lock glyph
 *
 * Each row is 16 px high; the 8 px row gaps keep 16 px glyphs from touching,
 * and the 4 px column inset matches the 4 px top margin.  Nothing may be drawn
 * outside 160x80; the painter clips anyway.
 */
#define LCD_PANEL_W       160
#define LCD_PANEL_H       80

/* The 66x66 QR box: removed in Task 2, together with paint_qr(), which is the
 * only thing that still uses it.  It stays here until then so this commit keeps
 * the panel painting. */
#define LCD_QR_X          0           /* removed in Task 2 */
#define LCD_QR_Y          7           /* removed in Task 2 */
#define LCD_QR_W          66          /* removed in Task 2 */
#define LCD_QR_H          66          /* removed in Task 2 */

#define LCD_COL_X         4
#define LCD_COL_W         152

#define LCD_ROW_ADDR_Y    4
#define LCD_ROW_IP_Y      LCD_ROW_ADDR_Y  /* old name: paint_ip() still uses it; removed in Task 2 */
#define LCD_ROW_SSID_Y    28
#define LCD_ROW_STATUS_Y  52
#define LCD_ROW_H         16

typedef struct {
    char ip[16];        /* "" when there is no address */
    char ssid[33];      /* UTF-8 as reported by the driver */
    char ap_pw[9];      /* setup AP password (8 digits): only while NET_SETUP, "" otherwise */
    int  bars;          /* 0..4; -1 = draw no bars (setup mode) */
    bool connected, connecting, error, locked;
} lcd_view_t;

typedef struct { lcd_view_t v; bool qr_valid; } lcd_shown_t;

/* Which regions differ between what is on the glass and want. */
enum { LCD_R_IP = 1, LCD_R_SSID = 2, LCD_R_BARS = 4, LCD_R_STATUS = 8, LCD_R_QR = 16 };

/* The union of changed region bits (0 when nothing changed).
 *
 * LCD_R_QR is set when the IP differs, when the shown view has no valid code,
 * or when the wanted IP is "" -- a blank address must clear the code rather
 * than leave a stale one on the glass. */
unsigned lcd_view_diff(const lcd_shown_t *shown, const lcd_view_t *want);

/* The address row's text: "http://<ip>" in out.  "" when ip is empty (a blank
 * address must leave a blank row, never a stale one) and "" when out_sz cannot
 * hold the whole URL -- a partial "http://192.16" is worse than nothing.
 * out is always NUL-terminated when out_sz > 0. */
void lcd_view_addr_line(const char *ip, char *out, size_t out_sz);

/* The status words in draw order.  Normally: the link word always, then "Err" when the
 * error flag is set; locked is a flag for the caller, never a word.
 *
 * Setup mode is the exception: when ap_pw is set (NET_SETUP only), the words are
 * {"PW:", ap_pw} -- the password is the only thing that matters on that screen
 * (there is no link to report, no card work and no user password), so it wins over
 * connected/error/locked.  Never cut the digits: a truncated password is unusable.
 * The painter's two-word fit path shrinks the glyph gap and then the word gap.
 *
 * Writes at most max pointers and returns the count written (0 when max <= 0). */
int lcd_view_words(const lcd_view_t *v, const char **words, int max);

/* RSSI in dBm -> 0..4 bars.  A link that is not connected, or an RSSI of 0
 * (net_rssi()'s "not connected" value), reads as 0 bars. */
int lcd_view_bars_from_rssi(int rssi, bool connected);

/* ---- Geometry that is pure arithmetic, kept out of the painter ----
 *
 * The largest integer scale (px per module) at which a modules x modules code
 * fits in box_px, or 0 when not even 1 px per module fits.  Never a
 * fractional scale: a phone camera needs the modules square and integral. */
int qr_px_per_module(int modules, int box_px);

/* True once the card has been missing for hold_ms (the spec's 3 s hold).
 * sd_missing_ms is how long refresh_status().sd_ok has been false; it is
 * ignored, so any value reads false, while the card is present. */
bool lcd_view_should_error(bool sd_ok, uint32_t sd_missing_ms);

/* ---- The tight pen: glyphs cropped to their ink ----
 *
 * The natural advance (font_glyph_advance) is the layout's rhythm: "192.168.1.225"
 * is 104 px at 8 px per cell.  This crops each glyph to its inked columns and
 * leaves `gap` px between glyphs, which is what the painter draws with -- the
 * 152 px column is measured on this pen, not on the natural advance.  A blank
 * glyph advances 4 px.
 *
 * pen_span: the ink width of one glyph, plus its first inked column (either
 * out pointer may be NULL).  0 for a blank glyph.
 * pen_px: what s occupies on the pen.
 * pen_fit: cut s to max_px, keeping room for a 16 px ellipsis when cutting;
 *   writes UTF-8 to out without splitting a sequence and returns the result's
 *   pixel width (0 when nothing, not even the ellipsis, fits). */
int pen_span(uint32_t cp, int *first, int *width);
int pen_px(const char *s, int gap);
int pen_fit(const char *s, int max_px, int gap, char *out, size_t out_sz);
