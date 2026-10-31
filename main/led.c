/* Adapted from RemoteUSB led.c (MIT, see THIRD_PARTY_NOTICES.md). */
/* Bit-banged APA102. A hardware SPI driver would claim GPIO21 (SD D2). */
#include "led.h"
#include "board.h"
#include "driver/gpio.h"
#define LED_BRIGHTNESS 7 /* APA102 global brightness 0-31 */
static void shift(uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        gpio_set_level(BOARD_LED_DI, (v >> i) & 1);
        gpio_set_level(BOARD_LED_CLK, 1);
        gpio_set_level(BOARD_LED_CLK, 0);
    }
}
void led_init(void) {
    gpio_config_t c = { .pin_bit_mask = BIT64(BOARD_LED_DI) | BIT64(BOARD_LED_CLK), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&c);
    gpio_set_level(BOARD_LED_CLK, 0);
    led_set(0, 0, 0);
}
void led_set(uint8_t r, uint8_t g, uint8_t b) {
    shift(0, 32);
    shift((0xE0u | LED_BRIGHTNESS) << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | r, 32);
    shift(0xFFFFFFFFu, 32);
}
