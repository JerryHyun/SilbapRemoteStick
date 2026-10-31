#pragma once
#include <stdint.h>

/* The machine's volume: a FAT32 (type 0x0C) primary partition on the SD card, written by
 * tools/mkcard.py as partition 1 at LBA 2048, 33554432 sectors (16 GiB), label SILBAPSTICK.
 * The card is mounted at /sd through a diskio window starting at first_lba, so FatFs sees the
 * partition's boot sector as sector 0 and never the MBR. */
typedef struct {
    uint32_t first_lba;
    uint32_t sectors;
} card_vol_t;

/* Parses an MBR (the first 512 bytes of the card). Pure: no I/O, host-tested.
 * Returns 1 with *out filled when partition entry 0 is type 0x0C, starts at LBA >= 2048 and ends
 * inside card_sectors; 0 when there is no usable entry 0 (wrong type, or it does not fit);
 * -1 when the 512 bytes do not end in the 0x55AA signature, i.e. there is no partition table. */
int card_volume_find(const uint8_t mbr[512], uint32_t card_sectors, card_vol_t *out);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "sdmmc_cmd.h"

/* Reads LBA 0 of the card and runs card_volume_find on it. ESP_OK when a volume was found,
 * ESP_ERR_INVALID_RESPONSE for a missing MBR signature, ESP_ERR_NOT_FOUND for an MBR without a
 * usable type 0x0C entry 0. Does raw sector I/O: call with sdcard_lock() held. */
esp_err_t card_volume_read(sdmmc_card_t *card, card_vol_t *out);
#endif
