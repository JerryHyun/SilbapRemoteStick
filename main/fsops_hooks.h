#pragma once
#include <stdint.h>
/* What fsops needs from the firmware. Host tests pass fakes that count calls; main/fsops_esp.c binds the real ones. */
typedef struct {
    void (*lock)(void); void (*unlock)(void);   /* firmware: sdcard_lock/unlock (recursive) */
    void (*close_catalog_files)(void);          /* firmware: cat_close_files(live) — called under lock */
    void (*changed)(void);                      /* firmware: refresh_request(REFRESH_CHANGED) — called after unlock */
    void (*activity)(void);                     /* firmware: refresh_activity — called after unlock, on every mutation */
    uint64_t (*free_bytes)(void);               /* free space on the card; called under lock */
    int64_t (*now)(void);                       /* timekeep_now: Unix seconds */
} fs_hooks_t;
