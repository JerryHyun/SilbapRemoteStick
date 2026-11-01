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

/* Setup mode: the AP password must be on the glass or nobody can join the AP.
 * settings.h promises it "stays what is printed/known", so "printed" is this. */
static void test_words_setup_pw(void) {
    lcd_view_t v = view("192.168.4.1", "SilbapStick-A1B2", -1, true, false, false, false);
    strcpy(v.ap_pw, "12345678");
    const char *w[2];
    T_EQ_INT(lcd_view_words(&v, w, 2), 2);
    T_EQ_STR(w[0], "PW:");
    T_EQ_STR(w[1], "12345678");

    /* It wins over the link word in every combination: there is no card work and
     * no user password to report while the setup AP is up. */
    v.error = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 2); T_EQ_STR(w[0], "PW:");
    v.locked = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 2); T_EQ_STR(w[1], "12345678");
    v.connected = false; v.connecting = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 2); T_EQ_STR(w[0], "PW:");

    /* capacity: one slot gets the label only, never a bare password */
    T_EQ_INT(lcd_view_words(&v, w, 1), 1); T_EQ_STR(w[0], "PW:");
    T_EQ_INT(lcd_view_words(&v, w, 0), 0);

    /* clearing the field returns the normal link word */
    v.ap_pw[0] = 0;
    v.error = false; v.locked = false; v.connecting = false; v.connected = true;
    T_EQ_INT(lcd_view_words(&v, w, 2), 1); T_EQ_STR(w[0], "Connected");

    /* the password is never cut: "PW:" + gap + 8 digits fits the column on the tight pen */
    T_CHECK(pen_px("PW:", 0) + 4 + pen_px("12345678", 0) <= LCD_COL_W);

    /* a change of password alone repaints the status line, and so does appearing/disappearing */
    lcd_shown_t shown; memset(&shown, 0, sizeof shown);
    shown.v = view("192.168.4.1", "SilbapStick-A1B2", -1, true, false, false, false);
    strcpy(shown.v.ap_pw, "12345678");
    lcd_view_t want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);
    strcpy(want.ap_pw, "87654321");
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);
    want.ap_pw[0] = 0;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);
}

/* There is no QR any more: four region bits, and an address change repaints the
 * address row only. */
static void test_no_qr_and_addr_diff(void) {
    /* the QR bit is gone: a view cannot ask for one */
    T_EQ_INT(LCD_R_IP | LCD_R_SSID | LCD_R_BARS | LCD_R_STATUS, 15);
    lcd_shown_t shown; memset(&shown, 0, sizeof shown);
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    lcd_view_t want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);
    /* an address change repaints the address row only */
    strcpy(want.ip, "192.168.0.43");
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP);
    /* a blank address repaints the row (it must go empty, not keep the old text) */
    shown.v = want;
    want.ip[0] = '\0';
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP);
}

static void test_pw_row_clears_after_setup(void) {
    /* setup mode: PW: and the digits */
    lcd_view_t v = view("192.168.4.1", "SilbapStick-A1B2", -1, true, false, false, false);
    strcpy(v.ap_pw, "12345678");
    const char *w[2];
    T_EQ_INT(lcd_view_words(&v, w, 2), 2);
    T_EQ_STR(w[0], "PW:"); T_EQ_STR(w[1], "12345678");
    /* leaving setup mode clears the field before the words are chosen: the status line must not
     * keep showing the AP password after a network is joined */
    v.ap_pw[0] = '\0';
    T_EQ_INT(lcd_view_words(&v, w, 2), 1);
    T_EQ_STR(w[0], "Connected");
    /* and the diff repaints the status row on that transition */
    lcd_shown_t shown; memset(&shown, 0, sizeof shown);
    shown.v = view("192.168.4.1", "SilbapStick-A1B2", -1, true, false, false, false);
    strcpy(shown.v.ap_pw, "12345678");
    lcd_view_t want = shown.v; want.ap_pw[0] = '\0';
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);
}

static void test_diff(void) {
    lcd_shown_t shown;
    lcd_view_t want;

    /* identical */
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);

    /* IP change repaints the address row */
    want.ip[0] = '\0';
    strcpy(want.ip, "192.168.0.43");
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP);

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

    /* a blank IP must clear the row, never show a stale address */
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    want = shown.v;
    want.ip[0] = '\0';
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP);

    /* empty -> empty is nothing to do */
    shown.v = view("", "", -1, false, false, false, false);
    want = shown.v;
    T_EQ_INT(lcd_view_diff(&shown, &want), 0);

    /* an address appearing after a blank one draws the row */
    shown.v = view("", "", -1, false, false, false, false);
    want = view("192.168.0.42", "", -1, false, false, false, false);
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_IP);

    /* error joins the status line */
    shown.v = view("192.168.0.42", "MyHome", 4, true, false, false, false);
    want = shown.v;
    want.error = true;
    T_EQ_INT(lcd_view_diff(&shown, &want), LCD_R_STATUS);
}

/* The address row: "http://" once, never doubled, and never a partial URL. */
static void test_addr_line(void) {
    char b[40];
    lcd_view_addr_line("192.168.1.225", b, sizeof b);  T_EQ_STR(b, "http://192.168.1.225");
    lcd_view_addr_line("192.168.4.1", b, sizeof b);    T_EQ_STR(b, "http://192.168.4.1");
    lcd_view_addr_line("", b, sizeof b);               T_EQ_STR(b, "");          /* no stale address */
    /* never a partial URL when the buffer is short */
    lcd_view_addr_line("192.168.1.225", b, 12);        T_EQ_STR(b, "");

    /* degenerate arguments: no out, no size, no address */
    lcd_view_addr_line("192.168.1.225", NULL, sizeof b);
    lcd_view_addr_line("192.168.1.225", b, 0);         T_EQ_STR(b, "");
    lcd_view_addr_line(NULL, b, sizeof b);             T_EQ_STR(b, "");
    /* exactly enough room for the whole URL and its NUL is enough */
    lcd_view_addr_line("192.168.4.1", b, strlen("http://192.168.4.1") + 1);
    T_EQ_STR(b, "http://192.168.4.1");
    /* one byte short is not */
    lcd_view_addr_line("192.168.4.1", b, strlen("http://192.168.4.1"));
    T_EQ_STR(b, "");
}

static void test_layout_new(void) {
    /* three 16 px rows with 8 px gaps, 4 px top margin, inside 160x80 */
    T_CHECK(LCD_ROW_ADDR_Y + LCD_ROW_H <= 80);
    T_CHECK(LCD_ROW_SSID_Y >= LCD_ROW_ADDR_Y + LCD_ROW_H);
    T_CHECK(LCD_ROW_STATUS_Y >= LCD_ROW_SSID_Y + LCD_ROW_H);
    T_CHECK(LCD_ROW_STATUS_Y + LCD_ROW_H <= 80);
    T_CHECK(LCD_COL_X + LCD_COL_W <= 160);
    /* the full-width column: 4 px inset on both sides of the 160 px panel */
    T_EQ_INT(LCD_COL_X, 4);
    T_EQ_INT(LCD_COL_W, 152);
    T_EQ_INT(LCD_COL_X + LCD_COL_W, LCD_PANEL_W - LCD_COL_X);
    T_EQ_INT(LCD_ROW_H, 16);
    /* every row the painter will draw fits, measured on the pen the painter uses */
    char a[40];
    lcd_view_addr_line("192.168.1.225", a, sizeof a);
    T_CHECK(pen_px(a, 1) <= LCD_COL_W);                       /* 118 px */
    T_CHECK(pen_px("http://255.255.255.255", 1) <= LCD_COL_W);/* 135 px, the widest IPv4 */
    T_CHECK(pen_px("http://192.168.4.1", 1) <= LCD_COL_W);    /* 104 px */
    T_CHECK(pen_px("SilbapStick-A1B2", 1) <= LCD_COL_W);      /* 104 px */
    T_CHECK(pen_px("PW:12345678", 1) <= LCD_COL_W);           /*  71 px */
}

static void test_layout(void) {
    /* three rows, 4 px top margin, 8 px row gaps, all inside 160x80 */
    T_CHECK(LCD_COL_X + LCD_COL_W <= LCD_PANEL_W);
    T_CHECK(LCD_ROW_ADDR_Y + LCD_ROW_H <= LCD_PANEL_H);
    T_CHECK(LCD_ROW_SSID_Y + LCD_ROW_H <= LCD_PANEL_H);
    T_CHECK(LCD_ROW_STATUS_Y + LCD_ROW_H <= LCD_PANEL_H);
    T_CHECK(LCD_ROW_ADDR_Y + LCD_ROW_H <= LCD_ROW_SSID_Y);
    T_CHECK(LCD_ROW_SSID_Y + LCD_ROW_H <= LCD_ROW_STATUS_Y);
    T_EQ_INT(LCD_COL_X, 4);
    T_EQ_INT(LCD_ROW_ADDR_Y, 4);
    T_EQ_INT(LCD_ROW_SSID_Y, 28);
    T_EQ_INT(LCD_ROW_STATUS_Y, 52);
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
    /* The status line: the painter uses the tight pen and closes the glyph gap
     * to 0 and the word gap to 4 when two words must fit.  "Connected Err" is
     * then 75 px and with the 4 + 8 px lock 87 px.  "Connecting Err" is 80 px
     * without the lock (92 with it).  All of it fits the 152 px column with the
     * 1 px gap still open, which is what the painter actually draws with.
     * That is why the error word is "Err": "Connected Error" was the version
     * that needed the gap closed to keep the link word whole. */
    T_CHECK(pen_px("Connected", 0) + 4 + pen_px("Err", 0) <= LCD_COL_W);
    T_CHECK(pen_px("Connected", 0) + 4 + pen_px("Err", 0) + 4 + 8 <= LCD_COL_W);
    T_CHECK(pen_px("Connecting", 0) + 4 + pen_px("Err", 0) <= LCD_COL_W);
    /* the widest status line, lock included, still fits the full-width column */
    T_CHECK(pen_px("Connecting", 0) + 4 + pen_px("Err", 0) + 4 + 8 <= LCD_COL_W);
    /* and with the 1 px glyph gap the painter uses by default, no shrinking is needed */
    T_CHECK(pen_px("Connecting", 1) + 8 + pen_px("Err", 1) + 4 + 8 <= LCD_COL_W);
    T_CHECK(pen_px("Connected", 1) + 8 + pen_px("Err", 1) + 4 + 8 <= LCD_COL_W);
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

    /* "192.168.1.225" at 8 px per cell is 104 px; the tight pen is what the
     * painter draws with, and it is narrower than the cells suggest. */
    T_CHECK(pen_px("192.168.1.225", 1) < font_text_px("192.168.1.225"));
    T_CHECK(pen_px("192.168.1.225", 1) <= LCD_COL_W);
    /* the widest quad fits the full-width column with the gap still open */
    T_CHECK(pen_px("255.255.255.255", 1) <= LCD_COL_W);
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
    T_RUN(test_words_setup_pw);
    T_RUN(test_no_qr_and_addr_diff);
    T_RUN(test_pw_row_clears_after_setup);
    T_RUN(test_diff);
    T_RUN(test_addr_line);
    T_RUN(test_layout_new);
    T_RUN(test_layout);
    T_RUN(test_bars);
    T_RUN(test_error_hold);
    T_RUN(test_advance);
    T_RUN(test_ellipsize_px);
    T_RUN(test_geometry_fits);
    T_RUN(test_pen);
    T_DONE();
}
