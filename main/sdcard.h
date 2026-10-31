#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "partition_card.h"
esp_err_t sdcard_mount(void);   /* bus width and clock from Kconfig (4-bit, 20 MHz unless set); finds the 16 GiB
                                 * SILBAPSTICK partition (LBA 2048, type 0x0C) and mounts it read-write at SD_MOUNT,
                                 * max_files 8, via a diskio window offset by first_lba. Never formats: a card
                                 * without that layout is refused with an error and nothing is mounted. */
void      sdcard_unmount(void);
/* Drops FatFs's cached FAT and directory sectors by detaching and mounting the same FATFS object again.
 * The machine writes the volume as raw sectors over USB MSC, behind FatFs's back, so without this the web's
 * listing is stale after the machine saves, and the machine's cached directory is stale after an upload.
 * Takes sdcard_lock; ESP_ERR_INVALID_STATE when nothing is mounted. Never unmounts for good: if the second
 * f_mount fails it retries once and logs loudly. An open file/dir object does not stop it (this FatFs has
 * FF_FS_LOCK = 0: f_mount(NULL) checks nothing and flushes nothing) but such an object becomes invalid, so
 * the caller must only call this on a quiet volume. */
esp_err_t sdcard_remount(void);
bool      sdcard_present(void); /* mounted and sdmmc_get_status() (CMD13) == ESP_OK; takes sdcard_lock */
/* The partition the host is served (the same window /sd is mounted through). NULL when nothing is mounted; valid
 * until the next sdcard_unmount(). */
const card_vol_t *sdcard_volume(void);
#include "sdmmc_cmd.h"
sdmmc_card_t *sdcard_card(void); /* NULL without a card */
void      sdcard_bench(void);   /* dev build: starts a task that benchmarks raw reads 10 s later (aligned DMA vs unaligned buffer) */
uint32_t  sdcard_serial(void);  /* card CID serial, for the volume serial mix; 0 without a card */
const char *sdcard_ff_drive(void); /* FatFs drive SD_MOUNT is mounted from ("0:"), for the catalog's f_readdir scan */
void      sdcard_lock(void);    /* recursive mutex around every SD access */
void      sdcard_unlock(void);
/* Lock rule: every SD/VFS access takes sdcard_lock (recursive): catalog_build/load, cat_vfat_ops data reads
 * (via read10), prepare_sd, mount/unmount and the CMD13 status poll. Two exceptions, both relying on FatFs's
 * own per-volume lock (FF_FS_REENTRANT=1): the sdlog task's appends, and the boot verify's metadata-only
 * catalog_fingerprint scan (holding the lock for it would stall every READ10). TinyUSB callbacks never do SD
 * I/O themselves except vfat_read under the lock, and read10 checks the state before taking it (and again
 * after), so a BUSY disk never makes USB wait on a long build. */
