#include "t.h"
#include "lcd_view.h"
#include <string.h>

static lcd_view_t view(const char *ip, const char *ssid, int bars,
                       bool connected, bool connecting, bool error, bool locked) {
    lcd_view_t v;
    memset(&v, 0, sizeof v);
    strncpy(v.ip, ip, sizeof v.ip - 1);
    strncpy(v.ssid, ssid, sizeof v.ssid - 1);
    v.bars = bars;
    v.connected = connected;
    v.connecting = connecting;
    v.error = error;
    v.locked = locked;
    return v;
}

static void test_words(void) {
    const char *w[4];
    lcd_view_t v = view("", "", 0, false, false, false, false);

    /* the pre-Wi-Fi default: no booleans at all reads as Connecting */
    T_EQ_INT(lcd_view_words(&v, w, 2), 1);
    T_EQ_STR(w[0], "Connecting");

    v = view("", "", 0, false, true, false, false);
    T_EQ_INT(lcd_view_words(&v, w, 2), 1);
    T_EQ_STR(w[0], "Connecting");

    v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    T_EQ_INT(lcd_view_words(&v, w, 2), 1);
    T_EQ_STR(w[0], "Connected");

    /* Error is a second word, never a replacement */
    v.error = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 2);
    T_EQ_STR(w[0], "Connected");
    T_EQ_STR(w[1], "Err");

    /* locked is a flag, not a word */
    v.error = false;
    v.locked = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 1);
    T_EQ_STR(w[0], "Connected");

    v.error = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 2);
    T_EQ_STR(w[0], "Connected");
    T_EQ_STR(w[1], "Err");

    /* capacity is respected; the link word wins */
    T_EQ_INT(lcd_view_words(&v, w, 1), 1);
    T_EQ_STR(w[0], "Connected");

    T_EQ_INT(lcd_view_words(&v, w, 0), 0);
    T_EQ_INT(lcd_view_words(&v, w, -1), 0);
}

static void test_words_locked_only(void) {
    /* an all-false view with locked set still reads Connecting; the caller
     * adds the lock bitmap itself. */
    lcd_view_t v = view("", "", -1, false, false, false, true);
    const char *w[2];
    T_EQ_INT(lcd_view_words(&v, w, 2), 1);
    T_EQ_STR(w[0], "Connecting");
    T_CHECK(v.locked);
}

static void test_diff(void) {
    lcd_shown_t shown;
    lcd_view_t want;

    /* identical */
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    shown.qr_valid = true;
    want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);

    /* IP change also regenerates the code */
    want.ip[0] = '\0';
    strcpy(want.ip, "192.168.0.43");
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP | LCD_R_QR);

    /* SSID-only */
    want = shown.v;
    strcpy(want.ssid, "OtherHome");
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_SSID);

    /* locked flip is a status change only */
    want = shown.v;
    want.locked = true;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);

    /* connected flip is a status change only */
    want = shown.v;
    want.connected = false;
    want.connecting = true;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);

    /* bars change is the SSID row's bars only */
    want = shown.v;
    want.bars = 2;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_BARS);

    /* an invalid code on the glass forces a repaint */
    shown.qr_valid = false;
    want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_QR);
    shown.qr_valid = true;

    /* a blank IP must clear the code, never show a stale one */
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    shown.qr_valid = true;
    want = shown.v;
    want.ip[0] = '\0';
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP | LCD_R_QR);

    /* blank IP with a code on the glass: clear it once */
    shown.v = view("", "", -1, false, false, false, false);
    shown.qr_valid = true;
    want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_QR);

    /* ...and the pass after the painter cleared it is idle: no relayout loop while the
     * address is blank (the authoritative state, not a per-pass demand) */
    shown.qr_valid = false;
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);

    /* empty -> empty with no code on the glass: nothing to do */
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);

    /* an address appearing after a blank one draws the code again */
    shown.v = view("", "", -1, false, false, false, false);
    shown.qr_valid = false;
    want = view("192.168.0.42", "", -1, false, false, false, false);
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP | LCD_R_QR);

    /* error joins the status line */
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    shown.qr_valid = true;
    want = shown.v;
    want.error = true;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);
}

static void test_layout(void) {
    T_CHECK(LCD_QR_X + LCD_QR_W <= 160 && LCD_QR_Y + LCD_QR_H <= 80);
    T_CHECK(LCD_COL_X + 90 <= 160);
    T_CHECK(LCD_ROW_IP_Y + 16 <= 80);
    T_CHECK(LCD_ROW_SSID_Y + 16 <= 80);
    T_CHECK(LCD_ROW_STATUS_Y + 16 <= 80);
    T_CHECK(LCD_ROW_IP_Y + 16 <= LCD_ROW_SSID_Y);
    T_CHECK(LCD_ROW_SSID_Y + 16 <= LCD_ROW_STATUS_Y);
    T_CHECK(LCD_QR_X + LCD_QR_W <= LCD_COL_X);
    T_EQ_INT(LCD_QR_X, 0);
    T_EQ_INT(LCD_QR_Y, 7);
    T_EQ_INT(LCD_QR_W, 66);
    T_EQ_INT(LCD_QR_H, 66);
    T_EQ_INT(LCD_COL_X, 70);
    T_EQ_INT(LCD_ROW_IP_Y, 4);
    T_EQ_INT(LCD_ROW_SSID_Y, 24);
    T_EQ_INT(LCD_ROW_STATUS_Y, 44);
}

static void test_bars(void) {
    T_EQ_INT(lcd_view_bars_from_rssi(-50, true), 4);
    T_EQ_INT(lcd_view_bars_from_rssi(-56, true), 3);
    T_EQ_INT(lcd_view_bars_from_rssi(-66, true), 2);
    T_EQ_INT(lcd_view_bars_from_rssi(-76, true), 1);
    T_EQ_INT(lcd_view_bars_from_rssi(-90, true), 0);
    T_EQ_INT(lcd_view_bars_from_rssi(0, false), 0);
    T_EQ_INT(lcd_view_bars_from_rssi(0, true), 0);
    /* exactly on a threshold is the lower band */
    T_EQ_INT(lcd_view_bars_from_rssi(-55, true), 3);
    T_EQ_INT(lcd_view_bars_from_rssi(-65, true), 2);
    T_EQ_INT(lcd_view_bars_from_rssi(-75, true), 1);
    T_EQ_INT(lcd_view_bars_from_rssi(-85, true), 0);
    T_EQ_INT(lcd_view_bars_from_rssi(-40, false), 0);
}

static void test_qr_scale(void) {
    /* the plan's four cases: v2 (25 modules) + 4+4 quiet = 33 */
    T_EQ_INT(qr_px_per_module(33, 66), 2);
    T_EQ_INT(qr_px_per_module(33, 65), 1);   /* never 1.9 */
    T_EQ_INT(qr_px_per_module(33, 32), 0);
    T_EQ_INT(qr_px_per_module(21, 66), 3);   /* v1, 13+8 = 21 */
    /* degenerate boxes */
    T_EQ_INT(qr_px_per_module(33, 33), 1);
    T_EQ_INT(qr_px_per_module(33, 0), 0);
    T_EQ_INT(qr_px_per_module(33, -5), 0);
    T_EQ_INT(qr_px_per_module(0, 66), 0);
    T_EQ_INT(qr_px_per_module(-1, 66), 0);
}

static void test_error_hold(void) {
    /* the 3 s hold: nothing while the card is present, nothing before 3 s,
     * Error from 3 s on, and gone again as soon as it is back */
    T_EQ_INT(lcd_view_should_error(true, 0), 0);
    T_EQ_INT(lcd_view_should_error(true, 9999), 0);
    T_EQ_INT(lcd_view_should_error(false, 0), 0);
    T_EQ_INT(lcd_view_should_error(false, 2999), 0);
    T_EQ_INT(lcd_view_should_error(false, 3000), 1);
    T_EQ_INT(lcd_view_should_error(false, 3001), 1);
    T_EQ_INT(lcd_view_should_error(false, 60000), 1);
    T_EQ_INT(lcd_view_should_error(true, 60000), 0);
}

static void test_advance(void) {
    /* half-width ASCII and full-width Hangul, decided by the glyph data */
    T_EQ_INT(font_glyph_advance('A'), 8);
    T_EQ_INT(font_glyph_advance('M'), 8);
    T_EQ_INT(font_glyph_advance('.'), 8);
    T_EQ_INT(font_glyph_advance(' '), 8);
    T_EQ_INT(font_glyph_advance(0xAC00), 16);       /* 가 */
    T_EQ_INT(font_glyph_advance(0xAC01), 16);       /* 각 */
    T_EQ_INT(font_glyph_advance(0xD7A3), 16);       /* 힣 */
    T_EQ_INT(font_glyph_advance(0x3131), 16);       /* ㄱ */
    T_EQ_INT(font_glyph_advance(0x25CF), 8);        /* filled circle */
    T_EQ_INT(font_glyph_advance(0x2026), 8);        /* ellipsis */
    /* a codepoint outside the table renders '?', so it advances like '?' */
    T_EQ_INT(font_glyph_advance(0x10FFFF), font_glyph_advance('?'));

    T_EQ_INT(font_text_px(""), 0);
    T_EQ_INT(font_text_px("AB"), 16);
    T_EQ_INT(font_text_px("192.168.1.225"), 13 * 8);
    T_EQ_INT(font_text_px("\xEA\xB0\x80\xEB\x82\x98"), 32);        /* 가나 */
    T_EQ_INT(font_text_px("A\xEA\xB0\x80"), 24);                     /* A 가 */
    T_EQ_INT(font_text_px("\xEF\xBC\x91"), 8);                       /* a lone invalid byte-ish cell */
}

static void test_ellipsize_px(void) {
    char out[64];

    /* fits whole: untouched, and the pixel count is the plain width */
    T_EQ_INT(font_ellipsize_px("MyHome", 90, out, sizeof out), 48);
    T_EQ_STR(out, "MyHome");

    /* a long ASCII SSID in 90 px: 11 glyphs (88 px) fit, but cutting must
     * leave the 16 px ellipsis, so 9 glyphs + the ellipsis = 88 px */
    T_EQ_INT(font_ellipsize_px("MyHomeNetwork2", 90, out, sizeof out), 88);
    T_EQ_STR(out, "MyHomeNet\xE2\x80\xA6");

    /* the SSID column without bars is 90 px; with bars the text gets 68 */
    T_EQ_INT(font_ellipsize_px("MyHomeNetwork2", 68, out, sizeof out), 64);
    T_EQ_STR(out, "MyHome\xE2\x80\xA6");

    /* full-width text: 5 cells = 80 px in 90, six do not fit */
    T_EQ_INT(font_ellipsize_px("\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4\xEB\x9D\xBC\xEB\xA7\x88", 90, out, sizeof out), 80);
    T_EQ_STR(out, "\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4\xEB\x9D\xBC\xEB\xA7\x88");

    /* mixed: the string is 56 px and fits 56 exactly, so it is not cut */
    T_EQ_INT(font_ellipsize_px("A\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4", 56, out, sizeof out), 56);
    T_EQ_STR(out, "A\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4");
    /* one pixel less and it is: A + 가 (24 px) + the 16 px ellipsis */
    T_EQ_INT(font_ellipsize_px("A\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4", 55, out, sizeof out), 40);
    T_EQ_STR(out, "A\xEA\xB0\x80\xE2\x80\xA6");

    /* no room at all */
    T_EQ_INT(font_ellipsize_px("MyHome", 0, out, sizeof out), 0);
    T_EQ_STR(out, "");
    T_EQ_INT(font_ellipsize_px("MyHome", -1, out, sizeof out), 0);
    T_EQ_STR(out, "");

    /* too small even for the ellipsis: keep what fits, no cut mark */
    T_EQ_INT(font_ellipsize_px("MyHomeNetwork2", 8, out, sizeof out), 0);
    T_EQ_STR(out, "");
    T_EQ_INT(font_ellipsize_px("MyHomeNetwork2", 15, out, sizeof out), 0);

    /* the ellipsis itself fits when there is room for nothing else */
    T_EQ_INT(font_ellipsize_px("MyHomeNetwork2", 16, out, sizeof out), 16);
    T_EQ_STR(out, "\xE2\x80\xA6");

    /* out_sz is respected and never splits a sequence */
    T_EQ_INT(font_ellipsize_px("\xEA\xB0\x80\xEB\x82\x98", 90, out, 4), 16);
    T_EQ_STR(out, "\xEA\xB0\x80");

    /* an empty string is not a cut */
    T_EQ_INT(font_ellipsize_px("", 90, out, sizeof out), 0);
    T_EQ_STR(out, "");
}

static void test_geometry_fits(void) {
    /* the QR the painter draws for a typical address must land inside the
     * binding box and leave a real quiet zone */
    const int v2 = 25;
    int sc = qr_px_per_module(v2 + 8, LCD_QR_W);
    T_EQ_INT(sc, 2);
    T_CHECK(v2 * sc <= LCD_QR_W);
    T_CHECK(LCD_QR_X + v2 * sc <= LCD_COL_X);      /* never under the text column */

    /* the status line: the painter uses the tight pen and closes the glyph gap
     * to 0 and the word gap to 4 when two words must fit.  "Connected Err" is
     * then 75 px and with the 4 + 8 px lock 87 px, both inside the 90 px column.
     * "Connecting Err" is 80 px without the lock (92 with it), so the words fit
     * whole and only the lock yields.  That is why the error word is "Err":
     * "Connected Error" needed 87 px with no room for the lock at all, and
     * "Connecting Error" 92 px, which cut the link word. */
    T_CHECK(pen_px("Connected", 0) + 4 + pen_px("Err", 0) <= LCD_COL_W);
    T_CHECK(pen_px("Connected", 0) + 4 + pen_px("Err", 0) + 4 + 8 <= LCD_COL_W);
    T_CHECK(pen_px("Connecting", 0) + 4 + pen_px("Err", 0) <= LCD_COL_W);
}

static void test_pen(void) {
    /* The natural cell advance is 8/16 px; the tight pen crops to the ink.
     * 'M' inks columns 2..8 and 'W' 1..9, so neither is 8 px of ink. */
    int first, w;
    T_CHECK(pen_span('M', &first, &w) > 0);
    T_CHECK(w < 8);                                  /* side bearings are cropped */
    T_EQ_INT(first, 1);
    T_EQ_INT(pen_span(' ', &first, &w), 0);          /* a blank glyph has no ink */
    T_EQ_INT(pen_span(0xAC00, &first, &w), 14);      /* 가 inks 14 of its 16 columns */

    /* "192.168.1.225" at 8 px per cell is 104 px, which the 90 px column
     * cannot take; on the tight pen it fits with a 1 px gap */
    T_CHECK(font_text_px("192.168.1.225") > LCD_COL_W);
    T_CHECK(pen_px("192.168.1.225", 1) <= LCD_COL_W);
    /* the widest quad is the tight one: it fits only with the gap closed,
     * which is what the painter tries before it would cut a digit */
    T_CHECK(pen_px("255.255.255.255", 1) > LCD_COL_W);
    T_CHECK(pen_px("255.255.255.255", 0) <= LCD_COL_W);
    T_EQ_INT(pen_px("", 1), 0);
    T_EQ_INT(pen_px(" ", 1), 4);

    /* the status words on the tight pen: "Connected Err" leaves room for the
     * lock, which the old "Error" did not */
    T_CHECK(pen_px("Connected", 0) + 4 + pen_px("Err", 0) + 4 + 8 <= LCD_COL_W);

    char out[64];
    /* fits whole */
    T_EQ_INT(pen_fit("192.168.1.225", LCD_COL_W, 1, out, sizeof out), pen_px("192.168.1.225", 1));
    T_EQ_STR(out, "192.168.1.225");

    /* cut: keeps whole glyphs and adds a 16 px ellipsis (+gap) */
    int got = pen_fit("MyHomeNetwork2", 50, 1, out, sizeof out);
    T_CHECK(got <= 50);
    T_CHECK(strstr(out, "\xE2\x80\xA6") != NULL);
    T_CHECK(strncmp(out, "MyHome", 6) == 0);

    /* no room at all */
    T_EQ_INT(pen_fit("MyHome", 0, 1, out, sizeof out), 0);
    T_EQ_STR(out, "");
    T_EQ_INT(pen_fit("MyHome", -3, 1, out, sizeof out), 0);

    /* out_sz never splits a sequence */
    T_EQ_INT(pen_fit("\xEA\xB0\x80\xEB\x82\x98", 90, 1, out, 4), 14);
    T_EQ_STR(out, "\xEA\xB0\x80");
}

int main(void) {
    T_RUN(test_words);
    T_RUN(test_words_locked_only);
    T_RUN(test_diff);
    T_RUN(test_layout);
    T_RUN(test_bars);
    T_RUN(test_qr_scale);
    T_RUN(test_error_hold);
    T_RUN(test_advance);
    T_RUN(test_ellipsize_px);
    T_RUN(test_geometry_fits);
    T_RUN(test_pen);
    T_DONE();
}
