#include "t.h"
#include "partition_card.h"
#include <string.h>

static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void test_find(void) {
    uint8_t mbr[512] = {0};
    card_vol_t v;
    /* no table */
    T_EQ_INT(card_volume_find(mbr, 1000, &v), -1);
    /* one FAT32 LBA entry from 2048 */
    mbr[510] = 0x55; mbr[511] = 0xAA;
    mbr[446 + 4] = 0x0C;
    w32(mbr + 446 + 8, 2048);
    w32(mbr + 446 + 12, 33554432);
    T_EQ_INT(card_volume_find(mbr, 33556480, &v), 1);
    T_EQ_INT(v.first_lba, 2048); T_EQ_INT(v.sectors, 33554432);
    /* the volume claims more than the card has */
    T_EQ_INT(card_volume_find(mbr, 1000, &v), 0);
    /* a non-FAT32 type byte is not our partition */
    mbr[446 + 4] = 0x07;
    T_EQ_INT(card_volume_find(mbr, 33556480, &v), 0);
}

static void test_16gib(void) {
    /* the size the tool writes is exactly 16 GiB in 512-byte sectors */
    T_EQ_INT(16ull * 1024 * 1024 * 1024 / 512, 33554432u);
}

/* Not in the plan's list, but the checks below pin the edges the mount relies on. */
static void test_edges(void) {
    uint8_t mbr[512] = {0};
    card_vol_t v;
    mbr[510] = 0x55; mbr[511] = 0xAA;
    mbr[446 + 4] = 0x0C;
    /* the card's own capacity is the exact end of the volume */
    w32(mbr + 446 + 8, 2048); w32(mbr + 446 + 12, 33554432);
    T_EQ_INT(card_volume_find(mbr, 2048 + 33554432, &v), 1);
    T_EQ_INT(card_volume_find(mbr, 2048 + 33554432 - 1, &v), 0);   /* one sector short */
    /* a first LBA below 2048 is not the layout the tool writes */
    w32(mbr + 446 + 8, 63); w32(mbr + 446 + 12, 33554432);
    T_EQ_INT(card_volume_find(mbr, 1u << 30, &v), 0);
    /* a zero-length entry is not a volume */
    w32(mbr + 446 + 8, 2048); w32(mbr + 446 + 12, 0);
    T_EQ_INT(card_volume_find(mbr, 1u << 30, &v), 0);
    /* the archival partition is type 0x0C too: entry 0 missing must not silently fall through */
    w32(mbr + 446 + 8, 0); w32(mbr + 446 + 12, 0);
    mbr[446 + 16 + 4] = 0x0C;
    w32(mbr + 446 + 16 + 8, 33556480); w32(mbr + 446 + 16 + 12, 100000);
    T_EQ_INT(card_volume_find(mbr, 1u << 30, &v), 0);
    /* a 0x55 byte without its 0xAA pair is not a signature */
    mbr[511] = 0;
    T_EQ_INT(card_volume_find(mbr, 1u << 30, &v), -1);
}

int main(void) {
    T_RUN(test_find);
    T_RUN(test_16gib);
    T_RUN(test_edges);
    T_DONE();
}
