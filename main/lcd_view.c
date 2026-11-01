/*
 * lcd_view.c - the view model, the status-word selection and the repaint diff.
 *
 * Pure C, libc only.  Nothing here draws: the painter asks for the words and
 * for the changed regions, and this file answers.
 */
#include "lcd_view.h"

#include <stdio.h>
#include <string.h>

unsigned lcd_view_diff(const lcd_shown_t *shown, const lcd_view_t *want) {
    unsigned r = 0;

    if (!shown || !want) {
        return 0;
    }

    if (strcmp(shown->v.ip, want->ip) != 0) {
        r |= LCD_R_IP;
    }
    if (strcmp(shown->v.ssid, want->ssid) != 0) {
        r |= LCD_R_SSID;
    }
    if (shown->v.bars != want->bars) {
        r |= LCD_R_BARS;
    }
    /* The password is a status-line word: it replaces the link word and "Err" while setup mode lasts, so a new
     * value (or leaving setup mode) must repaint the row even though no boolean changed. */
    if (strcmp(shown->v.ap_pw, want->ap_pw) != 0) {
        r |= LCD_R_STATUS;
    }
    if (shown->v.connected != want->connected ||
        shown->v.connecting != want->connecting ||
        shown->v.error != want->error ||
        shown->v.locked != want->locked) {
        r |= LCD_R_STATUS;
    }

    return r;
}

/* "http://" is what tells a person who has never seen this device that the value goes in a
 * browser.  It is part of the same line, not a separate row: one row of "what to type" beats
 * two rows of "here is a prefix, here is an address", and it costs 43 px of a 160 px panel. */
void lcd_view_addr_line(const char *ip, char *out, size_t out_sz) {
    if (!out || !out_sz) {
        return;
    }
    out[0] = '\0';
    if (!ip || !ip[0]) {
        return;                                /* blank address: blank row, never a stale one */
    }
    char tmp[40];
    int n = snprintf(tmp, sizeof tmp, "http://%s", ip);
    if (n <= 0 || (size_t)n >= sizeof tmp || (size_t)n >= out_sz) {
        return;                                /* never a partial URL */
    }
    memcpy(out, tmp, (size_t)n + 1);
}

int lcd_view_words(const lcd_view_t *v, const char **words, int max) {
    int n = 0;

    if (!words || max <= 0) {
        return 0;
    }

    /* Setup mode: the AP password is the only thing that matters on that screen, and it wins over the link
     * word, "Err" and the padlock (there is no card work and no user password there). Two words with the colon
     * attached, so the painter can shrink the gap between them if it must; the digits are never cut. */
    if (v && v->ap_pw[0]) {
        words[n++] = "PW:";
        if (n < max) words[n++] = v->ap_pw;
        return n;
    }

    /* "Connecting" is the default, pre-Wi-Fi state: an all-false view is not
     * "not connected", it is still trying. */
    words[n++] = (v && v->connected) ? "Connected" : "Connecting";

    /* Error joins the link word; it never replaces it.  "Err" rather than
     * "Error": "Err" is 24 px at the natural advance, which leaves room for the
     * padlock on the same row where "Error" did not. */
    if (v && v->error && n < max) {
        words[n++] = "Err";
    }

    return n;
}

int lcd_view_bars_from_rssi(int rssi, bool connected) {
    if (!connected || rssi >= 0) {
        return 0;   /* net_rssi() reports 0 when the STA is not connected */
    }
    if (rssi > -55) return 4;
    if (rssi > -65) return 3;
    if (rssi > -75) return 2;
    if (rssi > -85) return 1;
    return 0;
}

bool lcd_view_should_error(bool sd_ok, uint32_t sd_missing_ms) {
    return !sd_ok && sd_missing_ms >= 3000u;
}

/* ---- The tight pen: glyphs cropped to their ink ----
 *
 * The natural cell advance (font_glyph_advance: 8 or 16 px) is the layout's
 * rhythm; cropping each glyph to its inked columns and leaving `gap` px
 * between them recovers the side bearings, so a line is narrower than its cell
 * count suggests -- "192.168.1.225" is 104 px on the cells and 99 px with the
 * gap closed.  Same glyphs, same 16 px height, nothing scaled.  This is the pen
 * the 152 px full-width column is measured with.
 */

#define PEN_SPACE_PX 4

int pen_span(uint32_t cp, int *first, int *width) {
    const uint16_t *rows = font_rows(cp);
    int lo = 16, hi = -1;
    for (int r = 0; r < 16; r++) {
        uint16_t bits = rows[r];
        for (int c = 0; c < 16; c++) {
            if (bits & (uint16_t)(0x8000u >> c)) {
                if (c < lo) lo = c;
                if (c > hi) hi = c;
            }
        }
    }
    if (hi < 0) {
        return 0;
    }
    if (first) *first = lo;
    if (width) *width = hi - lo + 1;
    return hi - lo + 1;
}

int pen_px(const char *s, int gap) {
    int px = 0, n = 0;
    const char *p = s;
    while (p && *p) {
        int w = pen_span(font_utf8_next(&p), NULL, NULL);
        px += w ? w : PEN_SPACE_PX;
        n++;
    }
    return n ? px + gap * (n - 1) : 0;
}

int pen_fit(const char *s, int max_px, int gap, char *out, size_t out_sz) {
    static const char ellipsis[] = "\xE2\x80\xA6";     /* U+2026 */
    if (!out || out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!s || max_px <= 0) {
        return 0;
    }

    bool cut = pen_px(s, gap) > max_px;
    int ell_px = pen_px(ellipsis, 0) + gap;            /* the cut mark and its gap */
    int budget = cut ? max_px - ell_px : max_px;

    size_t used = 0;
    int px = 0, n = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        int w = pen_span(font_utf8_next(&q), NULL, NULL);
        int add = (w ? w : PEN_SPACE_PX) + (n ? gap : 0);
        if (px + add > budget) {
            break;
        }
        size_t len = (size_t)(q - p);
        if (used + len > out_sz - 1) {
            break;                                     /* never split a sequence */
        }
        memcpy(out + used, p, len);
        used += len;
        px += add;
        n++;
        p = q;
    }

    if (cut && px + ell_px <= max_px && used + sizeof ellipsis - 1 <= out_sz - 1) {
        memcpy(out + used, ellipsis, sizeof ellipsis - 1);
        used += sizeof ellipsis - 1;
        px += ell_px;
    }
    out[used] = '\0';
    return px;
}
