/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Host test of firmware/src/storage.c (build with -DBOARD_SMK37=1 -Ifirmware/hal) against a simulated NOR flash:
 * erase -> 0xFF, program can only clear bits, page writes must not wrap. The pool of objects through st_*: saves
 * and loads of several parts, a power loss at any point of a save (the previous copy stays current, or the new one
 * is complete), the room refused, a flash error, the sectors erased ahead. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fm1_cc.h"
#include "fm1_flash_map.h"
#define __attribute__(x)   /* (the sources' .pool sections: no Mach-O section names) */

static uint8_t nor[0x100000];
static long budget = -1;               /* power loss: erases and programmed bytes left (-1: no loss) */
static int erase_error, outside;
static uint32_t erases;

static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    if (!FL_IN(off, 0x1000u, FL_DATA_LO, FL_DATA_HI) || (off & 0xFFFu))
        outside++;
    if (erase_error || budget == 0)
        return -8;
    if (budget > 0)
        budget--;
    erases++;
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (!FL_IN(off, n, FL_DATA_LO, FL_DATA_HI))
        outside++;
    if ((off & 0xFFu) + n > 256u) {
        printf("page wrap at %#x\n", off);
        exit(1);
    }
    for (i = 0; i < n; i++) {
        if (budget == 0)
            return -9;
        if (budget > 0)
            budget--;
        nor[off + i] &= s[i];
    }
    return 0;
}
#include "../firmware/src/storage.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

#define N 20000u                        /* A's and B's length, mostly */
static uint8_t a[30u * 4064u], b[30u * 4064u], got[30u * 4064u];

static int holds(uint32_t obj, const uint8_t *want, uint32_t n)   /* the object's current copy is want */
{
    return st_load(obj, got, sizeof got) == (int)n && !memcmp(got, want, n);
}

int main(void)
{
    uint32_t i, k, cut, torn_ok = 1, torn_both = 0;
    long total;
    static uint8_t keep[0x100000];
    for (i = 0; i < sizeof a; i++) {         /* (A and B: their first 20000 bytes, unless said) */
        a[i] = (uint8_t)(i * 7u + 3u);
        b[i] = (uint8_t)(i * 13u + 1u);
    }
    memset(nor, 0xFF, sizeof nor);
    st_boot();
    check("CRC-32 is zlib's (check value 0xCBF43926)", st_crc32("123456789", 9) == 0xCBF43926u);
    check("an empty store: no object, nothing used", st_size(0) < 0 && st_load(4, got, sizeof got) < 0 &&
          st_used() == 0);
    check("save A (20000 bytes: 5 parts) to object 0", st_save(0, a, N) == 0 && st_held(0) == 5u);
    check("load returns A", holds(0, a, N) && st_size(0) == (int)N);
    check("a small object (5 bytes, one part) beside it", st_save(3, "hello", 5) == 0 && holds(3, (const uint8_t *)"hello", 5) &&
          st_used() == 6u);
    check("an empty object: one part", st_save(2, "", 0) == 0 && st_size(2) == 0 && st_held(2) == 1u);
    check("save B over A: B loads, A's sectors no longer held", st_save(0, b, N) == 0 && holds(0, b, N) &&
          st_used() == 7u);
    st_boot();                                   /* power cycle */
    check("after a power cycle: B, the small objects", holds(0, b, N) && holds(3, (const uint8_t *)"hello", 5) &&
          st_size(2) == 0 && st_used() == 7u);

    /* a power loss at every point of a save of A over B (erases and programmed bytes counted): after it, the
     * object is B, or (the save's last byte written) A; never anything else, and the other objects stay */
    memcpy(keep, nor, sizeof keep);
    budget = 1L << 30;
    st_save(0, a, N);
    total = (1L << 30) - budget;
    for (cut = 0; cut <= (uint32_t)total; cut += cut < 300u || (long)cut > total - 300 ? 1u : 61u) {
        memcpy(nor, keep, sizeof nor);
        st_boot();
        budget = (long)cut;
        st_save(0, a, N);
        budget = -1;
        st_boot();                               /* the power back */
        k = holds(0, a, N);
        torn_ok &= (k || holds(0, b, N)) && holds(3, (const uint8_t *)"hello", 5) && st_size(2) == 0;
        torn_both |= k << ((long)cut == total);
    }
    check("a save torn at any byte: the old copy or the whole new one, never half", torn_ok);
    check(".. the new one only once its last byte is written", torn_both == 2u);
    memcpy(nor, keep, sizeof nor);
    st_boot();

    /* room: 29 sectors. B (5) and two small ones held (7) */
    for (i = 0; i < 4u; i++)                     /* object 1 three times (its old copy freed each time), 4 once */
        if (st_save(i == 1u ? 4u : 1u, a, N))
            break;
    check("objects 1 and 4 (5 parts each) fit, saved again and again", i == 4u && st_used() == 17u);
    check("object 2 grown to 5 parts: fits (12 sectors free)", st_save(2, a, N) == 0 && st_used() == 21u);
    check("object 3 grown to 3 parts: fits (8 free, its old copy held until then)",
          st_save(3, a, 3u * ST_PART) == 0 && st_used() == 23u);
    check("object 1 grown to 6 parts (6 free): fits", st_save(1, b, 6u * ST_PART) == 0 &&
          holds(1, b, 6u * ST_PART) && st_used() == 24u);
    memcpy(keep, nor, sizeof keep);
    k = erases;
    check("object 4 grown to 6 parts (5 free): refused with ST_FULL", st_save(4, b, 6u * ST_PART) == ST_FULL);
    check(".. nothing erased or written, its copy kept", erases == k && !memcmp(keep, nor, sizeof nor) &&
          holds(4, a, N));
    check("a smaller copy into the free sector: fits", st_save(4, b, 100) == 0 && holds(4, b, 100));
    check("the biggest object the pool takes: 29 parts, not 30",
          st_save(0, a, 30u * ST_PART) == ST_FULL && ST_POOL * ST_PART < 30u * ST_PART);

    /* a flash error: the erase fails, the copy before stays */
    erase_error = 1;
    st_tidy();
    check("an erase error: the save fails, the copy before stays", st_save(4, a, 50) != 0 && holds(4, b, 100));
    erase_error = 0;

    /* erasing ahead: the free sectors erased one at a time, never a held one */
    memcpy(keep, nor, sizeof keep);
    while (st_tidy())
        ;
    for (i = 0, k = 1; i < 5u; i++)
        k &= st_size(i) >= 0;
    check("st_tidy erases the free sectors only: every object kept", k && holds(1, b, 6u * ST_PART) && holds(4, b, 100));
    check(".. then a save that fits in them needs no erase", st_room_erased(ST_PART) &&
          (k = erases, st_save(4, a, 60) == 0 && erases == k && holds(4, a, 60)));
    check("nothing outside the store window erased or written", outside == 0);
    printf("%s\n", bad ? "STORAGE TEST FAILED" : "storage test passed");
    return bad != 0;
}
