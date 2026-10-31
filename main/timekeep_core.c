#include "timekeep_core.h"
#include <stdio.h>
#include <stdlib.h>

int64_t tk_effective(const tk_state_t *s, int64_t now_unix) {
    return now_unix < s->floor ? s->floor : now_unix;
}
bool tk_should_save(const tk_state_t *s, int64_t now_unix) {
    return s->synced && !s->unverified && now_unix - s->last_saved >= TK_SAVE_INTERVAL_S;
}
bool tk_accept_client(const tk_state_t *s, int64_t now_unix, int64_t client_ms) {
    int64_t c = client_ms / 1000;
    if (c < s->floor || c < TK_MIN_CLIENT_S) return false;
    return !s->synced || now_unix < s->floor;
}
void tk_tz_string(int tz_min, char *out, size_t cap) {
    int inv = -tz_min, a = abs(inv), h = a / 60, m = a % 60;
    const char *sg = inv < 0 ? "-" : "+";
    if (m) snprintf(out, cap, "UTC%s%d:%02d", sg, h, m);
    else   snprintf(out, cap, "UTC%s%d", sg, h);
}
void tk_on_client(tk_state_t *s) { s->synced = true; s->unverified = true; }
void tk_on_sntp(tk_state_t *s, int64_t now_unix) {
    s->floor = now_unix; s->synced = true; s->unverified = false; s->last_saved = 0;
}
void tk_check_uptime(tk_state_t *s, int64_t uptime_s) {
    if (s->unverified && uptime_s >= TK_CORROBORATE_UPTIME_S) s->unverified = false;
}
