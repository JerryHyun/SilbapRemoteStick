#include "partition_card.h"

/* MBR layout: 4 primary entries of 16 bytes at 446, signature 0x55AA at 510. */
#define MBR_PART_TABLE  446
#define MBR_ENTRY_SIZE  16
#define MBR_SIG_LO      510
#define MBR_SIG_HI      511
#define MBR_TYPE_OFF    4
#define MBR_LBA_OFF     8
#define MBR_COUNT_OFF   12
#define MBR_TYPE_FAT32  0x0C
#define MBR_MIN_FIRST_LBA 2048u   /* 1 MiB: where the layout tool puts partition 1 */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int card_volume_find(const uint8_t mbr[512], uint32_t card_sectors, card_vol_t *out) {
    if (!mbr || !out) return -1;
    if (mbr[MBR_SIG_LO] != 0x55 || mbr[MBR_SIG_HI] != 0xAA) return -1;

    /* Entry 0 only, on purpose: it is the machine's 16 GiB volume. The archival partition is also
     * type 0x0C, so scanning past a damaged entry 0 could mount a 100+ GiB volume at /sd. */
    const uint8_t *e = mbr + MBR_PART_TABLE;
    if (e[MBR_TYPE_OFF] != MBR_TYPE_FAT32) return 0;
    uint32_t first = le32(e + MBR_LBA_OFF);
    uint32_t count = le32(e + MBR_COUNT_OFF);
    if (count == 0 || first < MBR_MIN_FIRST_LBA) return 0;
    if ((uint64_t)first + count > (uint64_t)card_sectors) return 0;   /* claims more than the card has */
    out->first_lba = first;
    out->sectors = count;
    return 1;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "sdmmc_cmd.h"

esp_err_t card_volume_read(sdmmc_card_t *card, card_vol_t *out) {
    if (!card || !out) return ESP_ERR_INVALID_ARG;
    if (card->csd.sector_size != 512) return ESP_ERR_NOT_SUPPORTED;
    /* 64-byte aligned so the one sector read takes the DMA path instead of the bounce path. */
    uint8_t mbr[512] __attribute__((aligned(64)));
    esp_err_t err = sdmmc_read_sectors(card, mbr, 0, 1);
    if (err != ESP_OK) return err;
    int r = card_volume_find(mbr, (uint32_t)card->csd.capacity, out);
    if (r == 1) return ESP_OK;
    return r < 0 ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_FOUND;
}
#endif
