#include "t.h"
#include "button_policy.h"

/* A short press refreshes; a bounce does not. */
static void test_release_short(void) {
    T_EQ_INT(bp_on_release(300), BP_REFRESH);
    T_EQ_INT(bp_on_release(BP_DEBOUNCE_MS), BP_REFRESH);              /* exactly at the debounce floor */
    T_EQ_INT(bp_on_release(BP_REFRESH_MAX_MS - 1), BP_REFRESH);       /* just under 1 s */
}

/* A long press below the hold thresholds does nothing on release: bp_on_hold() owns those. */
static void test_release_long(void) {
    T_EQ_INT(bp_on_release(3000), BP_NONE);
    T_EQ_INT(bp_on_release(BP_REFRESH_MAX_MS), BP_NONE);              /* exactly 1 s */
    T_EQ_INT(bp_on_release(BP_CLEAR_PW_MS), BP_NONE);
    T_EQ_INT(bp_on_release(BP_FACTORY_RESET_MS), BP_NONE);
}

/* Bounce and absurd durations are still just a release: no action. */
static void test_release_edges(void) {
    T_EQ_INT(bp_on_release(0), BP_NONE);                              /* no press at all */
    T_EQ_INT(bp_on_release(1), BP_NONE);                              /* contact bounce */
    T_EQ_INT(bp_on_release(BP_DEBOUNCE_MS - 1), BP_NONE);
    T_EQ_INT(bp_on_release(3000), BP_NONE);
    T_EQ_INT(bp_on_release(60000), BP_NONE);                          /* a minute held: the hold loop did it */
    T_EQ_INT(bp_on_release(0xFFFFFFFFu), BP_NONE);                    /* huge: no overflow, still NONE */
}

/* Walk a held button up to 5 s: CLEAR_PW on the crossing poll only. */
static void test_hold_clear_pw(void) {
    bp_state_t st = { false, false };
    T_EQ_INT(bp_on_hold(&st, 0), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 1000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 2000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 3000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 4000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 4999), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 5000), BP_CLEAR_PW);
    T_EQ_INT(bp_on_hold(&st, 5001), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 6000), BP_NONE);
}

/* The same hold continues to 15 s: FACTORY_RESET on the crossing poll only. */
static void test_hold_factory_reset(void) {
    bp_state_t st = { false, false };
    T_EQ_INT(bp_on_hold(&st, 4000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 5000), BP_CLEAR_PW);
    T_EQ_INT(bp_on_hold(&st, 6000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 14000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 14999), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 15000), BP_FACTORY_RESET);
    T_EQ_INT(bp_on_hold(&st, 15001), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 16000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 20000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 0xFFFFFFFFu), BP_NONE);                 /* stays quiet forever after */
}

/* A single poll that jumps past both thresholds reports the highest only, and never the lesser one later. */
static void test_hold_big_jump(void) {
    bp_state_t st = { false, false };
    T_EQ_INT(bp_on_hold(&st, 20000), BP_FACTORY_RESET);
    T_EQ_INT(bp_on_hold(&st, 20001), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 6000), BP_NONE);                        /* 5 s was marked fired too */
    T_EQ_INT(bp_on_hold(&st, 20000), BP_NONE);
}

/* bp_reset starts a new press: both thresholds fire again. */
static void test_reset_rearms(void) {
    bp_state_t st = { false, false };
    T_EQ_INT(bp_on_hold(&st, 5000), BP_CLEAR_PW);
    T_EQ_INT(bp_on_hold(&st, 15000), BP_FACTORY_RESET);
    bp_reset(&st);
    T_EQ_INT(bp_on_hold(&st, 1000), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 5000), BP_CLEAR_PW);
    T_EQ_INT(bp_on_hold(&st, 14999), BP_NONE);
    T_EQ_INT(bp_on_hold(&st, 15000), BP_FACTORY_RESET);
    T_EQ_INT(bp_on_hold(&st, 15001), BP_NONE);
}

/* bp_reset on an already-clean state and on NULL are harmless. */
static void test_reset_edges(void) {
    bp_state_t st = { false, true };
    bp_reset(&st);
    T_EQ_INT(bp_on_hold(&st, 5000), BP_CLEAR_PW);
    T_EQ_INT(bp_on_hold(&st, 15000), BP_FACTORY_RESET);
    bp_reset(NULL);
    T_EQ_INT(bp_on_hold(NULL, 5000), BP_NONE);
    T_EQ_INT(bp_on_hold(NULL, 15000), BP_NONE);
}

/* The OTA arm window turns the same short press into a confirmation, and nothing else changes. */
static void test_release_ota_armed(void) {
    T_EQ_INT(bp_on_release_ex(300, true), BP_CONFIRM_OTA);
    T_EQ_INT(bp_on_release_ex(BP_DEBOUNCE_MS, true), BP_CONFIRM_OTA);
    T_EQ_INT(bp_on_release_ex(BP_REFRESH_MAX_MS - 1, true), BP_CONFIRM_OTA);
    T_EQ_INT(bp_on_release_ex(300, false), BP_REFRESH);               /* not armed: the old action */
    T_EQ_INT(bp_on_release_ex(1, true), BP_NONE);                     /* a bounce confirms nothing */
    T_EQ_INT(bp_on_release_ex(0, true), BP_NONE);
    T_EQ_INT(bp_on_release_ex(BP_DEBOUNCE_MS - 1, true), BP_NONE);
    T_EQ_INT(bp_on_release_ex(BP_REFRESH_MAX_MS, true), BP_NONE);     /* a long press: the hold loop owns it */
    T_EQ_INT(bp_on_release_ex(3000, true), BP_NONE);
    T_EQ_INT(bp_on_release_ex(0xFFFFFFFFu, true), BP_NONE);
    T_EQ_INT(bp_on_release(300), bp_on_release_ex(300, false));       /* the wrapper is the armed == false case */
}

int main(void) {
    T_RUN(test_release_short);
    T_RUN(test_release_long);
    T_RUN(test_release_edges);
    T_RUN(test_release_ota_armed);
    T_RUN(test_hold_clear_pw);
    T_RUN(test_hold_factory_reset);
    T_RUN(test_hold_big_jump);
    T_RUN(test_reset_rearms);
    T_RUN(test_reset_edges);
    T_DONE();
}
