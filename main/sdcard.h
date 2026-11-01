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
 * f_mount fails it retries once (ESP_FAIL when both fail: the drive is then registered with fs_type == 0).
 * f_mount(NULL) flushes nothing (this FatFs has FF_FS_LOCK = 0: it checks no open object and writes no dirty
 * window), so it invalidates every open FIL/FF_DIR and can lose a dirty FAT sector: the caller must not call
 * this while a web operation holds the volume open. That guard lives in refresh.c, next to fsops' in-flight
 * counter; sdcard.c must not depend on fsops. */
esp_err_t sdcard_remount(void);
bool      sdcard_present(void); /* mounted and sdmmc_get_status() (CMD13) == ESP_OK; takes sdcard_lock */
/* The partition the host is served (the same window /sd is mounted through). NULL when nothing is mounted; valid
 * until the next sdcard_unmount(). */
const card_vol_t *sdcard_volume(void);
#include "sdmmc_cmd.h"
sdmmc_card_t *sdcard_card(void); /* NULL without a card */
void      sdcard_bench(void);   /* dev build: starts a task that benchmarks raw reads 10 s later (aligned DMA vs unaligned buffer) */
uint32_t  sdcard_serial(void);  /* card CID serial, for the volume serial mix; 0 without a card */
const char *sdcard_ff_drive(void); /* FatFs drive SD_MOUNT is mounted from ("0:"), for anything that needs the volume */
void      sdcard_lock(void);    /* recursive mutex around every SD access */
void      sdcard_unlock(void);
/* Lock rule: every SD/VFS access takes sdcard_lock (recursive): mount/unmount/remount, the CMD13 status poll,
 * every fsops step, the refresh task's counting walk, the web's directory scans and the WebDAV file stream (one
 * short hold per readdir/stat/read chunk, never across a network send or receive). The sdlog task is the one
 * writer that relies on FatFs's own per-volume lock (FF_FS_REENTRANT=1) instead, because holding sdcard_lock
 * would stall every READ10 behind a log append. TinyUSB callbacks check the MSC state before taking the lock
 * (and the diskio window again after), so a BUSY disk never makes USB wait on a long operation. */
