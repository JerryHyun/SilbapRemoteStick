/* Binds fsops to the firmware: the SD lock, the live catalog's open files, the refresh task, FatFs free space and
 * the guarded clock. Kept apart from fsops.c so that file builds and is tested on the host. */
#include "fsops.h"
#include "board.h"
#include "esp_vfs_fat.h"
#include "refresh.h"
#include "sdcard.h"
#include "timekeep.h"

static void changed(void) { refresh_request(REFRESH_CHANGED); }
static void activity(void) { refresh_activity(); }
static uint64_t free_bytes(void) {
    uint64_t total = 0, fr = 0;
    sdcard_lock();
    esp_err_t e = esp_vfs_fat_info(SD_MOUNT, &total, &fr);
    sdcard_unlock();
    return e == ESP_OK ? fr : 0;
}

void fs_esp_init(void) {
    static const fs_hooks_t h = {
        .lock = sdcard_lock, .unlock = sdcard_unlock,
        .close_catalog_files = refresh_close_data_files,
        .changed = changed,
        .activity = activity,
        .free_bytes = free_bytes,
        .now = timekeep_now,
    };
    fs_init(SD_MOUNT, SILBAP_DIR "/tmp", &h);
}
