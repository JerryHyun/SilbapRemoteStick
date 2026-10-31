#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "settings.h"
/* Call once from app_main right after settings_load and BEFORE the SD card is mounted, so FatFs
 * timestamps (get_fattime -> time() + localtime_r, components/fatfs/diskio/diskio.c) use the guarded clock:
 * sets TZ from s->tz_min, raises the RTC to the NVS last_time floor if it is lower, starts the hourly
 * NVS saver and registers an IP_EVENT_STA_GOT_IP handler that (re)starts SNTP (pool.ntp.org).
 * The event loop and netif are created here if net_start has not yet done so (both tolerate repeats). */
void    timekeep_start(settings_t *s);
int64_t timekeep_now(void);           /* Unix seconds, never below the floor */
bool    timekeep_synced(void);        /* SNTP or the browser has set the clock since boot */
/* From X-Client-Time (ms) / POST /api/time. Applies tz_min to TZ always (persisting it is the caller's job via
 * settings_save) and sets the RTC when tk_accept_client allows. Returns true when the RTC was set. */
bool    timekeep_client_hint(int64_t client_ms, int tz_min);
