/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Persistent storage on the SPI NOR: V-Track's store, the data family VTRACK, in the store window FL_DATA
 * (0x9D000..0xBAFFF, fm1_flash_map.h) and nowhere else.
 *   st_boot()                   once at power-on, before any other st_* call: the owner tag checked (below), the
 *                               pool scanned. Without it (no flash part answered) the store is empty and refuses
 *                               every save
 *   st_save(obj, src, len)      object obj (0 .. ST_OBJECTS - 1) written anew: 0, ST_FULL if the pool has no room
 *                               for it beside every object's current copy (nothing written), or another negative
 *                               value if the flash failed (the object's previous copy stays current)
 *   st_load(obj, dst, max)      the object's current copy into dst: its length, -1 if there is none (or it is
 *                               longer than max)
 *   st_size(obj)                its length, -1 if there is none
 *   st_held(obj)                the sectors its current copy takes; st_used(): every object's; ST_POOL sectors in all
 *   st_tidy()                   one sector no current copy holds erased ahead of a save, if one needs it: 1 if done
 *   st_room_erased(len)         1 if len bytes fit into sectors already erased (a save without an erase)
 *   st_owner_cleared            1 if the boot cleared another firmware's data
 * The pool: the sectors after the owner tag. An object is stored as a copy of one or more parts, a part a sector:
 * its 32-byte header (magic, object, part, parts, the copy's sequence number, the part's and the object's length,
 * the part's CRC, the header's CRC) at offset 0, written LAST, the part's bytes after it. A copy is complete when
 * every one of its parts is there with valid CRCs; an object's current copy is its complete copy with the highest
 * sequence number. A save writes a new copy, with a sequence number above every other, into sectors no current copy
 * holds, so a write torn at any point leaves the previous copy current: nothing is half-written.
 * The owner tag: the store window is the stock firmware's VM, so after an install it may hold the stock firmware's
 * or another firmware's data (Felucca's too), which V-Track must never read as its own. The first sector holds the
 * tag "SMK37DATAVTRACK". Anything else there (another tag, or none): every sector of the window that is not
 * erased is erased, then the tag written.
 * Flash access goes through three hooks (storage_hw.c on the device; the host tests bring their own):
 *   st_read(off, dst, n)   st_erase(off)   st_prog(off, src, n) */
#if !BOARD_SMK37
#error "storage.c: the SMK-37 store window"
#endif
#define ST_MAGIC 0x4B525456u                   /* "VTRK" */
#define ST_SECTOR 4096u
#define ST_HEAD 32u                            /* the header; the part's bytes after it */
#define ST_PART (ST_SECTOR - ST_HEAD)          /* a part's bytes, at most */
#define ST_OWNER_OFF FL_DATA_LO
#define ST_POOL_LO (FL_DATA_LO + ST_SECTOR)
#define ST_POOL ((FL_DATA_HI - ST_POOL_LO) / ST_SECTOR)   /* 29 sectors */
#define ST_OBJECTS 5u                          /* store.c: the four slots, the autosave */
#define ST_FULL (-2)
_Static_assert(FL_WIN_SAFE(FL_DATA_LO, FL_DATA_HI) && ST_POOL == 29u, "the store window: the tag and the pool");

typedef struct {
    uint32_t magic;
    uint8_t obj, part, parts, rsv;
    uint32_t seq, len, total, crc, rsv2;
    uint32_t hcrc;
} st_hdr_t;
_Static_assert(sizeof(st_hdr_t) == ST_HEAD, "storage header layout");

static int st_read(uint32_t off, void *dst, uint32_t n);
static int st_erase(uint32_t off);
static int st_prog(uint32_t off, const void *src, uint32_t n);

static uint32_t st_crc32_add(uint32_t c, const void *p, uint32_t n)   /* zlib CRC-32, 4 bits per step; running:
                                                                         * from 0xFFFFFFFF, the CRC is ~c */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static uint32_t st_crc32(const void *p, uint32_t n) { return ~st_crc32_add(0xFFFFFFFFu, p, n); }

static struct {                                /* what each pool sector holds, from the scan */
    st_hdr_t h;
    uint8_t valid;                             /* a part: header and bytes intact */
    uint8_t erased;                            /* all 0xFF: written without an erase */
} st_sec[ST_POOL];
static uint32_t st_cur[ST_OBJECTS];            /* each object's current copy (its sequence number), 0: none */
static uint32_t st_seq;                        /* the highest sequence number in the pool */
static uint8_t st_owner_cleared;
static uint8_t st_ok;                          /* st_boot ran: the store may be written */
static uint8_t st_page[256] __attribute__((aligned(4)));   /* the driver wants RAM sources (main loop only) */

static uint32_t st_off(uint32_t s) { return ST_POOL_LO + s * ST_SECTOR; }

static int st_is_erased(uint32_t off, uint32_t n)   /* [off, off + n) all 0xFF */
{
    uint32_t k, i;
    for (; n; off += k, n -= k) {
        k = n > sizeof st_page ? sizeof st_page : n;
        if (st_read(off, st_page, k))
            return 0;
        for (i = 0; i < k; i++)
            if (st_page[i] != 0xFFu)
                return 0;
    }
    return 1;
}

static int st_part_ok(uint32_t s, const st_hdr_t *h)   /* the part's bytes match its CRC */
{
    uint32_t c = 0xFFFFFFFFu, k, off = st_off(s) + ST_HEAD, n = h->len;
    for (; n; off += k, n -= k) {
        k = n > sizeof st_page ? sizeof st_page : n;
        if (st_read(off, st_page, k))
            return 0;
        c = st_crc32_add(c, st_page, k);
    }
    return ~c == h->crc;
}

static void st_scan_sector(uint32_t s)
{
    st_hdr_t *h = &st_sec[s].h;
    st_sec[s].valid = st_sec[s].erased = 0;
    if (st_read(st_off(s), h, sizeof *h))
        return;
    if (h->magic == ST_MAGIC && h->hcrc == st_crc32(h, sizeof *h - 4u) && h->obj < ST_OBJECTS &&
        h->part < h->parts && h->len <= ST_PART && h->total <= (uint32_t)h->parts * ST_PART && st_part_ok(s, h))
        st_sec[s].valid = 1;
    else
        st_sec[s].erased = (uint8_t)st_is_erased(st_off(s), ST_SECTOR);
}

static int st_part_at(uint32_t obj, uint32_t seq, uint32_t part)   /* the sector of the copy's part, -1: none */
{
    uint32_t s;
    for (s = 0; s < ST_POOL; s++)
        if (st_sec[s].valid && st_sec[s].h.obj == obj && st_sec[s].h.seq == seq && st_sec[s].h.part == part)
            return (int)s;
    return -1;
}

static int st_complete(uint32_t obj, uint32_t seq, uint32_t parts, uint32_t total)
{
    uint32_t p, sum = 0;
    for (p = 0; p < parts; p++) {
        int s = st_part_at(obj, seq, p);
        if (s < 0 || st_sec[s].h.parts != parts || st_sec[s].h.total != total)
            return 0;
        sum += st_sec[s].h.len;
    }
    return sum == total;
}

static void st_current(void)                   /* each object's current copy, from st_sec */
{
    uint32_t s, o;
    st_seq = 0;
    for (o = 0; o < ST_OBJECTS; o++)
        st_cur[o] = 0;
    for (s = 0; s < ST_POOL; s++) {
        const st_hdr_t *h = &st_sec[s].h;
        if (!st_sec[s].valid)
            continue;
        if (h->seq > st_seq)
            st_seq = h->seq;
        if (h->seq > st_cur[h->obj] && st_complete(h->obj, h->seq, h->parts, h->total))
            st_cur[h->obj] = h->seq;
    }
}

static int st_holds(uint32_t s)                /* sector s is part of an object's current copy */
{
    return st_sec[s].valid && st_cur[st_sec[s].h.obj] == st_sec[s].h.seq;
}

static void st_owner_check(void)
{
    static const char TAG[16] = "SMK37DATAVTRACK";
    uint8_t tag[16];
    uint32_t off;
    if (st_read(ST_OWNER_OFF, tag, sizeof tag) == 0 && !memcmp(tag, TAG, sizeof tag))
        return;
    for (off = FL_DATA_LO; off < FL_DATA_HI; off += ST_SECTOR)
        if (!st_is_erased(off, ST_SECTOR)) {
            st_erase(off);
            st_owner_cleared = off != ST_OWNER_OFF || st_owner_cleared;
        }
    memcpy(st_page, TAG, sizeof TAG);
    st_prog(ST_OWNER_OFF, st_page, sizeof TAG);
}

static void st_boot(void)
{
    uint32_t s;
    st_owner_cleared = 0;
    st_owner_check();
    for (s = 0; s < ST_POOL; s++)
        st_scan_sector(s);
    st_current();
    st_ok = 1;
}

static uint32_t st_parts(uint32_t len) { return len ? (len + ST_PART - 1u) / ST_PART : 1u; }

static int st_size(uint32_t obj)
{
    int s;
    if (obj >= ST_OBJECTS || !st_cur[obj] || (s = st_part_at(obj, st_cur[obj], 0)) < 0)
        return -1;
    return (int)st_sec[s].h.total;
}

static uint32_t st_held(uint32_t obj)
{
    uint32_t s, n = 0;
    for (s = 0; s < ST_POOL; s++)
        n += st_holds(s) && st_sec[s].h.obj == obj;
    return n;
}

static uint32_t st_used(void)
{
    uint32_t s, n = 0;
    for (s = 0; s < ST_POOL; s++)
        n += (uint32_t)st_holds(s);
    return n;
}

static int st_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t p, at = 0;
    int len = st_size(obj);
    if (len < 0 || (uint32_t)len > max)
        return -1;
    for (p = 0; p < st_parts((uint32_t)len); p++) {
        int s = st_part_at(obj, st_cur[obj], p);
        st_hdr_t h;
        if (s < 0 || st_read(st_off((uint32_t)s), &h, sizeof h) || memcmp(&h, &st_sec[s].h, sizeof h) ||
            st_read(st_off((uint32_t)s) + ST_HEAD, (uint8_t *)dst + at, h.len) ||
            st_crc32((uint8_t *)dst + at, h.len) != h.crc)
            return -1;
        at += h.len;
    }
    return len;
}

static int st_room_erased(uint32_t len)
{
    uint32_t s, n = 0;
    for (s = 0; s < ST_POOL; s++)
        n += !st_holds(s) && st_sec[s].erased;
    return n >= st_parts(len);
}

static int st_tidy(void)
{
    uint32_t s;
    for (s = 0; s < ST_POOL && st_ok; s++)
        if (!st_holds(s) && !st_sec[s].erased) {
            st_sec[s].valid = 0;
            st_sec[s].erased = st_erase(st_off(s)) == 0 && st_is_erased(st_off(s), ST_SECTOR);
            return 1;
        }
    return 0;
}

static int st_write_part(uint32_t s, st_hdr_t *h, const uint8_t *src)   /* erase if needed, bytes, header last */
{
    uint32_t off = st_off(s), at, k;
    int rc;
    st_sec[s].valid = 0;
    if (!st_sec[s].erased && (rc = st_erase(off)) != 0)
        return rc;
    st_sec[s].erased = 0;
    for (at = 0; at < h->len; at += k) {       /* pages: the first one after the header */
        k = 256u - ((ST_HEAD + at) & 255u);
        k = k > h->len - at ? h->len - at : k;
        memcpy(st_page, src + at, k);
        if ((rc = st_prog(off + ST_HEAD + at, st_page, k)) != 0)
            return rc;
    }
    h->hcrc = st_crc32(h, sizeof *h - 4u);
    memcpy(st_page, h, sizeof *h);
    if ((rc = st_prog(off, st_page, sizeof *h)) != 0)   /* the header, last */
        return rc;
    st_scan_sector(s);                         /* read back: a failing part must not count as saved */
    return st_sec[s].valid && !memcmp(&st_sec[s].h, h, sizeof *h) ? 0 : -7;
}

static int st_save(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t parts = st_parts(len), free = 0, s, p, at = 0;
    int rc = 0;
    if (obj >= ST_OBJECTS || !st_ok)
        return -1;
    for (s = 0; s < ST_POOL; s++)
        free += !st_holds(s);
    if (parts > free || len > parts * ST_PART)
        return ST_FULL;
    st_seq++;
    for (p = 0; p < parts && !rc; p++) {
        st_hdr_t h;
        uint32_t best = ST_POOL;
        for (s = 0; s < ST_POOL; s++)          /* a free sector, erased ones first */
            if (!st_holds(s) && !(st_sec[s].valid && st_sec[s].h.obj == obj && st_sec[s].h.seq == st_seq) &&
                (best == ST_POOL || (st_sec[s].erased && !st_sec[best].erased)))
                best = s;
        memset(&h, 0, sizeof h);
        h.magic = ST_MAGIC;
        h.obj = (uint8_t)obj;
        h.part = (uint8_t)p;
        h.parts = (uint8_t)parts;
        h.rsv = 0xFFu;
        h.rsv2 = 0xFFFFFFFFu;
        h.seq = st_seq;
        h.len = len - at > ST_PART ? ST_PART : len - at;
        h.total = len;
        h.crc = st_crc32((const uint8_t *)src + at, h.len);
        rc = best == ST_POOL ? -1 : st_write_part(best, &h, (const uint8_t *)src + at);
        at += h.len;
    }
    st_current();
    return rc ? rc : st_cur[obj] == st_seq ? 0 : -7;
}
