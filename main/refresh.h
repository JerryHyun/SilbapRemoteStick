#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "settings.h"
typedef enum { REFRESH_CHANGED = 0, REFRESH_FORCED = 1 } refresh_kind_t;
/* Starts the SD watcher task (core 0): presence poll, mount on insert, unmount + MSC state on removal, and the LED.
 * The disk itself is mounted by app_main before this call; there is no catalog and no rebuild any more, so this
 * only answers. Call after sdcard_mount was attempted, sdlog_init and usb_msc_start. */
void refresh_start(const settings_t *s);
/* Safe from any task, kept for the API surface (button, settings, fsops). Wakes the watcher for its coherency step
 * and asks it to re-count the volume (the count itself is coalesced: at most one walk per COUNT_MIN_MS). */
void refresh_request(refresh_kind_t kind);
/* Stamps "the web just touched the card". fsops calls this after it unlocks, on every mutation (and on every
 * upload chunk), so the coherency re-mount in refresh_task waits for a quiet bus and does not invalidate a
 * file the web still has open. A stamp cannot say "an upload is still open", so the re-mount is additionally
 * gated on fsops' in-flight counter (fs_inflight()); this is only the quiet-window half of the rule.
 * Safe from any task; never blocks, never touches the SD. */
void refresh_activity(void);
/* fsops calls this under sdcard_lock before every delete, overwrite, rename or move, so a reader that holds an
 * open file re-stats it. There is no catalog holding files open any more: this is a no-op kept for the hook. */
void refresh_close_data_files(void);
/* last_ms: duration of the last catalog build (0: no catalog any more); last_wall: Unix time that build started.
 * started: the watcher has published at least one status (distinguishes "no SD card" from "not running yet").
 * sd_ok: a card is in the slot; fs_ok: the volume on it is mounted and usable. They differ after a failed
 * re-mount: the card is still there but every /sd access returns EIO, so sd_ok stays true and fs_ok goes false
 * (and /api/status reports both). files/dirs are the volume's contents as the refresh task last counted them
 * with the web listing's rules (fs_hidden_name and .silbap skipped): a snapshot, refreshed shortly after a mount
 * and after each change, at most once per COUNT_MIN_MS, and 0 without a usable volume. They are not an atomic
 * count of the disk. */
typedef struct { uint32_t files, dirs; bool busy, sd_ok, fs_ok, started; uint32_t last_ms; int64_t last_wall; } refresh_status_t;
void refresh_status(refresh_status_t *out);
/* Creates SILBAP_DIR and its tmp folder (emptied), and EMB/Embf on an empty card. Call with the card mounted. */
void refresh_prepare_sd(void);
/* Forgets the LED colour refresh last set, so its idle loop paints it again within a tick (after the web API
 * showed purple during an upload that ended without a refresh). */
void refresh_led_reset(void);
