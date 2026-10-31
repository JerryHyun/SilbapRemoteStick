#include "refresh_policy.h"
uint32_t rp_replug_hold_ms(uint32_t requested) {
    if (requested < RP_REPLUG_MIN_MS) return RP_REPLUG_MIN_MS;
    if (requested > RP_REPLUG_MAX_MS) return RP_REPLUG_MAX_MS;
    return requested;
}
/* Milliseconds since t. A stamp up to 1 s ahead of now (taken by another task after now was sampled) counts as 0;
 * any other negative signed delta is a genuine age of 24.9..49.7 days and uses the unsigned difference. */
#define RP_FUTURE_SLACK_MS 1000u
static uint32_t since(uint32_t now, uint32_t t) {
    uint32_t d = now - t;
    return (uint32_t)(0u - d) < RP_FUTURE_SLACK_MS && d != 0 ? 0 : d;
}
static bool idle(uint32_t now, uint32_t last_read) { return last_read == 0 || since(now, last_read) >= RP_IDLE_MS; }
rp_decision_t rp_change(uint32_t now, uint32_t last_request, uint32_t last_read, bool forced, bool auto_refresh) {
    if (since(now, last_request) < RP_DEBOUNCE_MS) return RP_WAIT;
    if (forced) return idle(now, last_read) || since(now, last_request) >= RP_FORCED_MAX_WAIT_MS ? RP_RUN : RP_WAIT;
    if (!auto_refresh) return RP_WAIT;
    return idle(now, last_read) ? RP_RUN : RP_WAIT;
}
rp_decision_t rp_replug(uint32_t now, uint32_t last_request, uint32_t last_read, bool forced, bool auto_refresh,
                        uint32_t last_replug) {
    if (last_request == 0) return RP_WAIT;                       /* nothing asked: never cycle an unchanged disk */
    if (forced) return rp_change(now, last_request, last_read, true, auto_refresh);
    if (last_replug != 0 && since(now, last_replug) < RP_REPLUG_FLOOR_MS) return RP_WAIT;
    return rp_change(now, last_request, last_read, false, auto_refresh);
}
uint32_t rp_backoff_ms(uint32_t fails) {
    if (!fails) return 0;
    uint32_t ms = RP_BACKOFF_MIN_MS;
    while (--fails && ms < RP_BACKOFF_MAX_MS) ms *= 2;
    return ms < RP_BACKOFF_MAX_MS ? ms : RP_BACKOFF_MAX_MS;
}
rp_decision_t rp_boot_verify(uint32_t now, uint32_t boot, uint32_t last_read) {
    uint32_t age = since(now, boot);
    if (age < RP_BOOT_VERIFY_MS) return RP_WAIT;
    if (age >= RP_BOOT_VERIFY_CAP_MS) return RP_RUN;
    return idle(now, last_read) ? RP_RUN : RP_WAIT;
}
