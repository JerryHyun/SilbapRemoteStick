/* BOOT button (GPIO0). Polls, debounces and delegates every decision to the pure button_policy module; the long-press
 * actions are the only things that touch settings.
 *
 * State machine: idle until the debounced press begins (bp_reset, duration clock starts). While held, bp_on_hold() is
 * called each poll and its action runs at most once per press: 5 s clears the web password (solid yellow for a second,
 * then the blink resumes), 15 s factory-resets and restarts. From 1 s the LED blinks yellow at 2 Hz so the user can see
 * the hold has been recognised. On release the policy decides with the OTA arm window as an input: a short press
 * confirms a pending update (BP_CONFIRM_OTA) while ota_armed(), otherwise it requests a refresh, and any press that
 * ends without a restart hands the LED back with refresh_led_reset(). */
#include "button.h"
#include <string.h>
#include "board.h"
#include "button_policy.h"
#include "led.h"
#include "ota.h"
#include "refresh.h"
#include "sdlog.h"
#include "settings.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLL_MS         10
#define DEBOUNCE_MS     20
#define BLINK_FROM_MS   BP_REFRESH_MAX_MS   /* the policy's "long press" line: the blink starts where refresh ends */
#define BLINK_HALF_MS   250                 /* two phases of 250 ms: 2 Hz */
#define SOLID_MS        1000                /* solid yellow after the password clear, then blink again */

static const char *TAG = "button";

static settings_t *s_set;
static bp_state_t s_bp;
static int s_blink_phase = -1;      /* 0 = lit, 1 = dark; only repaints when the phase changes */

/* 1 s onwards: 250 ms on, 250 ms off. */
static void blink_yellow(unsigned held_ms) {
    int phase = (int)((held_ms / BLINK_HALF_MS) & 1u);
    if (phase == s_blink_phase) return;
    s_blink_phase = phase;
    led_set(phase ? 0 : 255, phase ? 0 : 160, 0);
}

/* Clears the saved web password and shows solid yellow for a second. Never log a secret; there is none left. */
static void clear_password(void) {
    settings_t t = *s_set;
    if (!settings_set_password(&t, "")) {        /* "" always succeeds; kept for the API's contract */
        ESP_LOGW(TAG, "clearing the web password failed");
        memset(&t, 0, sizeof t);
        return;
    }
    esp_err_t e = settings_save(&t);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "saving the cleared password failed: %s", esp_err_to_name(e));
        memset(&t, 0, sizeof t);
        return;
    }
    /* web_decide() reads these fields without a lock: clearing pw_set first is the safe direction (fail closed). */
    s_set->pw_set = false;
    memset(s_set->pw_salt, 0, sizeof s_set->pw_salt);
    memset(s_set->pw_hash, 0, sizeof s_set->pw_hash);
    memset(&t, 0, sizeof t);
    sdlog_event("button.log", 0x61, "BOOT 5 s: web password cleared");
    led_set(255, 160, 0);
    vTaskDelay(pdMS_TO_TICKS(SOLID_MS));
}

/* 15 s: wipe the namespace, drop the clock floor too, log, flash red and restart. */
static void factory_reset(void) {
    settings_factory_reset();
    settings_save_last_time(0);       /* settings_factory_reset keeps last_time; 0 reads back as "no floor" */
    sdlog_event("button.log", 0x62, "BOOT 15 s: factory reset");
    led_set(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void button_task(void *arg) {
    (void)arg;
    bool pressed = false;      /* debounced state */
    bool raw_prev = false;
    int stable = 0;
    int64_t t_down = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bool raw = gpio_get_level(BOARD_BTN) == 0;
        if (raw == raw_prev) { if (stable < 1000) stable++; } else { stable = 0; raw_prev = raw; }
        if (raw != pressed && stable * POLL_MS >= DEBOUNCE_MS) {
            pressed = raw;
            int64_t now = esp_timer_get_time() / 1000;
            if (pressed) {
                t_down = now;
                bp_reset(&s_bp);
                s_blink_phase = -1;
            } else {
                switch (bp_on_release_ex((unsigned)(now - t_down), ota_armed())) {
                case BP_REFRESH:     refresh_request(REFRESH_FORCED); break;
                case BP_CONFIRM_OTA: ota_confirm(); break;      /* the arm window's BOOT press */
                case BP_NONE:        break;                     /* a bounce, or a long press the hold loop owned */
                case BP_CLEAR_PW:
                case BP_FACTORY_RESET: break;                   /* never reported on release */
                }
                refresh_led_reset();       /* hand the LED back, whatever the press did */
            }
            continue;
        }
        if (!pressed) continue;
        unsigned held = (unsigned)((esp_timer_get_time() / 1000) - t_down);
        switch (bp_on_hold(&s_bp, held)) {
        case BP_CLEAR_PW: clear_password(); break;
        case BP_FACTORY_RESET: factory_reset(); break;   /* never returns */
        case BP_NONE:
        case BP_REFRESH:                                 /* neither is reported by bp_on_hold */
        case BP_CONFIRM_OTA:
            if (held >= BLINK_FROM_MS) blink_yellow(held);
            break;
        }
    }
}

void button_start(settings_t *s) {
    s_set = s;
    gpio_config_t c = { .pin_bit_mask = BIT64(BOARD_BTN), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&c);
    xTaskCreate(button_task, "button", 4096, NULL, 3, NULL);   /* settings_t copies + NVS calls need more than the old 2048 */
}
