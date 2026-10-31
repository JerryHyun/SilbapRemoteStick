#include "netpolicy.h"
np_mode_t np_decide(const np_in_t *in) {
    if (!in->have_creds || in->forced_setup) return NP_SETUP_AP;
    if (!in->connected && in->sta_fail_ms >= NP_SETUP_AFTER_MS) return NP_SETUP_AP;
    return NP_STA_ONLY;
}
uint32_t np_backoff_ms(unsigned attempt) {
    static const uint32_t t[] = { 2000, 5000, 10000 };
    return attempt < 3 ? t[attempt] : 30000;
}
