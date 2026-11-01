#include "sdcard.h"
#include "board.h"
#include "partition_card.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>

static const char *TAG = "sdcard";

/* The machine's volume is mounted, not the card: one path, so these statics describe the only drive
 * this firmware ever registers with FatFs. */
#define SD_MAX_FILES 8

static sdmmc_card_t *s_card;
static card_vol_t s_vol;                 /* valid while s_card != NULL */
static BYTE s_pdrv = FF_DRV_NOT_USED;
static FATFS *s_fs;
static SemaphoreHandle_t s_mutex;
static bool s_shape_logged;              /* the refusal is logged in full once, then quietly */
static bool s_remount_warned;            /* one loud re-mount failure, then debug: the caller retries every tick */
static bool s_remount_broken;            /* the once-only ESP_LOGE for a re-mount that failed twice */

static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
}

/* ---- ff_diskio implementation over a window of the card ----
 * FatFs sees sector 0 as the first sector of the SILBAPSTICK partition (its FAT32 boot sector), so the
 * MBR stays invisible to it and nothing can walk into the archival partition by accident. Everything
 * the driver touches is shifted by first_lba and bounded by sectors. The caller of f_mount/f_open holds
 * sdcard_lock (see sdcard.h); the driver does not take it again. */

static DSTATUS vol_initialize(BYTE pdrv) {
    (void)pdrv;
    return (s_card && sdmmc_get_status(s_card) == ESP_OK) ? 0 : STA_NOINIT;
}

static DSTATUS vol_status(BYTE pdrv) {
    (void)pdrv;
    /* FatFs calls this from validate() and from mount_volume()'s "already mounted" early-out. Returning 0
     * unconditionally (as the old code did) means the early-out can serve a FATFS object whose cached BPB,
     * cluster size and FAT base belong to a card that is no longer in the slot, after a fast swap the 2 s
     * presence poll did not see. So report the one thing this layer knows without I/O: no card object at all.
     *
     * Deliberately NOT a CMD13 here: diskio_sdmmc.c's ff_sdmmc_status() only polls when the caller enabled
     * s_disk_status_check_en (it is off by default, ff_diskio_register_sdmmc sets it false), because status is
     * called on every validate()/mount_volume(). Polling here would double the poll the presence task already
     * does every 2 s and could stall a READ10 for a card that is merely busy. The swap window is closed by the
     * refresh task instead: it unmounts on two missed probes, and a re-mount (or the CID mismatch path) rebuilds
     * the FATFS object. */
    return s_card ? 0 : STA_NOINIT;
}

static bool vol_in_range(uint32_t sector, UINT count) {
    return (uint64_t)sector + count <= (uint64_t)s_vol.sectors;
}

static DRESULT vol_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (!s_card) return RES_NOTRDY;
    if (!vol_in_range(sector, count)) return RES_PARERR;
    esp_err_t err = sdmmc_read_sectors(s_card, buff, (size_t)(sector + s_vol.first_lba), count);
    if (err != ESP_OK) { ESP_LOGE(TAG, "read lba %lu+%lu failed: %s", (unsigned long)sector, (unsigned long)count, esp_err_to_name(err)); return RES_ERROR; }
    return RES_OK;
}

static DRESULT vol_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (!s_card) return RES_NOTRDY;
    if (!vol_in_range(sector, count)) return RES_PARERR;
    esp_err_t err = sdmmc_write_sectors(s_card, buff, (size_t)(sector + s_vol.first_lba), count);
    if (err != ESP_OK) { ESP_LOGE(TAG, "write lba %lu+%lu failed: %s", (unsigned long)sector, (unsigned long)count, esp_err_to_name(err)); return RES_ERROR; }
    return RES_OK;
}

static DRESULT vol_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(DWORD *)buff = s_vol.sectors;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = SD_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            return RES_ERROR;            /* same answer as diskio_sdmmc.c */
        case CTRL_TRIM:
            return RES_PARERR;           /* no discard/erase on this window; FatFs ignores the result */
    }
    return RES_ERROR;
}

static const ff_diskio_impl_t s_vol_impl = {
    .init = &vol_initialize,
    .status = &vol_status,
    .read = &vol_read,
    .write = &vol_write,
    .ioctl = &vol_ioctl,
};

static void log_bad_card(esp_err_t why) {
    if (s_shape_logged) {
        ESP_LOGD(TAG, "card still has no SILBAPSTICK volume: %s", esp_err_to_name(why));
        return;
    }
    s_shape_logged = true;
    switch (why) {
        case ESP_ERR_INVALID_RESPONSE:
            ESP_LOGE(TAG, "card has no MBR: LBA 0 does not end in 0x55AA (a new card is usually one big exFAT volume)");
            break;
        case ESP_ERR_NOT_FOUND:
            ESP_LOGE(TAG, "MBR found, but partition 1 is not a 0x0C volume starting at LBA >= 2048");
            break;
        default:
            ESP_LOGE(TAG, "cannot read the card's partition table: %s", esp_err_to_name(why));
            break;
    }
    ESP_LOGE(TAG, "expected layout: partition 1 type 0x0C at LBA 2048, 33554432 sectors (16 GiB, FAT32, "
                  "label SILBAPSTICK); partition 2 type 0x0C, label SILBAP, not mounted");
    ESP_LOGE(TAG, "nothing was mounted and nothing was formatted; repartition the card with tools/mkcard.py");
}

esp_err_t sdcard_mount(void) {
    ensure_mutex();
    if (s_card) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    #if CONFIG_SILBAP_SD_FREQ_HIGHSPEED
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    #else
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    #endif
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    #if CONFIG_SILBAP_SD_WIDTH_1
    slot.width = 1;
    #else
    slot.width = 4;
    #endif
    slot.clk = BOARD_SD_CLK; slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0; slot.d1 = BOARD_SD_D1; slot.d2 = BOARD_SD_D2; slot.d3 = BOARD_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = calloc(1, sizeof *card);
    if (!card) return ESP_ERR_NO_MEM;

    bool host_up = false, diskio_up = false, vfs_up = false;
    char drv[3] = { '0', ':', 0 };
    esp_err_t e;

    sdcard_lock();

    /* sdmmc_card_init needs the host and the slot up; esp_vfs_fat_sdmmc_mount used to do this for us,
     * but it also mounts the whole card, which is no longer what /sd means. */
    e = host.init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "host init failed: %s", esp_err_to_name(e)); goto fail; }
    host_up = true;
    e = sdmmc_host_init_slot(host.slot, &slot);
    if (e != ESP_OK) { ESP_LOGE(TAG, "slot init failed: %s", esp_err_to_name(e)); goto fail; }
    e = sdmmc_card_init(&host, card);
    if (e != ESP_OK) { ESP_LOGE(TAG, "card init failed: %s", esp_err_to_name(e)); goto fail; }

    /* Read the real MBR before FatFs sees anything: the volume is a partition, not the card. */
    e = card_volume_read(card, &s_vol);
    if (e != ESP_OK) { log_bad_card(e); goto fail; }

    BYTE pdrv = FF_DRV_NOT_USED;
    e = ff_diskio_get_drive(&pdrv);
    if (e != ESP_OK) { ESP_LOGE(TAG, "no free FatFs drive"); goto fail; }
    s_pdrv = pdrv;
    ff_diskio_register(pdrv, &s_vol_impl);
    diskio_up = true;
    s_card = card;                       /* the diskio callbacks need it from here on */

    drv[0] = (char)('0' + pdrv);
    esp_vfs_fat_conf_t conf = { .base_path = SD_MOUNT, .fat_drive = drv, .max_files = SD_MAX_FILES };
    e = esp_vfs_fat_register_cfg(&conf, &s_fs);
    if (e == ESP_ERR_INVALID_STATE) {    /* something else already owns SD_MOUNT: not ours to fix */
        ESP_LOGE(TAG, "%s is already registered with VFS", SD_MOUNT);
        s_fs = NULL;
        goto fail;
    }
    if (e != ESP_OK) { ESP_LOGE(TAG, "VFS register failed: %s", esp_err_to_name(e)); s_fs = NULL; goto fail; }
    vfs_up = true;

    /* 1: mount now, so a wrong or unformatted filesystem is an error here rather than on the first fopen. */
    FRESULT res = f_mount(s_fs, drv, 1);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "mounting %s (lba %lu, %lu sectors) failed: FatFs %d; the volume is not a readable "
                 "FAT32 filesystem (not formatted, not repaired)", SD_MOUNT,
                 (unsigned long)s_vol.first_lba, (unsigned long)s_vol.sectors, (int)res);
        e = ESP_FAIL;
        goto fail;
    }

    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    uint64_t total = 0, freeb = 0;
    if (f_getfree(drv, &free_clusters, &fs) == FR_OK && fs) {
        total = (uint64_t)(fs->n_fatent - 2) * fs->csize * SD_SECTOR_SIZE;
        freeb = (uint64_t)free_clusters * fs->csize * SD_SECTOR_SIZE;
    }

    sdcard_unlock();
    s_shape_logged = false;
    ESP_LOGI(TAG, "mounted %s rw: SILBAPSTICK at lba %lu, %lu sectors (%.0f MB, %.0f MB free), drive %s",
             SD_MOUNT, (unsigned long)s_vol.first_lba, (unsigned long)s_vol.sectors,
             (double)total / (1024.0 * 1024.0), (double)freeb / (1024.0 * 1024.0), drv);
    ESP_LOGI(TAG, "card %s, serial %08lx, %d-bit bus, requested %d kHz, negotiated %d kHz%s",
             card->cid.name, (unsigned long)card->cid.serial, (int)slot.width,
             (int)host.max_freq_khz, (int)card->real_freq_khz, card->is_ddr ? ", DDR" : "");
    return ESP_OK;

fail:
    /* Undo in reverse order. The host is deinitialized last and only if init succeeded, since
     * card->host is copied in by sdmmc_card_init and is zero before that. */
    if (vfs_up) esp_vfs_fat_unregister_path(SD_MOUNT);
    if (diskio_up) ff_diskio_unregister(s_pdrv);
    s_pdrv = FF_DRV_NOT_USED;
    s_fs = NULL;
    s_card = NULL;
    if (host_up) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) host.deinit_p(host.slot);
        else host.deinit();
    }
    free(card);
    sdcard_unlock();
    return e;
}

void sdcard_unmount(void) {
    ensure_mutex();
    sdcard_lock();
    if (s_card) {
        char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
        f_mount(NULL, drv, 0);                 /* flush and detach the FATFS object */
        ff_diskio_unregister(s_pdrv);
        esp_vfs_fat_unregister_path(SD_MOUNT);
        sdmmc_card_t *card = s_card;
        if (card->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) card->host.deinit_p(card->host.slot);
        else card->host.deinit();
        free(card);
        s_card = NULL;
        s_fs = NULL;
        s_pdrv = FF_DRV_NOT_USED;
    }
    sdcard_unlock();
}

/* Drops FatFs's cached sectors: unregister the FATFS object, register it again and force the mount. What
 * f_mount(NULL, drv, 0) does here (components/fatfs/src/ff.c, f_mount, IDF v5.5.2): it checks no open object,
 * calls clear_share() (compiled out: FF_FS_LOCK is 0 in this build, CONFIG_FATFS_FS_LOCK=0), deletes the volume
 * mutex (FF_FS_REENTRANT=1), does NOT flush the sector window, sets cfs->fs_type = 0 and FatFs[vol] = 0. So the
 * call never refuses and never reports a reason; it simply invalidates every open FIL/FF_DIR (validate() compares
 * obj->id with fs->id, and mount_volume assigns a new one), and a later f_close on one returns FR_INVALID_OBJECT
 * and loses its dirty tail. That is why the caller must only come here on a quiet volume. The second f_mount
 * allocates a fresh mutex, mounts the BPB again (fs->id = ++Fsid) and leaves the drive usable. */
esp_err_t sdcard_remount(void) {
    ensure_mutex();
    sdcard_lock();
    if (!s_card || !s_fs || s_pdrv == FF_DRV_NOT_USED) {
        sdcard_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    f_mount(NULL, drv, 0);                 /* no flush, no refusal: see above */
    FRESULT res = f_mount(s_fs, drv, 1);
    if (res != FR_OK) {
        if (!s_remount_warned) {
            s_remount_warned = true;
            ESP_LOGW(TAG, "re-mount of %s failed: FatFs %d; /sd is unmounted, trying once more", SD_MOUNT, (int)res);
        } else {
            ESP_LOGD(TAG, "re-mount of %s still failing: FatFs %d, retrying", SD_MOUNT, (int)res);
        }
        res = f_mount(s_fs, drv, 1);
        if (res != FR_OK) {
            /* Nothing else can mount it: the VFS is still registered and the diskio window valid, so the next
             * sdcard_remount (the caller retries) is the recovery path. Say so once, then keep quiet. */
            if (!s_remount_broken) {
                s_remount_broken = true;
                ESP_LOGE(TAG, "re-mount of %s failed twice: FatFs %d; /sd stays unmounted until the next retry or card event",
                         SD_MOUNT, (int)res);
            }
            sdcard_unlock();
            return ESP_FAIL;
        }
    }
    sdcard_unlock();
    s_remount_warned = false;
    s_remount_broken = false;
    ESP_LOGD(TAG, "re-mounted %s: FatFs caches dropped", SD_MOUNT);
    return ESP_OK;
}

bool sdcard_present(void) {
    sdcard_lock();                       /* CMD13 is SD access too: not in the middle of a READ10's transfers */
    sdmmc_card_t *c = s_card;
    bool ok = c && sdmmc_get_status(c) == ESP_OK;
    sdcard_unlock();
    return ok;
}

sdmmc_card_t *sdcard_card(void) { return s_card; }
const card_vol_t *sdcard_volume(void) { return s_card ? &s_vol : NULL; }

#if CONFIG_SILBAP_DEV_CDC
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"
/* The raw-read bench is compiled out on purpose: it delayed 10 s and then held the SD lock for
 * 1.25 MiB of reads, which stalled the first catalog build. The MC100E asks TEST UNIT READY about
 * 1 s after boot and had given up long before the disk became READY (measured tur#1 at 1.1 s,
 * READY at 30.6 s), so the bench turned a 3 s boot into a 30 s one. Kept for reference; re-enable
 * only when nothing is waiting on the disk. */
#if 0
/* Reads total bytes from the card middle in 8 KiB chunks into buf and logs KB/s. */
static void bench_one(sdmmc_card_t *c, uint8_t *buf, size_t total, const char *what) {
    const size_t chunk = 8192, sec = c->csd.sector_size, n = chunk / sec, start = (size_t)c->csd.capacity / 2;
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = ESP_OK; size_t done = 0;
    for (; done < total && e == ESP_OK; done += chunk) e = sdmmc_read_sectors(c, buf, start + done / sec, n);
    int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "raw SD read bench (%s, buf %p): %u KiB in %lld ms = %lld KB/s (%s)", what, (void *)buf, (unsigned)(done / 1024),
             (long long)(us / 1000), us ? (long long)((int64_t)done * 1000000 / 1024 / us) : 0LL, esp_err_to_name(e));
}
static void bench_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));    /* after the host has opened the CDC console */
    sdcard_lock();
    sdmmc_card_t *c = s_card;
    if (c && (size_t)c->csd.capacity > 4096) {
        uint8_t *dma = heap_caps_aligned_alloc(4, 8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        /* deliberately odd address: not 4-byte aligned, so the driver must use its per-sector bounce path */
        uint8_t *raw = malloc(8192 + 4);
        if (dma) bench_one(c, dma, 1024 * 1024, "DMA-capable aligned, 1 MiB");
        if (raw) bench_one(c, raw + 1, 256 * 1024, "unaligned heap, 256 KiB");
        free(dma); free(raw);
    }
    sdcard_unlock();
    vTaskDelete(NULL);
}
#endif
void sdcard_bench(void) {}
#else
void sdcard_bench(void) {}
#endif

uint32_t sdcard_serial(void) { return s_card ? (uint32_t)s_card->cid.serial : 0; }

/* FatFs drive SD_MOUNT is mounted from ("0:"), for the catalog's f_readdir scan and for anything that
 * needs the volume the POSIX path maps to. One drive, one volume: s_pdrv. */
const char *sdcard_ff_drive(void) {
    static char drv[3] = "0:";
    if (s_pdrv < 10) drv[0] = (char)('0' + s_pdrv);
    return drv;
}

void sdcard_lock(void)   { ensure_mutex(); xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void sdcard_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }
