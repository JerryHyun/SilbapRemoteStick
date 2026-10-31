#include "t.h"
#include "timekeep_core.h"
static void test_monotonic(void) {
    tk_state_t s = { .last_saved = 1000, .floor = 1000, .synced = false };
    T_EQ_INT(tk_effective(&s, 10), 1000);           /* RTC reset to 1970 after power-off */
    T_EQ_INT(tk_effective(&s, 5000), 5000);
}
static void test_save_rate(void) {
    tk_state_t s = { .last_saved = 1000, .floor = 1000, .synced = true };
    T_CHECK(!tk_should_save(&s, 4599)); T_CHECK(tk_should_save(&s, 4600));
    s.synced = false; T_CHECK(!tk_should_save(&s, 99999));
}
static void test_client_hint(void) {
    tk_state_t s = { .floor = 1000, .synced = false };
    T_CHECK(tk_accept_client(&s, 10, 1761800000000LL));
    s.synced = true; T_CHECK(!tk_accept_client(&s, 1761800000, 1761800005000LL));
    T_CHECK(!tk_accept_client(&s, 10, 500000));     /* client clock below floor: ignore */
}
static void test_client_extra(void) {
    tk_state_t s = { .floor = 1000, .synced = true };
    T_CHECK(tk_accept_client(&s, 10, 1761800000000LL));    /* synced but RTC below floor */
    s.synced = false; s.floor = 1761900000;
    T_CHECK(!tk_accept_client(&s, 10, 1761800000000LL));   /* client older than the floor */
    s.floor = 0;
    T_CHECK(!tk_accept_client(&s, 10, 1000000LL));         /* implausible (1970) browser clock */
}
static void test_tz(void) {
    char b[24];
    tk_tz_string(540, b, sizeof b);  T_EQ_STR(b, "UTC-9");
    tk_tz_string(0, b, sizeof b);    T_EQ_STR(b, "UTC+0");
    tk_tz_string(-300, b, sizeof b); T_EQ_STR(b, "UTC+5");
    tk_tz_string(330, b, sizeof b);  T_EQ_STR(b, "UTC-5:30");
    tk_tz_string(-210, b, sizeof b); T_EQ_STR(b, "UTC+3:30");
}
static void test_sntp_authoritative(void) {
    tk_state_t s = { .last_saved = 4070908800LL, .floor = 4070908800LL, .synced = true, .unverified = true };  /* 2099 */
    tk_on_sntp(&s, 1790000000LL);
    T_EQ_INT(tk_effective(&s, 1790000000LL), 1790000000LL);
    T_CHECK(tk_should_save(&s, 1790000000LL));
    T_CHECK(!s.unverified);
}
static void test_browser_not_persisted(void) {
    tk_state_t s = { .floor = 1000, .last_saved = 1000 };
    T_CHECK(tk_accept_client(&s, 10, 1761800000000LL));
    tk_on_client(&s);
    T_CHECK(s.synced && s.floor == 1000);
    T_CHECK(!tk_should_save(&s, 1761800000LL));
    tk_check_uptime(&s, TK_CORROBORATE_UPTIME_S - 1); T_CHECK(!tk_should_save(&s, 1761800000LL));
    tk_check_uptime(&s, TK_CORROBORATE_UPTIME_S);     T_CHECK(tk_should_save(&s, 1761800000LL));
}
int main(void) {
    T_RUN(test_monotonic); T_RUN(test_save_rate); T_RUN(test_client_hint); T_RUN(test_client_extra); T_RUN(test_tz); T_RUN(test_sntp_authoritative); T_RUN(test_browser_not_persisted);
    T_DONE();
}
