#pragma once
/* BOOT-button (GPIO0) press durations -> actions. Pure decision logic, host-tested by
 * test/host/test_button_policy.c; main/button.c polls the pin and calls these. No ESP-IDF, no globals, no
 * allocation, no floating point.
 *
 * Rules (thresholds in milliseconds):
 *   - release after [BP_DEBOUNCE_MS, BP_REFRESH_MAX_MS)  -> BP_REFRESH  (a short press)
 *   - release after anything else (a bounce, or a long press whose thresholds the hold loop already handled)
 *                                                       -> BP_NONE
 *   - held to BP_CLEAR_PW_MS                             -> BP_CLEAR_PW, once
 *   - held to BP_FACTORY_RESET_MS                        -> BP_FACTORY_RESET, once
 *
 * Each hold threshold fires exactly on the first poll whose duration reaches it; every other poll returns
 * BP_NONE. A single poll that jumps past both thresholds reports only the highest one crossed
 * (BP_FACTORY_RESET) and marks both as fired, so the caller runs the most drastic action and never sees the
 * lesser one afterwards. The caller passes a non-decreasing duration and calls bp_reset() when the button goes
 * down, so the "fire once" flags start clear for every new press. */
#include <stdbool.h>
#include <stdint.h>

#define BP_DEBOUNCE_MS      50u     /* shorter than this is contact bounce: not a press at all */
#define BP_REFRESH_MAX_MS   1000u   /* a release at or after this is a long press, handled while held */
#define BP_CLEAR_PW_MS      5000u   /* held this long: forget the stored WebDAV password */
#define BP_FACTORY_RESET_MS 15000u  /* held this long: wipe the settings */

typedef enum {
    BP_NONE = 0,        /* nothing to do on this poll */
    BP_REFRESH,         /* short press: force a refresh */
    BP_CLEAR_PW,        /* 5 s hold: clear the saved password */
    BP_FACTORY_RESET,   /* 15 s hold: factory reset */
} bp_action_t;

/* Caller-owned, so the fire-once flags survive across polls. Zero-initialised or bp_reset() before a press. */
typedef struct {
    bool cleared;   /* BP_CLEAR_PW already fired during this press */
    bool reset;     /* BP_FACTORY_RESET already fired during this press */
} bp_state_t;

/* Clears both flags: call when the button goes down (or on init). NULL is ignored. */
void bp_reset(bp_state_t *st);

/* The button was released after a press that lasted ms milliseconds. Stateless: the hold loop owns the long
 * presses, so this only ever reports BP_REFRESH or BP_NONE. */
bp_action_t bp_on_release(unsigned ms);

/* Called repeatedly while the button is held; ms is the duration so far (non-decreasing). Fires each threshold
 * at most once per press through st. NULL is ignored (returns BP_NONE). */
bp_action_t bp_on_hold(bp_state_t *st, unsigned ms);
