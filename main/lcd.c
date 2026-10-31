/*
 * lcd.c - ST7735 160x80 display via the esp_lcd panel driver
 *
 * Uses the LilyGO ST7735 panel driver (esp_lcd_st7735.c, vendored from the
 * T-Dongle-S3 factory firmware in MainasuK/RemoteUSB) with the factory's exact
 * panel configuration: 160x80, BGR, gap (1,26), swap_xy, mirror_y, inverted.
 *
 * The board has no PSRAM, so there is no frame buffer: every write is a single
 * panel row taken from one 320-byte DMA-capable row buffer and pushed with
 * esp_lcd_panel_draw_bitmap().  A binary semaphore set by the panel IO's
 * on_color_trans_done callback makes each call synchronous, so a caller may
 * free or overwrite its pixels as soon as the call returns.
 */
#include "lcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board.h"
#include "esp_lcd_st7735.h"

static const char *TAG = "lcd";

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_PCLK_HZ    (40 * 1000 * 1000)  /* factory speed */
#define LCD_ROW_PX     LCD_W               /* one panel row: 320 bytes */

static bool s_tried;                       /* lcd_init was called; never retried */
static bool s_inited;                      /* panel is up and can be drawn to */
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;           /* given by on_color_trans_done */
static SemaphoreHandle_t s_mux;            /* one drawing task at a time */
static uint16_t *s_row;                    /* LCD_ROW_PX pixels, DMA-capable */

/* ISR context. */
static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx) {
    (void)io; (void)edata; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

/* Drops a completion left over from a previous call that timed out. */
static void clear_done(void) {
    while (xSemaphoreTake(s_done, 0) == pdTRUE) { }
}

/* Waits for the last queued colour transfer; the caller holds s_mux. */
static void wait_idle(void) {
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "colour transfer did not complete in 1 s");
    }
}

/* Pushes one clipped row (<= LCD_ROW_PX pixels); the caller holds s_mux. */
static void put_row(int x, int y, int w, const uint16_t *px) {
    if (w > LCD_ROW_PX) w = LCD_ROW_PX;
    memcpy(s_row, px, (size_t)w * sizeof(uint16_t));
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + 1, s_row);
    if (e != ESP_OK) ESP_LOGE(TAG, "draw_bitmap(%d,%d,%d) failed: %s", x, y, w, esp_err_to_name(e));
    wait_idle();
}

/* Clips a rectangle to the panel; false when nothing is left. */
static bool clip(int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) return false;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= LCD_W || *y >= LCD_H) return false;
    if (*x + *w > LCD_W) *w = LCD_W - *x;
    if (*y + *h > LCD_H) *h = LCD_H - *y;
    return *w > 0 && *h > 0;
}

void lcd_init(void) {
    if (s_tried) return;
    s_tried = true;

    s_mux = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_row = heap_caps_malloc((size_t)LCD_ROW_PX * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_mux || !s_done || !s_row) {
        ESP_LOGE(TAG, "no memory for the LCD buffers");
        return;
    }

    /* CS, DC and RST belong to the panel IO / driver; only the backlight is ours.
     * The T-Dongle-S3 drives it active low (LOW = on). */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BOARD_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_LCD_BL, 1);   /* off while the panel initialises */

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_ROW_PX * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 1, 26));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));

    s_inited = true;
    lcd_fill_rect(0, 0, LCD_W, LCD_H, LCD_COLOR_BLACK);
    gpio_set_level(BOARD_LCD_BL, 0);   /* on */
    ESP_LOGI(TAG, "ST7735 ready, %dx%d", LCD_W, LCD_H);
}

int lcd_width(void) { return LCD_W; }
int lcd_height(void) { return LCD_H; }

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
    int cw = w, ch = h;
    if (!s_inited || !clip(&x, &y, &cw, &ch)) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    clear_done();
    for (int i = 0; i < cw; i++) s_row[i] = color;
    for (int r = 0; r < ch; r++) {
        esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + cw, y + r + 1, s_row);
        if (e != ESP_OK) ESP_LOGE(TAG, "fill failed: %s", esp_err_to_name(e));
        wait_idle();
    }
    xSemaphoreGive(s_mux);
}

void lcd_blit(int x, int y, int w, int h, const uint16_t *px) {
    int cx = x, cy = y, cw = w, ch = h;
    if (!px || !s_inited || !clip(&cx, &cy, &cw, &ch)) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    clear_done();
    for (int r = 0; r < ch; r++) {
        int src_row = (cy - y) + r;                 /* row in the caller's image */
        const uint16_t *src = px + (size_t)src_row * (size_t)w + (size_t)(cx - x);
        put_row(cx, cy + r, cw, src);
    }
    xSemaphoreGive(s_mux);
}
