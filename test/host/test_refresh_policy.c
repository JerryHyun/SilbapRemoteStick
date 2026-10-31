#include "t.h"
#include "refresh_policy.h"
static void test_debounce(void) {
    T_EQ_INT(rp_change(1000, 1000, 0, false, true), RP_WAIT);
    T_EQ_INT(rp_change(2499, 1000, 0, false, true), RP_WAIT);
    T_EQ_INT(rp_change(2500, 1000, 0, false, true), RP_RUN);
    /* a new request restarts the debounce */
    T_EQ_INT(rp_change(2500, 2400, 0, false, true), RP_WAIT);
    T_EQ_INT(rp_change(3900, 2400, 0, false, true), RP_RUN);
    T_EQ_INT(rp_change(2000, 1000, 0, true, true), RP_WAIT);   /* forced is debounced too */
}
static void test_idle_wait(void) {
    T_EQ_INT(rp_change(10000, 1000, 9000, false, true), RP_WAIT);    /* read 1 s ago */
    T_EQ_INT(rp_change(11999, 1000, 9000, false, true), RP_WAIT);
    T_EQ_INT(rp_change(12000, 1000, 9000, false, true), RP_RUN);     /* 3 s idle */
    T_EQ_INT(rp_change(20000, 1000, 0, false, true), RP_RUN);        /* never read */
}
static void test_forced_bypass(void) {
    /* a forced request is debounced like any other, then runs at once on a bus that never read */
    T_EQ_INT(rp_change(2000, 1000, 0, true, true), RP_WAIT);
    T_EQ_INT(rp_change(10000, 1000, 0, true, true), RP_RUN);
    T_EQ_INT(rp_change(10000, 1000, 0, true, false), RP_RUN);   /* auto_refresh is about automatic rebuilds */
}
static void test_forced_courtesy_wait(void) {
    /* a forced request prefers an idle bus ... */
    T_EQ_INT(rp_change(10000, 8000, 9500, true, true), RP_WAIT);     /* request 2 s old, read 0.5 s ago */
    T_EQ_INT(rp_change(10000, 100, 9500, true, true), RP_WAIT);      /* request 9.9 s old, read 0.5 s ago */
    T_EQ_INT(rp_change(10000, 8000, 9500, true, false), RP_WAIT);    /* forced ignores auto_refresh, not the bus */
    /* ... but it cannot starve: the cap since the request forces the rebuild */
    T_EQ_INT(rp_change(10000, 0, 9500, true, true), RP_RUN);         /* request 10 s old: cap reached */
    T_EQ_INT(rp_change(10000, 0, 9500, true, false), RP_RUN);
    T_EQ_INT(rp_change(11000, 1000, 9500, true, true), RP_RUN);      /* request 10 s old, reads continue */
    /* an idle bus still runs well before the cap */
    T_EQ_INT(rp_change(10000, 8000, 6000, true, true), RP_RUN);      /* read 4 s ago */
    T_EQ_INT(rp_change(10000, 8000, 7000, true, true), RP_RUN);      /* read exactly 3 s ago */
    /* the debounce still comes first: a fresh forced request does not run on an idle bus either */
    T_EQ_INT(rp_change(10500, 10000, 0, true, true), RP_WAIT);
}
static void test_forced_future_stamps(void) {
    /* a read stamp just after now must look fresh (age 0), not 49 days old */
    T_EQ_INT(rp_change(10000, 1000, 10002, true, true), RP_WAIT);
    /* a request stamp just after now must look 0 old, so the cap cannot fire early */
    T_EQ_INT(rp_change(20004, 10005, 20000, true, true), RP_WAIT);   /* 9999 ms since the request */
    T_EQ_INT(rp_change(20005, 10005, 20000, true, true), RP_RUN);    /* 10000 ms: cap reached */
}
static void test_auto_refresh_off(void) {
    T_EQ_INT(rp_change(100000, 1000, 0, false, false), RP_WAIT);
    T_EQ_INT(rp_change(100000, 1000, 5, false, false), RP_WAIT);
}
static void test_boot_verify(void) {
    T_EQ_INT(rp_boot_verify(4999, 0, 0), RP_WAIT);                   /* too early */
    T_EQ_INT(rp_boot_verify(5000, 0, 0), RP_RUN);                    /* idle */
    T_EQ_INT(rp_boot_verify(6000, 0, 5900), RP_WAIT);                /* reads continue */
    T_EQ_INT(rp_boot_verify(8900, 0, 5900), RP_RUN);                 /* idle 3 s */
    T_EQ_INT(rp_boot_verify(14999, 0, 14990), RP_WAIT);
    T_EQ_INT(rp_boot_verify(15000, 0, 14999), RP_RUN);               /* 15 s cap, reads or not */
    T_EQ_INT(rp_boot_verify(130000, 100000, 129999), RP_RUN); /* relative to boot */
    T_EQ_INT(rp_boot_verify(104000, 100000, 0), RP_WAIT);
}
static void test_future_stamps(void) {
    /* stamps taken after now was sampled must not look 49 days old */
    T_EQ_INT(rp_change(1000, 1005, 0, false, true), RP_WAIT);
    T_EQ_INT(rp_change(1000, 1005, 0, true, true), RP_WAIT);
    T_EQ_INT(rp_change(10000, 1000, 10003, false, true), RP_WAIT);   /* read just after now */
    T_EQ_INT(rp_boot_verify(6000, 0, 6002), RP_WAIT);
    T_EQ_INT(rp_boot_verify(4000, 4003, 0), RP_WAIT);
    T_EQ_INT(rp_boot_verify(15000, 0, 15002), RP_RUN);               /* cap still applies */
}
static void test_long_uptime(void) {
    const uint32_t day = 86400000u;
    /* ages between 24.9 and 49.7 days are real ages, not "in the future" */
    T_EQ_INT(rp_change(30 * day + 5000, 30 * day, 0, false, true), RP_RUN);       /* request 5 s old */
    T_EQ_INT(rp_change(30 * day + 5000, 30 * day + 4000, 0, false, true), RP_WAIT); /* request 1 s old */
    T_EQ_INT(rp_change(40 * day, 100, 30 * day, false, true), RP_RUN);            /* last read 10 days ago... */
    T_EQ_INT(rp_change(30 * day, 1000, 1, false, true), RP_RUN);                  /* read ~30 days ago: idle */
    T_EQ_INT(rp_boot_verify(30 * day, 0, 0), RP_RUN);                             /* boot age ~30 days */
    T_EQ_INT(rp_boot_verify(30 * day, 0, 30 * day - 100), RP_RUN);                /* past the cap anyway */
    T_EQ_INT(rp_change(30 * day, 30 * day + 500, 0, false, true), RP_WAIT);       /* 500 ms future stamp = 0 */
    T_EQ_INT(rp_change(30 * day, 1000, 30 * day + 500, false, true), RP_WAIT);
}
/* M8: failed builds back off exponentially, 30 s .. 10 min */
static void test_build_backoff(void) {
    T_EQ_INT(rp_backoff_ms(0), 0);
    T_EQ_INT(rp_backoff_ms(1), 30000); T_EQ_INT(rp_backoff_ms(2), 60000); T_EQ_INT(rp_backoff_ms(3), 120000);
    T_EQ_INT(rp_backoff_ms(5), 480000); T_EQ_INT(rp_backoff_ms(6), 600000); T_EQ_INT(rp_backoff_ms(40), 600000);
    T_EQ_INT(rp_backoff_ms(0xFFFFFFFFu), RP_BACKOFF_MAX_MS);
}
/* The USB detach that makes a host re-read the directory must outlast the host's debouncing.
 * robbellie/embroidery-stick found 50 ms too short on real hardware; 500 ms worked. */
static void test_replug_hold(void) {
    T_EQ_INT(rp_replug_hold_ms(0), RP_REPLUG_MIN_MS);          /* never 0: a 0 ms gap is not a disconnect */
    T_EQ_INT(rp_replug_hold_ms(1), RP_REPLUG_MIN_MS);
    T_EQ_INT(rp_replug_hold_ms(499), RP_REPLUG_MIN_MS);
    T_EQ_INT(rp_replug_hold_ms(500), 500);                     /* the value embroidery-stick settled on */
    T_EQ_INT(rp_replug_hold_ms(1000), 1000);
    T_EQ_INT(rp_replug_hold_ms(2000), RP_REPLUG_MAX_MS);
    T_EQ_INT(rp_replug_hold_ms(60000), RP_REPLUG_MAX_MS);      /* a silly request cannot hide the disk for a minute */
    T_CHECK(RP_REPLUG_HOLD_MS >= RP_REPLUG_MIN_MS);
    T_CHECK(RP_REPLUG_HOLD_MS <= RP_REPLUG_MAX_MS);
}
/* rp_replug: the VBUS cycle uses rp_change's rule (one place for debounce/idle/cap) plus the automatic floor. */
static void test_replug_no_request(void) {
    /* nothing asked: an unchanged disk is never taken off the bus, even when idle and long after boot */
    T_EQ_INT(rp_replug(100000, 0, 0, false, true, 0), RP_WAIT);
    T_EQ_INT(rp_replug(100000, 0, 0, true, true, 0), RP_WAIT);
    T_EQ_INT(rp_replug(100000, 0, 0, true, false, 5000), RP_WAIT);
}
static void test_replug_forced(void) {
    /* the button: debounced, prefers an idle bus, and runs at the cap no matter how the machine reads */
    T_EQ_INT(rp_replug(1000, 1000, 0, true, true, 0), RP_WAIT);            /* fresh request */
    T_EQ_INT(rp_replug(2500, 1000, 0, true, true, 0), RP_RUN);             /* debounce over, bus never read */
    T_EQ_INT(rp_replug(2500, 1000, 2400, true, true, 0), RP_WAIT);         /* read 100 ms ago */
    T_EQ_INT(rp_replug(2500, 1000, 2400, true, false, 0), RP_WAIT);        /* auto_refresh off does not stop the button */
    T_EQ_INT(rp_replug(2500, 1000, 2400, true, false, 2000), RP_WAIT);     /* nor does the automatic floor */
    T_EQ_INT(rp_replug(11000, 1000, 10900, true, true, 0), RP_RUN);        /* cap: reads continue */
    T_EQ_INT(rp_replug(11000, 1000, 10900, true, false, 10950), RP_RUN);   /* forced ignores the floor */
    /* consecutive presses each get their own cycle: a newer stamp is a new request, not a coalesced one */
    T_EQ_INT(rp_replug(4000, 3000, 0, true, true, 2500), RP_WAIT);
    T_EQ_INT(rp_replug(4600, 3000, 0, true, true, 2500), RP_RUN);
}
static void test_replug_auto(void) {
    /* an automatic change: debounce, then a quiet bus, then the floor since the previous cycle */
    T_EQ_INT(rp_replug(1000, 1000, 0, false, true, 0), RP_WAIT);           /* debounce */
    T_EQ_INT(rp_replug(2500, 1000, 0, false, true, 0), RP_RUN);            /* 1.5 s, bus never read */
    T_EQ_INT(rp_replug(2500, 1000, 2400, false, true, 0), RP_WAIT);        /* read 100 ms ago */
    T_EQ_INT(rp_replug(5500, 1000, 2600, false, true, 0), RP_WAIT);        /* read 2.9 s ago: not idle yet */
    T_EQ_INT(rp_replug(5600, 1000, 2600, false, true, 0), RP_RUN);         /* read 3.0 s ago: idle */
    T_EQ_INT(rp_replug(5600, 1000, 2600, false, true, 3000), RP_WAIT);     /* floor: replug 2.6 s ago */
    T_EQ_INT(rp_replug(6000, 1000, 2600, false, true, 3000), RP_RUN);      /* floor over */
    T_EQ_INT(rp_replug(100000, 1000, 0, false, false, 0), RP_WAIT);        /* auto_refresh off */
    /* a continuous reader never lets an unforced cycle run: only the forced path has a cap */
    T_EQ_INT(rp_replug(60000, 1000, 59900, false, true, 0), RP_WAIT);
    T_EQ_INT(rp_replug(60000, 1000, 59900, true, true, 0), RP_RUN);        /* the cap covers the button */
}
static void test_replug_future_stamps(void) {
    /* stamps taken after now must look fresh, not 49 days old (since() handles it; rp_replug must not bypass it) */
    T_EQ_INT(rp_replug(1000, 1005, 0, false, true, 0), RP_WAIT);
    T_EQ_INT(rp_replug(10000, 1000, 10003, false, true, 0), RP_WAIT);
    T_EQ_INT(rp_replug(10000, 1000, 10003, true, true, 0), RP_WAIT);       /* not the cap either */
    T_EQ_INT(rp_replug(10000, 1000, 0, false, true, 10002), RP_WAIT);      /* replug just now: floor */
}
int main(void) {
    T_RUN(test_debounce); T_RUN(test_idle_wait); T_RUN(test_forced_bypass); T_RUN(test_forced_courtesy_wait);
    T_RUN(test_forced_future_stamps);
    T_RUN(test_auto_refresh_off); T_RUN(test_boot_verify); T_RUN(test_future_stamps); T_RUN(test_long_uptime); T_RUN(test_build_backoff);
    T_RUN(test_replug_hold);
    T_RUN(test_replug_no_request); T_RUN(test_replug_forced); T_RUN(test_replug_auto); T_RUN(test_replug_future_stamps);
    T_DONE();
}
