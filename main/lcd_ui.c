/*
 * lcd_ui.c - the display task: sample the system, diff it, paint the regions.
 *
 * Nothing else may draw while this task runs: it owns every pixel.  The panel
 * has no frame buffer (no PSRAM), so a region is painted one row at a time
 * from a small stack buffer: a glyph row is at most 16 px.  A glyph is at most
 * 16 blits (blank rows are skipped), a text line is at most 16 per glyph, and
 * there is never a full-screen repaint.
 *
 * The task never takes sdcard_lock and never touches the card.  It copies the
 * net state, refresh_status() and the settings, diffs the result against what
 * is on the glass, and paints only the regions that changed.
 */
#include "lcd_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "font_lookup.h"
#include "lcd.h"
#include "netif.h"
#include "refresh.h"
#include "settings.h"

/* Light ink on the panel's own background (lcd_init clears it to black). */
#define INK LCD_COLOR_WHITE
#define BG  LCD_COLOR_BLACK

#define TICK_MS      250     /* the sampling period when nothing wakes us */
#define SETTINGS_MS  1000    /* how often the settings are re-read: a new password shows within a second */

/* Signal bars: 3 px wide, 2 px apart, right-aligned at the column's edge, the
 * tallest 13 px above the SSID row's baseline. */
#define BAR_W   3
#define BAR_GAP 2
#define BAR_N   4
#define BARS_W  (BAR_N * BAR_W + (BAR_N - 1) * BAR_GAP)          /* 18 */
#define BARS_X  (LCD_COL_X + LCD_COL_W - BARS_W)                 /* 138 */
#define BARS_BASELINE (LCD_ROW_SSID_Y + LCD_ROW_H)               /* 44 */

/* The pen leaves this many pixels between glyphs.  It closes to 0 only when a
 * line would otherwise not fit (see paint_status). */
#define GAP_PX   1

#define WORD_GAP 8           /* between the status words */
#define MIN_WORD_GAP 4       /* what it shrinks to before a word is cut */
#define LOCK_GAP 4           /* between the last word and the padlock */
#define LOCK_W   8
#define LOCK_H   12

/* What a blank glyph advances on the tight pen (lcd_view.c). */
#define PEN_SPACE_PX 4

/* A padlock that reads at 8x12: a 4-row shackle over an 8-row body with a 2 px
 * keyhole (bit 7 = leftmost pixel).  Drawn, never font data: U+1F512 is outside
 * the BMP and would need a 4-byte UTF-8 path and a 32x32 source. */
static const uint8_t lock_8x12[LOCK_H] = {
    0x3C,   /* ..####.. */
    0x42,   /* .#....#. */
    0x42,   /* .#....#. */
    0x42,   /* .#....#. */
    0xFF,   /* ######## */
    0x81,   /* #......# */
    0x81,   /* #......# */
    0x99,   /* #..##..# */
    0x99,   /* #..##..# */
    0x81,   /* #......# */
    0x81,   /* #......# */
    0xFF,   /* ######## */
};

/* The view last painted.  Only the task touches this. */
static lcd_shown_t s_shown;

/* A one-shot view pushed by another task, guarded by the critical section. */
static portMUX_TYPE s_pub_mux = portMUX_INITIALIZER_UNLOCKED;
static lcd_view_t s_pub;
static bool s_has_pub;

static TaskHandle_t s_task;
static bool s_started;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = src ? strnlen(src, cap - 1) : 0;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- Drawing: rows only, never a frame buffer ----
 *
 * The glyphs are native 16 px (the layout was built for them; nothing is
 * scaled), placed by the tight pen in lcd_view.c so the address and the status
 * words fit the 152 px column.
 */

static void clear_row(int y) {
    lcd_fill_rect(LCD_COL_X, y, LCD_COL_W, LCD_ROW_H, BG);
}

/* Draws s at (x, y) on the tight pen; returns the pen's end. */
static int pen_draw(int x, int y, const char *s, int gap) {
    uint16_t line[16];
    int first, w;
    const char *p = s;
    while (*p) {
        uint32_t cp = font_utf8_next(&p);
        const uint16_t *rows = font_rows(cp);
        w = pen_span(cp, &first, NULL);
        if (!w) {
            x += PEN_SPACE_PX;
            continue;
        }
        for (int r = 0; r < 16; r++) {
            uint16_t bits = rows[r];
            if (bits == 0) continue;                 /* the row was cleared already */
            for (int c = 0; c < w; c++) {
                line[c] = (bits & (uint16_t)(0x8000u >> (first + c))) ? INK : BG;
            }
            lcd_blit(x, y + r, w, 1, line);
        }
        x += w + gap;
    }
    return x - gap;
}

/* Fits, then draws one row from x = LCD_COL_X; returns the pen's end. */
static int draw_row(int y, const char *s, int max_px, int gap) {
    char out[64];
    int w = pen_fit(s, max_px, gap, out, sizeof out);
    if (w == 0) {
        return LCD_COL_X;
    }
    return pen_draw(LCD_COL_X, y, out, gap);
}

static void draw_lock(int x, int y) {
    uint16_t line[LOCK_W];
    for (int r = 0; r < LOCK_H; r++) {
        uint8_t bits = lock_8x12[r];
        for (int c = 0; c < LOCK_W; c++) {
            line[c] = (bits & (uint8_t)(0x80u >> c)) ? INK : BG;
        }
        lcd_blit(x, y + r, LOCK_W, 1, line);
    }
}

static void draw_bars(int bars) {
    static const int h[BAR_N] = { 4, 7, 10, 13 };
    if (bars < 0) return;                       /* setup mode: no bars at all */
    if (bars > BAR_N) bars = BAR_N;
    for (int i = 0; i < bars; i++) {
        int x = BARS_X + i * (BAR_W + BAR_GAP);
        lcd_fill_rect(x, BARS_BASELINE - h[i], BAR_W, h[i], INK);
    }
}

/* ---- Regions ---- */

static void paint_addr(const lcd_view_t *v) {
    /* One draw, no retry loop: the widest thing this row can hold is
     * "http://255.255.255.255" at 135 px on the tight pen with the 1 px glyph
     * gap open (a typical "http://192.168.1.225" is 118 px), and the column is
     * 152 px.  There is no gap value to search for -- even the full-width IPv4
     * fits with room to spare, so a second pass could never be needed.  A
     * truncated address is still worse than none, so draw_row's ellipsis is a
     * guard against something that is not IPv4, not a path we take. */
    clear_row(LCD_ROW_ADDR_Y);
    char line[40];
    lcd_view_addr_line(v->ip, line, sizeof line);
    if (line[0]) draw_row(LCD_ROW_ADDR_Y, line, LCD_COL_W, GAP_PX);
}

static void paint_ssid(const lcd_view_t *v) {
    /* The text stops 4 px before the bars, so a long SSID never runs under
     * them; in setup mode there are no bars and it may use the whole column. */
    int room = (v->bars >= 0) ? (BARS_X - 4 - LCD_COL_X) : LCD_COL_W;
    clear_row(LCD_ROW_SSID_Y);
    if (v->ssid[0]) draw_row(LCD_ROW_SSID_Y, v->ssid, room, GAP_PX);
    draw_bars(v->bars);
}

static void paint_status(const lcd_view_t *v) {
    const char *w[2];
    int n = lcd_view_words(v, w, 2);
    const int y = LCD_ROW_STATUS_Y;
    const int end = LCD_COL_X + LCD_COL_W;

    clear_row(y);

    /* The error word is "Err" (see lcd_view_words), so two words fit without
     * cutting anything: on the tight pen with the 1 px glyph gap "Connected Err"
     * is 89 px and "Connecting Err" 95 px, both far inside the 152 px column, so
     * the shrink loop below never runs for a status line.  It stays because it
     * is the fit contract, and "Err" is the word that is never cut: the link
     * word takes what is left after it. */
    int gap = GAP_PX, word = WORD_GAP;
    if (n == 2) {
        while (pen_px(w[0], gap) + word + pen_px(w[1], gap) > LCD_COL_W) {
            if (gap > 0) gap--;
            else if (word > MIN_WORD_GAP) word = MIN_WORD_GAP;
            else break;
        }
    }

    char out[40];
    int x = LCD_COL_X;
    for (int i = 0; i < n; i++) {
        int room = end - x;
        if (i + 1 < n) {
            room -= word + pen_px(w[i + 1], gap);
            int got = pen_fit(w[i], room < 0 ? 0 : room, gap, out, sizeof out);
            if (got) x = pen_draw(x, y, out, gap);
            x += word;
        } else {
            int got = pen_fit(w[i], room, gap, out, sizeof out);
            if (got) x = pen_draw(x, y, out, gap);
        }
    }

    /* The padlock takes its 4 + 8 px only when the words leave room.  At this
     * width they always do: "Connected Err" ends at 89 px and "Connecting Err"
     * at 95, and the lock needs 12 more of the 152.  The check stays because a
     * cut word is worse than a missing lock. */
    if (v->locked && x + LOCK_GAP + LOCK_W <= end) {
        draw_lock(x + LOCK_GAP, y + (LCD_ROW_H - LOCK_H) / 2);
    }
}

/* ---- The task ---- */

typedef struct {
    settings_t s;
    uint32_t next_settings_ms;
    bool missing;              /* refresh_status().sd_ok has been false */
    uint32_t missing_since_ms;
} lcd_state_t;

/* Setup mode shows the setup AP's own SSID.  netif builds it from the MAC and
 * hands it to esp_wifi; there is no accessor, so read the live config.  The
 * field is not NUL-terminated when ssid_len is set, hence the explicit length.
 * Leaves out untouched when the query fails. */
static void setup_ssid(char *out, size_t cap) {
    wifi_config_t c;
    if (!cap || esp_wifi_get_config(WIFI_IF_AP, &c) != ESP_OK || !c.ap.ssid[0]) {
        return;
    }
    size_t n = c.ap.ssid_len ? c.ap.ssid_len : strnlen((const char *)c.ap.ssid, 32);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, c.ap.ssid, n);
    out[n] = '\0';
}

static void sample(lcd_view_t *want, lcd_state_t *st) {
    uint32_t now = now_ms();

    if ((int32_t)(now - st->next_settings_ms) >= 0) {
        settings_load(&st->s);           /* picks up a password or a new SSID */
        st->next_settings_ms = now + SETTINGS_MS;
    }

    refresh_status_t rs;
    refresh_status(&rs);
    /* Before the watcher publishes its first status the struct is zeroed; read
     * that as "card present" so boot never shows a false Error. */
    bool sd_ok = !rs.started || rs.sd_ok;
    if (sd_ok) {
        st->missing = false;
        st->missing_since_ms = 0;
    } else if (!st->missing) {
        st->missing = true;
        st->missing_since_ms = now;
    }
    uint32_t missing_ms = st->missing ? (now - st->missing_since_ms) : 0;

    net_state_t ns = net_state();
    bool connected = (ns == NET_ONLINE || ns == NET_SETUP);
    bool connecting = (ns == NET_CONNECTING);

    memset(want, 0, sizeof *want);
    net_ip(want->ip, sizeof want->ip);
    if (ns == NET_SETUP) {
        setup_ssid(want->ssid, sizeof want->ssid);
    }
    if (!want->ssid[0]) {
        copy_str(want->ssid, sizeof want->ssid, st->s.wifi_ssid);
    }
    /* Setup mode: there is no signal to report, so no bars are drawn. The AP password is shown instead of the
     * link word (the help text tells the user it is on the screen); it comes from the same settings copy that
     * was just re-read. Empty outside setup mode, so lcd_view_words keeps its normal contract. */
    want->bars = (ns == NET_SETUP) ? -1 : lcd_view_bars_from_rssi(net_rssi(), connected);
    if (ns == NET_SETUP) {
        copy_str(want->ap_pw, sizeof want->ap_pw, st->s.ap_pw);
    }
    want->connected = connected;
    want->connecting = connecting;
    want->error = lcd_view_should_error(sd_ok, missing_ms);
    /* No padlock in setup mode: there is no web password yet (first boot) and the status row belongs to the AP
     * password. */
    want->locked = st->s.pw_set && ns != NET_SETUP;
}

static bool take_published(lcd_view_t *out) {
    bool have;
    portENTER_CRITICAL(&s_pub_mux);
    have = s_has_pub;
    if (have) {
        *out = s_pub;
        s_has_pub = false;
    }
    portEXIT_CRITICAL(&s_pub_mux);
    return have;
}

static void paint(unsigned r, const lcd_view_t *want) {
    if (r & LCD_R_IP) paint_addr(want);
    if (r & (LCD_R_SSID | LCD_R_BARS)) paint_ssid(want);
    if (r & LCD_R_STATUS) paint_status(want);
    s_shown.v = *want;
}

static void lcd_ui_task(void *arg) {
    (void)arg;
    lcd_state_t st;
    memset(&st, 0, sizeof st);
    settings_load(&st.s);
    st.next_settings_ms = now_ms() + SETTINGS_MS;
    memset(&s_shown, 0, sizeof s_shown);

    /* The panel is black at boot, and an all-false view (no address, NET_OFF)
     * diffs to zero against the zeroed s_shown, so the first pass would draw
     * nothing and the status line would stay blank instead of reading
     * "Connecting".  Paint every region once. */
    bool first = true;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));

        lcd_view_t want;
        if (!take_published(&want)) {
            sample(&want, &st);
        }

        unsigned r = lcd_view_diff(&s_shown, &want);
        if (first) {
            r = LCD_R_IP | LCD_R_SSID | LCD_R_BARS | LCD_R_STATUS;
            first = false;
        }
        if (r) paint(r, &want);
    }
}

void lcd_ui_start(void) {
    if (s_started) return;
    if (xTaskCreatePinnedToCore(lcd_ui_task, "lcd_ui", 4096, NULL, 3, &s_task, 0) != pdPASS) {
        ESP_LOGE("lcd_ui", "task creation failed");
        return;
    }
    s_started = true;
}

void lcd_ui_publish(const lcd_view_t *v) {
    if (!v) return;
    portENTER_CRITICAL(&s_pub_mux);
    s_pub = *v;
    s_has_pub = true;
    portEXIT_CRITICAL(&s_pub_mux);
    if (s_task) xTaskNotifyGive(s_task);
}
