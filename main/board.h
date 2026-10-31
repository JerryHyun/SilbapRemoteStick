#pragma once
#define BOARD_SD_CLK 12
#define BOARD_SD_CMD 16
#define BOARD_SD_D0  14
#define BOARD_SD_D1  17
#define BOARD_SD_D2  21
#define BOARD_SD_D3  18
#define BOARD_LED_DI 40
#define BOARD_LED_CLK 39
#define BOARD_BTN    0
#define SD_MOUNT     "/sd"
#define SD_SECTOR_SIZE 512u      /* the SD card's logical sector; the partition's too */
#define SILBAP_DIR   SD_MOUNT "/.silbap"

/* ---- ST7735 LCD (SPI2) ---- */
#define BOARD_LCD_MOSI 3
#define BOARD_LCD_CLK  5
#define BOARD_LCD_CS   4
#define BOARD_LCD_DC   2
#define BOARD_LCD_RST  1
#define BOARD_LCD_BL   38
#define LCD_W          160
#define LCD_H          80