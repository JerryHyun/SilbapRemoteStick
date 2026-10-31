#include "t.h"
#include "netpolicy.h"
static void test_fallback_policy(void) {
    np_in_t a = { .have_creds = false };                         T_EQ_INT(np_decide(&a), NP_SETUP_AP);
    np_in_t b = { .have_creds = true, .sta_fail_ms = 19999 };     T_EQ_INT(np_decide(&b), NP_STA_ONLY);
    np_in_t c = { .have_creds = true, .sta_fail_ms = 20000 };     T_EQ_INT(np_decide(&c), NP_SETUP_AP);
    np_in_t d = { .have_creds = true, .connected = true, .sta_fail_ms = 99999 }; T_EQ_INT(np_decide(&d), NP_STA_ONLY);
    np_in_t e = { .have_creds = true, .connected = true, .forced_setup = true }; T_EQ_INT(np_decide(&e), NP_SETUP_AP);
}
static void test_backoff(void) {
    T_EQ_INT(np_backoff_ms(0), 2000); T_EQ_INT(np_backoff_ms(1), 5000);
    T_EQ_INT(np_backoff_ms(2), 10000); T_EQ_INT(np_backoff_ms(3), 30000); T_EQ_INT(np_backoff_ms(100), 30000);
}
int main(void) { T_RUN(test_fallback_policy); T_RUN(test_backoff); T_DONE(); }
