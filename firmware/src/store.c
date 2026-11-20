/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Project storage: four project slots and the autosave of the current project, each an object of the store
 * (storage.c) holding a project in the stored format (project.c). app.c calls it:
 *   store_boot()                power-on, before the player starts: the store checked (another firmware's data
 *                               cleared), the autosave loaded as the project (none, or one this version cannot
 *                               read: a new project)
 *   store_service(now, busy)    each main-loop frame (now: milliseconds; busy: the sequencer plays or a key is
 *                               held, so the audio must not stop): the autosave, below
 *   store_save(slot)            the project into slot 0..3: STORE_OK; STORE_FULL if it does not fit (nothing
 *                               written: the slot keeps what it held); STORE_FAILED if the flash failed (the same)
 *   store_load(slot)            the slot's project becomes the project: STORE_OK; STORE_EMPTY; STORE_FAILED (not
 *                               one this version can read: the project stays). The player should be stopped
 *   store_new()                 a new project
 *   store_used(slot), store_name(slot)   whether the slot holds a project; its name
 *   store_percent()             how full the project memory is, 0..100 (every slot's and the autosave's copy)
 *   store_autosave_full()       1 if the project changed and its autosave did not fit
 * The autosave: twice a second the project's stored form is compared with what was autosaved; once it has stayed
 * the same for STORE_QUIET_MS after a change it is autosaved, so a power cut loses at most the last few seconds.
 * An erase stops the audio for some 40 ms (storage_hw.c), so while the sequencer plays the autosave waits for
 * sectors erased ahead; while it is stopped and no key is held the store erases them ahead, one each STORE_TIDY_MS. */
#if !FELUCCA_FLASH
#error "store.c needs FELUCCA_FLASH (storage.c)"
#endif
#define STORE_SLOTS 4u
#define STORE_AUTOSAVE 4u                /* the autosave's object (storage.c: objects 0..3 the slots) */
#define STORE_OK 0
#define STORE_FULL 1
#define STORE_FAILED 2
#define STORE_EMPTY 3
#define STORE_CHECK_MS 500u
#define STORE_QUIET_MS 1500u
#define STORE_TIDY_MS 1000u
_Static_assert(STORE_AUTOSAVE < ST_OBJECTS && STORE_SLOTS <= STORE_AUTOSAVE, "store: the objects");
_Static_assert(PROJECT_STORE_MAX <= ST_POOL * ST_PART, "store: the biggest project fits into an empty pool");

static uint8_t store_buf[PROJECT_STORE_MAX] __attribute__((aligned(4), section(".pool")));   /* (main loop only) */

static struct {
    char name[STORE_SLOTS][PROJECT_NAME];   /* each slot's project's name; "" if it holds none */
    uint32_t check_at, tidy_at;
    uint32_t seen, seen_at;              /* the project's stored form (CRC) at the last check, since when */
    uint32_t saved;                      /* .. autosaved (or loaded from the autosave) */
    uint32_t refused;                    /* .. whose autosave failed (not tried again until it changes) */
    uint8_t saved_ok, refused_ok, full;
} store;

static uint32_t store_form(uint32_t *len)   /* the project in the stored format into store_buf: its CRC */
{
    *len = project_save(store_buf, sizeof store_buf);
    return st_crc32(store_buf, *len);
}

static void store_read_name(uint32_t slot)   /* the slot's project's name, from the store ("": none, or a project
                                              * this version cannot read) */
{
    int n = st_load(slot, store_buf, sizeof store_buf);
    if (n < 0 || !project_stored_name(store_buf, (uint32_t)n, store.name[slot]))
        store.name[slot][0] = 0;
    else if (!store.name[slot][0])
        store.name[slot][0] = '-';       /* (a project with no name: the slot is still in use) */
}

static void store_boot(void)
{
    uint32_t s, len;
    int n;
    if (flash_ok)                        /* (main.c flash_boot: the expected part answered) */
        st_boot();
    for (s = 0; s < STORE_SLOTS; s++)
        store_read_name(s);
    n = st_load(STORE_AUTOSAVE, store_buf, sizeof store_buf);
    if (n < 0 || !project_load(store_buf, (uint32_t)n))
        project_init();
    store.saved = store.seen = store_form(&len);   /* what is in RAM now is what is kept */
    store.saved_ok = 1;
    store.refused_ok = store.full = 0;
    store.check_at = store.seen_at = store.tidy_at = fm1_ms;
}

static void store_changed(void)          /* the project was replaced: autosave it */
{
    store.saved_ok = 0;
    store.check_at = fm1_ms - STORE_CHECK_MS;
}

static int store_save(uint32_t slot)
{
    uint32_t len;
    int rc;
    if (slot >= STORE_SLOTS)
        return STORE_FAILED;
    store_form(&len);
    rc = len ? st_save(slot, store_buf, len) : -1;
    store_read_name(slot);
    return rc == 0 ? STORE_OK : rc == ST_FULL ? STORE_FULL : STORE_FAILED;
}

static int store_load(uint32_t slot)
{
    int n;
    if (slot >= STORE_SLOTS)
        return STORE_FAILED;
    n = st_load(slot, store_buf, sizeof store_buf);
    if (n < 0)
        return st_size(slot) < 0 ? STORE_EMPTY : STORE_FAILED;
    if (!project_load(store_buf, (uint32_t)n))
        return STORE_FAILED;
    store_changed();
    return STORE_OK;
}

static void store_new(void)
{
    project_init();
    store_changed();
}

static int store_used(uint32_t slot) { return slot < STORE_SLOTS && store.name[slot][0] != 0; }
static const char *store_name(uint32_t slot) { return slot < STORE_SLOTS ? store.name[slot] : ""; }
static uint32_t store_percent(void) { return (st_used() * 100u + ST_POOL - 1u) / ST_POOL; }
static int store_autosave_full(void) { return store.full; }

static void store_service(uint32_t now, int busy)
{
    uint32_t len, c;
    int rc;
    if (now - store.check_at >= STORE_CHECK_MS) {
        store.check_at = now;
        c = store_form(&len);
        if (c != store.seen)
            store.seen = c, store.seen_at = now;
    }
    if ((store.saved_ok && store.seen == store.saved) || (store.refused_ok && store.seen == store.refused)) {
        if (!busy && now - store.tidy_at >= STORE_TIDY_MS) {   /* nothing to autosave: erase ahead */
            store.tidy_at = now;
            st_tidy();
        }
        return;
    }
    if (now - store.seen_at < STORE_QUIET_MS)
        return;
    c = store_form(&len);
    if (c != store.seen) {               /* (changed since the check: wait until it is quiet) */
        store.seen = c, store.seen_at = now;
        return;
    }
    if (busy && !st_room_erased(len))
        return;
    rc = len ? st_save(STORE_AUTOSAVE, store_buf, len) : -1;
    if (rc == 0) {
        store.saved = c;
        store.saved_ok = 1;
        store.full = 0;
    } else {
        store.refused = c;
        store.refused_ok = 1;
        store.full = rc == ST_FULL;
    }
}
