/*
 * lcd.h - ST7735 160x80 status panel: init, clipped fill and row-wise blit.
 *
 * The panel is driven in small windows; there is no full-frame buffer (the
 * board has no PSRAM).  This header stays free of ESP-IDF includes so the font
 * table and the pure modules (font lookup, view) can be compiled on the host.
 */
#pragma once

#include <stdint.h>

/* RGB565 */
#define LCD_COLOR_BLACK   0x0000
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_CYAN    0x07FF
#define LCD_COLOR_MAGENTA 0xF81F
#define LCD_COLOR_GRAY    0x7BEF
#define LCD_COLOR_ORANGE  0xFD20

/* Configures GPIO and SPI2, creates the panel, clears it to black and turns the
 * backlight on.  Idempotent: a second call returns immediately. */
void lcd_init(void);

/* Fills a rectangle, clipped to the panel; a rectangle entirely outside is
 * ignored silently. */
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Copies a w x h row-major RGB565 image to (x, y), clipped to the panel.  Rows
 * that fall outside are skipped; a clipped row is sent column by column.  The
 * pixels are transmitted before the call returns, so the caller may reuse px
 * immediately afterwards. */
void lcd_blit(int x, int y, int w, int h, const uint16_t *px);

int lcd_width(void);
int lcd_height(void);

/* ---- Generated 16x16 font table (main/font_data.c) ---- */
/* One run of codepoints inside font_glyphs, in glyph units. */
typedef struct {
    uint32_t first;   /* first codepoint of the run */
    uint32_t last;    /* last codepoint, inclusive */
    uint32_t offset;  /* index of the run's first glyph, in glyphs (16 rows each) */
} font_range_t;

extern const uint16_t font_glyphs[];      /* 16 rows per glyph, bit 15 = leftmost */
extern const uint32_t font_first_cp;      /* first codepoint in the table */
extern const uint32_t font_last_cp;       /* last codepoint in the table */
extern const font_range_t font_ranges[];  /* sorted; font_first_cp..font_last_cp */
extern const unsigned font_range_count;
