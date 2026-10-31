#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Pure timekeeping rules (host-tested). Times are Unix seconds. */
#define TK_SAVE_INTERVAL_S 3600
#define TK_MIN_CLIENT_S    1735689600LL   /* 2025-01-01: a browser clock earlier than this is not believed */
typedef struct {
    int64_t last_saved;   /* last value written to NVS (0 = never) */
    int64_t floor;        /* the clock never reports less than this (NVS last_time at boot) */
    bool synced;          /* a source (SNTP or the browser) has set the RTC since boot */
    bool unverified;      /* synced only by the browser clock: kept in RAM, never written to NVS until corroborated */
} tk_state_t;
/* now_unix: current RTC seconds. Returns the time to use (never below floor). */
int64_t tk_effective(const tk_state_t *s, int64_t now_unix);
/* true when the NVS copy should be refreshed (>= 3600 s since last_saved, synced and not unverified) */
bool    tk_should_save(const tk_state_t *s, int64_t now_unix);
/* browser time (ms): set the RTC only when not synced, or when the RTC is below floor; never below the floor or TK_MIN_CLIENT_S.
 * After an accepted hint call tk_on_client(). */
bool    tk_accept_client(const tk_state_t *s, int64_t now_unix, int64_t client_ms);
#define TK_CORROBORATE_UPTIME_S (24 * 3600)
/* Browser time was applied: synced, but unverified (not persisted). Does not touch the floor. */
void    tk_on_client(tk_state_t *s);
/* SNTP is authoritative: floor = now (may LOWER a bad floor), synced, verified, next save due at once. */
void    tk_on_sntp(tk_state_t *s, int64_t now_unix);
/* Without SNTP, browser time is trusted for persistence after 24 h of uptime. */
void    tk_check_uptime(tk_state_t *s, int64_t uptime_s);
/* POSIX TZ string for tz_min minutes east of UTC (sign inverted): 540 -> "UTC-9", -330 -> "UTC+5:30". */
void    tk_tz_string(int tz_min, char *out, size_t cap);
