/* SPDX-License-Identifier: GPL-3.0-only
 * Felucca on the SMK-37 Pro (BOARD_SMK37); V-Track: Copyright (C) 2026 Manuel Rubin */
/* SMK-37 Pro store owner tag, the data family VTRACK (storage.c st_boot, build with -DBOARD_SMK37=1): a simulated
 * flash with another firmware's store (Felucca's), the stock firmware's VM, an erased one and V-Track's own; only
 * the store window FL_DATA is ever erased or written, ours is kept, and nothing of another family is read. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fm1_cc.h"
#if !BOARD_SMK37
#error "build with -DBOARD_SMK37=1"
#endif
#include "fm1_flash_map.h"

static uint8_t nor[0x100000];
static int bad_write, erases;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    if ((off & 0xFFFu) || !FL_IN(off, 0x1000u, FL_DATA_LO, FL_DATA_HI))
        bad_write++;
    erases++;
    memset(nor + off, 0xFF, 0x1000);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (!FL_IN(off, n, FL_DATA_LO, FL_DATA_HI))
        bad_write++;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
#include "../firmware/src/storage.c"

static int bad;
static void check(const char *what, int ok) { printf("smk owner: %-74s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }

static void others(void)                           /* outside the store window: never touched */
{
    memset(nor, 0xFF, sizeof nor);
    memset(nor + 0xC1000, 0x33, 0x1000);           /* USRTRIM: the unit's calibration */
    memset(nor + 0xC2000, 0x44, 0x1000);           /* USRFLASH: the stock mapping, a Felucca sample */
    memcpy(nor + 0xEAFF8, "usrdata", 8);           /* .. the stock firmware's end marker */
    memset(nor + 0xBB000, 0x55, 0x1000);           /* the update staging */
}
static int others_kept(void)
{
    return nor[0xC1000] == 0x33 && nor[0xC1FFF] == 0x33 && nor[0xC2000] == 0x44 && !memcmp(nor + 0xEAFF8, "usrdata", 8) &&
           nor[0xBB000] == 0x55 && bad_write == 0;
}
static int only_tag(void)                          /* the window erased, our tag in its first sector */
{
    uint32_t i;
    if (memcmp(nor + FL_DATA_LO, "SMK37DATAVTRACK", 16))
        return 0;
    for (i = FL_DATA_LO + 16u; i < FL_DATA_HI; i++)
        if (nor[i] != 0xFF)
            return 0;
    return 1;
}

int main(void)
{
    static uint8_t keep[0x100000];
    others();                                      /* Felucca's store: objects, its tag at 0xAF000, its voice bank */
    memset(nor + 0x9D000, 0x11, 0x12000);
    memcpy(nor + 0xAF000, "SMK37DATAFELUCCA", 16);
    memset(nor + 0xB0000, 0x22, 0x1000);
    st_boot();
    check("Felucca's store: cleared, tagged VTRACK, no object read", only_tag() && st_owner_cleared && st_used() == 0);
    check("... calibration, USRFLASH, its marker and the update staging untouched", others_kept());

    others();                                      /* V-Track-shaped data under another tag: still not ours */
    st_owner_cleared = 0;
    memcpy(nor + FL_DATA_LO, "SMK37DATAVTRACK", 16);
    st_boot();
    st_save(0, "song", 4);
    memcpy(nor + FL_DATA_LO, "SMK37DATAOTHER\0", 16);
    st_boot();
    check("another tag over objects shaped like ours: cleared, none read", only_tag() && st_owner_cleared &&
          st_size(0) < 0);

    others();                                      /* the stock VM: anything there */
    st_owner_cleared = 0;
    memset(nor + 0x9D000, 0x5A, 0x1E000);
    st_boot();
    check("the stock firmware's VM: cleared, tagged", only_tag() && st_owner_cleared && others_kept());

    others();                                      /* erased (a first install over nothing): only the tag written */
    st_owner_cleared = 0;
    erases = 0;
    st_boot();
    check("an erased window: the tag written, nothing erased, nothing said", only_tag() && erases == 0 &&
          !st_owner_cleared);

    st_save(2, "kept", 4);                         /* ours: kept as it is */
    memcpy(keep, nor, sizeof keep);
    erases = 0;
    st_boot();
    check("V-Track's own store: kept, nothing erased or written", !memcmp(keep, nor, sizeof nor) && erases == 0 &&
          st_size(2) == 4 && !st_owner_cleared);
    check("never outside the store window", bad_write == 0);
    printf("%s\n", bad ? "SMK OWNER TEST FAILED" : "smk owner test passed");
    return bad != 0;
}
