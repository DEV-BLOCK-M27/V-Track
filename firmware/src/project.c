/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The project model: what a piece of music is, as operations on it. The app edits it, the player (in the audio ISR)
 * reads it:
 *   project_init()                      a new project: every pattern empty, PROJECT_ROWS_NEW rows, 120 BPM; an order
 *                                       list of one entry, P00 on each track
 *   project_orders()                    the order list's entries, 1 .. PROJECT_ORDERS
 *   project_order(e, track)             the pattern entry e names for track 0..3 (0 past the end)
 *   project_set_order(e, track, p)      sets it, kept within 0 .. PROJECT_PATTERNS - 1
 *   project_entry(), project_set_entry(e)   the current entry (the order list's cursor), kept within the entries;
 *                                       the pattern calls below mean its patterns
 *   project_order_add(e)                a new entry after e: each track's first pattern no entry names (none left:
 *                                       e's); 1 if done, 0 if the list is full (PROJECT_ORDERS)
 *   project_order_dup(e)                a copy of e after it; 1 / 0 as above
 *   project_order_delete(e)             e taken out (not the last one left: 0); the entries after it a place up, the
 *                                       current entry stays on its entry (it, deleted: the one after, or before at
 *                                       the end)
 *   project_order_move(e, dir)          e moved dir places (+ down), within the list; the entries between move the
 *                                       other way. Its new place
 *   project_rows_of(track, p), project_field_of(track, p, row, f)   pattern p's length, a field of it (as below)
 *   project_pattern(track)              the pattern track 0..3 edits: the current entry's, 0 .. PROJECT_PATTERNS - 1;
 *                                       every call below means that pattern
 *   project_set_pattern(track, p)       the current entry names p for the track, kept within 0 .. PROJECT_PATTERNS - 1
 *   project_rows(track)                 the length of track 0..3's pattern in rows
 *   project_set_rows(track, n)          sets it, kept within PROJECT_ROWS_MIN .. PROJECT_ROWS (rows past it keep
 *                                       what they hold, for when it grows again)
 *   project_field(track, row, f)        a field of a row (PROJECT_F_*), PROJECT_EMPTY if it is empty (no change):
 *                                         NOTE:   PROJECT_NOTE(MIDI number), PROJECT_CUT or PROJECT_RELEASE
 *                                         INS:    PROJECT_INS(instrument 0..15)
 *                                         VOL:    PROJECT_VOL(volume 0..15)
 *                                         EFFECT: PROJECT_EFFECT(letter, parameter 0..255), the letter one of
 *                                                 PROJECT_EFFECT_LETTERS
 *   project_set_field(track, row, f, v) writes it (anything else is refused: the row stays as it was)
 *   project_set_field_of(track, p, row, f, v)   .. into pattern p of the track
 *   project_clear_row(track, row)       every field of the row empty (an empty row: no edit, no undo step)
 *   project_insert_row(track, row)      an empty row there, the rows below a row down (the last one falls off)
 *   project_delete_row(track, row)      the row taken out, the rows below a row up (the last one empty)
 *   project_copy(track)                 the track's pattern (its rows and length) copied
 *   project_paste(track)                .. into the track's pattern: an identical one; 1 if done, 0 if none copied
 *   project_checkpoint(track), project_checkpoint_of(track, p)   the undo step: the pattern as it is now, before
 *                                       an edit (clear, insert, delete and paste take it themselves)
 *   project_undo()                      the pattern of the undo step back as it was (its rows and length); 1 if
 *                                       done, 0 if there is none (one level: an undo uses it up). A new project
 *                                       has none; the copied pattern stays
 *   project_bpm(), project_set_bpm(b)   the tempo, one row a sixteenth; kept within PROJECT_BPM_MIN .. MAX
 *   project_ins_chip(i)                 the chip instrument 0..15 belongs to (ADR 0002), PROJECT_CHIP_NONE: a free slot
 *   project_ins_name(i)                 its name ("" for a free slot)
 *   project_set_ins_name(i, name)       renames it: up to 7 characters, trailing spaces dropped
 *   project_ins_new(i, chip, name)      slot i becomes an instrument of the chip, every sequence empty (its steps 0)
 *   project_ins_copy(from, to)          instrument from (not free) copied to slot to (free): the same chip, name and
 *                                       sequences; 1 if done, else 0 (nothing changed)
 *   project_seq(i, s)                   its sequence s (PROJECT_SEQ_*), read-only: len steps (0 = none, 64 at most),
 *                                       v[0 .. len-1], loop and release a step or PROJECT_SEQ_NONE
 *   project_set_seq(i, s, v, n, loop, release)  sets it (n kept to 64; a loop or release past the end: none)
 *   project_set_step(i, s, k, v)        step k (0..63) of it to v, kept within PROJECT_SEQ_MIN .. MAX[s]; a step
 *                                       past its end lengthens it to k + 1 (the steps between keep what they hold)
 *   project_set_len(i, s, n)            its length, kept within 0 .. 64 (steps past it keep what they hold; a loop
 *                                       or release point past it: none)
 *   project_set_loop(i, s, k), project_set_release(i, s, k)   its loop / release point: a step within its length
 *                                       or PROJECT_SEQ_NONE (anything else is refused)
 * An NES instrument's sequences: volume 0..15, arpeggio (semitones added to the note), pitch (added to the period
 * each frame: + lower), duty 0..3 (a pulse) or mode 0..1 (the noise). A new project has the factory instruments 00
 * LEAD, 01 BASS, 02 KICK (the triangle), 03 SNARE and 04 HAT (the noise); 05 to 0F are free.
 * Each field is one byte (the effect: one half-word), so the player reads it whole while the app writes it. An
 * entry's patterns are a byte each too: a row starting while the order list changes may take some of its tracks'
 * patterns from the entry before the change and some from after it (that row only). */
#define PROJECT_TRACKS 4u
#define PROJECT_ROWS 64u                 /* a pattern's room, its longest */
#define PROJECT_ROWS_MIN 16u
#define PROJECT_PATTERNS 32u             /* a track's patterns */
#define PROJECT_ORDERS 64u               /* the order list's entries, at most */
#define PROJECT_ROWS_NEW 16u             /* a new pattern's length: one page, all of it on the pads */
#define PROJECT_BPM_MIN 30u
#define PROJECT_BPM_MAX 300u
#define PROJECT_EMPTY 0u                 /* no change */
#define PROJECT_NOTE(m) ((m) + 1u)       /* MIDI 0..127 -> 1..128 */
#define PROJECT_RELEASE 0xFEu            /* === */
#define PROJECT_CUT 0xFFu                /* --- */
#define PROJECT_IS_NOTE(v) ((v) >= 1u && (v) <= 128u)
#define PROJECT_INS(i) ((i) + 1u)        /* instrument 0..15 -> 1..16 */
#define PROJECT_INSTRUMENTS 16u
#define PROJECT_VOL(v) ((v) + 1u)        /* volume 0..15 -> 1..16 */
#define PROJECT_VOLUMES 16u
#define PROJECT_EFFECT(c, p) ((uint32_t)(c) << 8 | (p))
#define PROJECT_EFFECT_LETTERS "01234ABDFGSV" /* FamiTracker's: arpeggio, slides, portamento, vibrato, volume slide,
                                            jump, skip, tempo, delay, cut, duty */
#define PROJECT_F_NOTE 0u
#define PROJECT_F_INS 1u
#define PROJECT_F_VOL 2u
#define PROJECT_F_EFFECT 3u
#define PROJECT_CHIP_NONE 0u             /* a free instrument slot */
#define PROJECT_CHIP_NES 1u
#define PROJECT_SEQ_VOL 0u               /* an instrument's sequences */
#define PROJECT_SEQ_ARP 1u
#define PROJECT_SEQ_PITCH 2u
#define PROJECT_SEQ_DUTY 3u              /* duty (pulse) or mode (noise) */
#define PROJECT_SEQS 4u
#define PROJECT_SEQ_STEPS 64u
#define PROJECT_SEQ_NONE 0xFFu           /* no loop / release point */
#define PROJECT_NAME 8u                  /* an instrument's name: up to 7 characters */

#define PROJECT_ORDER() __asm__ volatile("" ::: "memory")   /* the stores before it land before those after (the
                                                            * player in the audio ISR relies on the order) */

/* each sequence's values: volume 0..15, arpeggio -24..24 semitones, pitch -127..127, duty 0..3 (mode 0..1) */
static const int8_t PROJECT_SEQ_MIN[PROJECT_SEQS] = {0, -24, -127, 0},
                    PROJECT_SEQ_MAX[PROJECT_SEQS] = {15, 24, 127, 3};

typedef struct {
    uint8_t len, loop, release;
    int8_t v[PROJECT_SEQ_STEPS];
} project_seq_t;

typedef struct {
    uint8_t chip;
    char name[PROJECT_NAME];
    project_seq_t seq[PROJECT_SEQS];
} project_ins_t;

static struct {                          /* the patterns: 40 KiB, in the pool (gfx.c's cv_px is there too) */
    volatile uint8_t note[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS],
        ins[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS], vol[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS];
    volatile uint16_t effect[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS];
    volatile uint8_t rows[PROJECT_TRACKS][PROJECT_PATTERNS];   /* their lengths */
    project_ins_t ins_def[PROJECT_INSTRUMENTS];                /* the instruments: 4.4 KiB */
} project_pat __attribute__((section(".pool")));

static struct {
    volatile uint8_t order[PROJECT_ORDERS][PROJECT_TRACKS];   /* the order list: each entry's pattern a track */
    volatile uint8_t orders;             /* its entries, 1 .. PROJECT_ORDERS */
    volatile uint8_t entry;              /* the current entry, 0 .. orders - 1 */
    volatile uint16_t bpm;
} project;

typedef struct {                         /* one pattern's rows: the undo step, the copied pattern */
    uint8_t valid, track, p, rows;
    uint8_t note[PROJECT_ROWS], ins[PROJECT_ROWS], vol[PROJECT_ROWS];
    uint16_t effect[PROJECT_ROWS];
} project_snap_t;

static project_snap_t project_undo_snap, project_copied;

static uint32_t project_ins_chip(uint32_t i)
{
    return i < PROJECT_INSTRUMENTS ? project_pat.ins_def[i].chip : PROJECT_CHIP_NONE;
}

static const char *project_ins_name(uint32_t i) { return i < PROJECT_INSTRUMENTS ? project_pat.ins_def[i].name : ""; }

static void project_set_ins_name(uint32_t i, const char *name)
{
    char *d;
    uint32_t k;
    if (i >= PROJECT_INSTRUMENTS)
        return;
    d = project_pat.ins_def[i].name;
    for (k = 0; k + 1u < PROJECT_NAME && name[k]; k++)
        d[k] = name[k];
    for (; k < PROJECT_NAME; k++)
        d[k] = 0;
    for (k = PROJECT_NAME - 1u; k-- > 0 && d[k] == ' ';)
        d[k] = 0;
}

static void project_ins_new(uint32_t i, uint32_t chip, const char *name)
{
    project_ins_t *d;
    uint32_t s, k;
    if (i >= PROJECT_INSTRUMENTS)
        return;
    d = &project_pat.ins_def[i];
    d->chip = (uint8_t)chip;
    project_set_ins_name(i, name);
    for (s = 0; s < PROJECT_SEQS; s++) {
        d->seq[s].len = 0, d->seq[s].loop = d->seq[s].release = PROJECT_SEQ_NONE;
        for (k = 0; k < PROJECT_SEQ_STEPS; k++)
            d->seq[s].v[k] = 0;
    }
}

static int project_ins_copy(uint32_t from, uint32_t to)
{
    if (from >= PROJECT_INSTRUMENTS || to >= PROJECT_INSTRUMENTS || project_ins_chip(from) == PROJECT_CHIP_NONE ||
        project_ins_chip(to) != PROJECT_CHIP_NONE)
        return 0;
    memcpy(project_pat.ins_def[to].name, project_pat.ins_def[from].name, sizeof project_pat.ins_def[to].name);
    memcpy(project_pat.ins_def[to].seq, project_pat.ins_def[from].seq, sizeof project_pat.ins_def[to].seq);
    PROJECT_ORDER();
    project_pat.ins_def[to].chip = project_pat.ins_def[from].chip;   /* last: the player plays it from now on */
    return 1;
}

static const project_seq_t *project_seq(uint32_t i, uint32_t s)
{
    return &project_pat.ins_def[i < PROJECT_INSTRUMENTS ? i : 0u].seq[s < PROJECT_SEQS ? s : 0u];
}

static void project_set_seq(uint32_t i, uint32_t s, const int8_t *v, uint32_t n, uint32_t loop, uint32_t release)
{
    project_seq_t *q;
    uint32_t k;
    if (i >= PROJECT_INSTRUMENTS || s >= PROJECT_SEQS)
        return;
    q = &project_pat.ins_def[i].seq[s];
    n = n > PROJECT_SEQ_STEPS ? PROJECT_SEQ_STEPS : n;
    q->len = 0;                          /* (the player sees no steps while they change) */
    for (k = 0; k < n; k++)
        q->v[k] = v[k];
    q->loop = (uint8_t)(loop < n ? loop : PROJECT_SEQ_NONE);
    q->release = (uint8_t)(release < n ? release : PROJECT_SEQ_NONE);
    q->len = (uint8_t)n;
}

static void project_set_step(uint32_t i, uint32_t s, uint32_t k, int32_t v)
{
    project_seq_t *q;
    if (i >= PROJECT_INSTRUMENTS || s >= PROJECT_SEQS || k >= PROJECT_SEQ_STEPS)
        return;
    q = &project_pat.ins_def[i].seq[s];
    q->v[k] = (int8_t)(v < PROJECT_SEQ_MIN[s] ? PROJECT_SEQ_MIN[s] : v > PROJECT_SEQ_MAX[s] ? PROJECT_SEQ_MAX[s] : v);
    PROJECT_ORDER();
    if (k >= q->len)                     /* (one byte: the player sees the old length or the new) */
        q->len = (uint8_t)(k + 1u);
}

static void project_set_len(uint32_t i, uint32_t s, int32_t n)
{
    project_seq_t *q;
    if (i >= PROJECT_INSTRUMENTS || s >= PROJECT_SEQS)
        return;
    q = &project_pat.ins_def[i].seq[s];
    n = n < 0 ? 0 : n > (int32_t)PROJECT_SEQ_STEPS ? (int32_t)PROJECT_SEQ_STEPS : n;
    if (q->loop != PROJECT_SEQ_NONE && q->loop >= n)   /* (the points first: the player never sees one past it) */
        q->loop = PROJECT_SEQ_NONE;
    if (q->release != PROJECT_SEQ_NONE && q->release >= n)
        q->release = PROJECT_SEQ_NONE;
    PROJECT_ORDER();
    q->len = (uint8_t)n;
}

static void project_set_point(uint32_t i, uint32_t s, uint32_t k, uint32_t release)
{
    project_seq_t *q;
    if (i >= PROJECT_INSTRUMENTS || s >= PROJECT_SEQS)
        return;
    q = &project_pat.ins_def[i].seq[s];
    if (k != PROJECT_SEQ_NONE && k >= q->len)
        return;
    if (release)
        q->release = (uint8_t)k;
    else
        q->loop = (uint8_t)k;
}
static void project_set_loop(uint32_t i, uint32_t s, uint32_t k) { project_set_point(i, s, k, 0); }
static void project_set_release(uint32_t i, uint32_t s, uint32_t k) { project_set_point(i, s, k, 1); }

/* the factory instruments: name; volume, arpeggio, pitch, duty sequences (n, loop, release, steps) */
#define PROJECT_FACTORY 5u
#define NO PROJECT_SEQ_NONE
static const struct {
    char name[PROJECT_NAME];
    struct { uint8_t n, loop, release; int8_t v[16]; } seq[PROJECT_SEQS];
} PROJECT_FACTORY_INS[PROJECT_FACTORY] = {
    {"LEAD", {{13, NO, 7, {15, 14, 13, 12, 11, 10, 9, 8, 7, 5, 3, 1, 0}}, {0}, {0}, {3, NO, NO, {1, 1, 2}}}},
    {"BASS", {{4, NO, 1, {15, 13, 6, 0}}, {0}, {0}, {1, NO, NO, {1}}}},
    {"KICK", {{8, NO, NO, {15, 15, 14, 12, 10, 6, 3, 0}}, {0}, {8, NO, NO, {0, 32, 48, 64, 80, 96, 112, 120}},
              {0}}},
    {"SNARE", {{10, NO, NO, {15, 13, 11, 9, 7, 6, 4, 3, 1, 0}}, {0}, {0}, {1, NO, NO, {0}}}},
    {"HAT", {{5, NO, NO, {10, 7, 4, 2, 0}}, {0}, {0}, {1, NO, NO, {0}}}},
};

#undef NO

static void project_init(void)
{
    uint32_t t, p, r;
    for (t = 0; t < PROJECT_INSTRUMENTS; t++)
        project_ins_new(t, PROJECT_CHIP_NONE, "");
    for (t = 0; t < PROJECT_FACTORY; t++) {
        project_ins_new(t, PROJECT_CHIP_NES, PROJECT_FACTORY_INS[t].name);
        for (p = 0; p < PROJECT_SEQS; p++)
            project_set_seq(t, p, PROJECT_FACTORY_INS[t].seq[p].v, PROJECT_FACTORY_INS[t].seq[p].n,
                            PROJECT_FACTORY_INS[t].seq[p].loop, PROJECT_FACTORY_INS[t].seq[p].release);
    }
    for (t = 0; t < PROJECT_TRACKS; t++) {
        for (p = 0; p < PROJECT_PATTERNS; p++) {
            for (r = 0; r < PROJECT_ROWS; r++) {
                project_pat.note[t][p][r] = project_pat.ins[t][p][r] = project_pat.vol[t][p][r] = PROJECT_EMPTY;
                project_pat.effect[t][p][r] = PROJECT_EMPTY;
            }
            project_pat.rows[t][p] = PROJECT_ROWS_NEW;
        }
        project.order[0][t] = 0;
    }
    project.orders = 1;
    project.entry = 0;
    project.bpm = 120;
    project_undo_snap.valid = 0;         /* (the copied pattern stays: it can go into another project) */
}

static uint32_t project_clamp(int32_t v, uint32_t n)   /* v within 0 .. n - 1 */
{
    return v < 0 ? 0u : v >= (int32_t)n ? n - 1u : (uint32_t)v;
}

/* --- the order list --- */

static uint32_t project_orders(void) { return project.orders; }
static uint32_t project_entry(void) { return project.entry; }
static void project_set_entry(int32_t e) { project.entry = (uint8_t)project_clamp(e, project.orders); }

static uint32_t project_order(uint32_t e, uint32_t track)
{
    return e < project.orders && track < PROJECT_TRACKS ? project.order[e][track] : 0u;
}

static void project_set_order(uint32_t e, uint32_t track, int32_t p)
{
    if (e < project.orders && track < PROJECT_TRACKS)
        project.order[e][track] = (uint8_t)project_clamp(p, PROJECT_PATTERNS);
}

static int project_order_insert(uint32_t e, const uint8_t *pat)   /* pat[] as a new entry after e */
{
    uint32_t k, t, n = project.orders;
    if (e >= n || n >= PROJECT_ORDERS)
        return 0;
    for (k = n; k > e + 1u; k--)        /* the entries after it a place on, the last first */
        for (t = 0; t < PROJECT_TRACKS; t++)
            project.order[k][t] = project.order[k - 1u][t];
    for (t = 0; t < PROJECT_TRACKS; t++)
        project.order[e + 1u][t] = pat[t];
    PROJECT_ORDER();
    project.orders = (uint8_t)(n + 1u);
    return 1;
}

static int project_order_add(uint32_t e)
{
    uint8_t pat[PROJECT_TRACKS];
    uint32_t t, p, k;
    if (e >= project.orders)
        return 0;
    for (t = 0; t < PROJECT_TRACKS; t++) {   /* each track's first pattern no entry names */
        pat[t] = project.order[e][t];
        for (p = 0; p < PROJECT_PATTERNS; p++) {
            for (k = 0; k < project.orders && project.order[k][t] != p; k++)
                ;
            if (k == project.orders) {
                pat[t] = (uint8_t)p;
                break;
            }
        }
    }
    return project_order_insert(e, pat);
}

static int project_order_dup(uint32_t e)
{
    uint8_t pat[PROJECT_TRACKS];
    uint32_t t;
    if (e >= project.orders)
        return 0;
    for (t = 0; t < PROJECT_TRACKS; t++)
        pat[t] = project.order[e][t];
    return project_order_insert(e, pat);
}

static int project_order_delete(uint32_t e)
{
    uint32_t k, t, n = project.orders;
    if (e >= n || n < 2u)
        return 0;
    if (project.entry > e || project.entry >= n - 1u)   /* (first: it stays within the entries) */
        project.entry = (uint8_t)(project.entry - 1u);
    PROJECT_ORDER();
    project.orders = (uint8_t)(n - 1u);   /* (then: the player never reads past the end) */
    PROJECT_ORDER();
    for (k = e; k + 1u < n; k++)
        for (t = 0; t < PROJECT_TRACKS; t++)
            project.order[k][t] = project.order[k + 1u][t];
    return 1;
}

static uint32_t project_order_move(uint32_t e, int32_t dir)
{
    uint32_t to, t;
    if (e >= project.orders)
        return e;
    to = project_clamp((int32_t)e + dir, project.orders);
    while (e != to) {                    /* a place at a time: the entries between move the other way */
        uint32_t k = to > e ? e + 1u : e - 1u;
        for (t = 0; t < PROJECT_TRACKS; t++) {
            uint8_t p = project.order[e][t];
            project.order[e][t] = project.order[k][t];
            project.order[k][t] = p;
        }
        e = k;
    }
    return to;
}

/* --- the patterns --- */

static uint32_t project_pattern(uint32_t track) { return project_order(project.entry, track); }

static void project_set_pattern(uint32_t track, int32_t p) { project_set_order(project.entry, track, p); }

static uint32_t project_rows_of(uint32_t track, uint32_t p)
{
    return track < PROJECT_TRACKS && p < PROJECT_PATTERNS ? project_pat.rows[track][p] : 0u;
}

static uint32_t project_rows(uint32_t track) { return project_rows_of(track, project_pattern(track)); }

static void project_set_rows(uint32_t track, int32_t n)
{
    if (track < PROJECT_TRACKS)
        project_pat.rows[track][project_pattern(track)] =
            (uint8_t)(n < (int32_t)PROJECT_ROWS_MIN ? PROJECT_ROWS_MIN
                      : n > (int32_t)PROJECT_ROWS   ? PROJECT_ROWS
                                                    : (uint32_t)n);
}

static uint32_t project_field_of(uint32_t track, uint32_t p, uint32_t row, uint32_t f)
{
    if (row >= project_rows_of(track, p))
        return PROJECT_EMPTY;
    return f == PROJECT_F_NOTE     ? project_pat.note[track][p][row]
           : f == PROJECT_F_INS    ? project_pat.ins[track][p][row]
           : f == PROJECT_F_VOL    ? project_pat.vol[track][p][row]
           : f == PROJECT_F_EFFECT ? project_pat.effect[track][p][row]
                                   : PROJECT_EMPTY;
}

static uint32_t project_field(uint32_t track, uint32_t row, uint32_t f)
{
    return project_field_of(track, project_pattern(track), row, f);
}

static int project_effect_ok(uint32_t v)     /* an effect: a letter of PROJECT_EFFECT_LETTERS, a parameter */
{
    const char *c;
    for (c = PROJECT_EFFECT_LETTERS; *c; c++)
        if ((uint32_t)(uint8_t)*c == v >> 8)
            return v >> 16 == 0;
    return 0;
}

static void project_set_field_of(uint32_t track, uint32_t p, uint32_t row, uint32_t f, uint32_t v)
{
    if (row >= project_rows_of(track, p))
        return;
    if (f == PROJECT_F_NOTE && (v == PROJECT_EMPTY || PROJECT_IS_NOTE(v) || v == PROJECT_CUT || v == PROJECT_RELEASE))
        project_pat.note[track][p][row] = (uint8_t)v;
    if (f == PROJECT_F_INS && v <= PROJECT_INS(PROJECT_INSTRUMENTS - 1u))
        project_pat.ins[track][p][row] = (uint8_t)v;
    if (f == PROJECT_F_VOL && v <= PROJECT_VOL(PROJECT_VOLUMES - 1u))
        project_pat.vol[track][p][row] = (uint8_t)v;
    if (f == PROJECT_F_EFFECT && (v == PROJECT_EMPTY || project_effect_ok(v)))
        project_pat.effect[track][p][row] = (uint16_t)v;
}

static void project_set_field(uint32_t track, uint32_t row, uint32_t f, uint32_t v)
{
    project_set_field_of(track, project_pattern(track), row, f, v);
}

/* --- editing: rows, copy and paste, undo --- */

static void project_snapshot(project_snap_t *d, uint32_t track, uint32_t p)   /* pattern p of track into d */
{
    uint32_t r;
    d->track = (uint8_t)track, d->p = (uint8_t)p, d->rows = project_pat.rows[track][p];
    for (r = 0; r < PROJECT_ROWS; r++) {
        d->note[r] = project_pat.note[track][p][r], d->ins[r] = project_pat.ins[track][p][r];
        d->vol[r] = project_pat.vol[track][p][r], d->effect[r] = project_pat.effect[track][p][r];
    }
    d->valid = 1;
}

static void project_restore(const project_snap_t *s, uint32_t track, uint32_t p)   /* s into pattern p of track */
{
    uint32_t r;
    for (r = 0; r < PROJECT_ROWS; r++) {
        project_pat.note[track][p][r] = s->note[r], project_pat.ins[track][p][r] = s->ins[r];
        project_pat.vol[track][p][r] = s->vol[r], project_pat.effect[track][p][r] = s->effect[r];
    }
    project_pat.rows[track][p] = s->rows;
}

static void project_checkpoint_of(uint32_t track, uint32_t p)
{
    if (track < PROJECT_TRACKS && p < PROJECT_PATTERNS)
        project_snapshot(&project_undo_snap, track, p);
}

static void project_checkpoint(uint32_t track) { project_checkpoint_of(track, project_pattern(track)); }

static int project_undo(void)
{
    if (!project_undo_snap.valid)
        return 0;
    project_restore(&project_undo_snap, project_undo_snap.track, project_undo_snap.p);
    project_undo_snap.valid = 0;
    return 1;
}

static void project_row_copy(uint32_t track, uint32_t p, uint32_t to, uint32_t from)   /* row from into row to */
{
    project_pat.note[track][p][to] = project_pat.note[track][p][from];
    project_pat.ins[track][p][to] = project_pat.ins[track][p][from];
    project_pat.vol[track][p][to] = project_pat.vol[track][p][from];
    project_pat.effect[track][p][to] = project_pat.effect[track][p][from];
}

static void project_row_empty(uint32_t track, uint32_t p, uint32_t row)
{
    project_pat.note[track][p][row] = project_pat.ins[track][p][row] = project_pat.vol[track][p][row] = PROJECT_EMPTY;
    project_pat.effect[track][p][row] = PROJECT_EMPTY;
}

static void project_clear_row(uint32_t track, uint32_t row)
{
    uint32_t f;
    for (f = PROJECT_F_NOTE; f <= PROJECT_F_EFFECT && project_field(track, row, f) == PROJECT_EMPTY; f++)
        ;
    if (f > PROJECT_F_EFFECT)
        return;                          /* empty already (or past the end): no edit, the undo step kept */
    project_checkpoint(track);
    project_row_empty(track, project_pattern(track), row);
}

static void project_insert_row(uint32_t track, uint32_t row)
{
    uint32_t p, n = project_rows(track), r;
    if (row >= n)
        return;
    project_checkpoint(track);
    p = project_pattern(track);
    for (r = n - 1u; r > row; r--)
        project_row_copy(track, p, r, r - 1u);
    project_row_empty(track, p, row);
}

static void project_delete_row(uint32_t track, uint32_t row)
{
    uint32_t p, n = project_rows(track), r;
    if (row >= n)
        return;
    project_checkpoint(track);
    p = project_pattern(track);
    for (r = row; r + 1u < n; r++)
        project_row_copy(track, p, r, r + 1u);
    project_row_empty(track, p, n - 1u);
}

static void project_copy(uint32_t track)
{
    if (track < PROJECT_TRACKS)
        project_snapshot(&project_copied, track, project_pattern(track));
}

static int project_paste(uint32_t track)
{
    if (track >= PROJECT_TRACKS || !project_copied.valid)
        return 0;
    project_checkpoint(track);
    project_restore(&project_copied, track, project_pattern(track));
    return 1;
}

static uint32_t project_bpm(void) { return project.bpm; }

static void project_set_bpm(int32_t bpm)
{
    project.bpm = (uint16_t)(bpm < (int32_t)PROJECT_BPM_MIN   ? PROJECT_BPM_MIN
                             : bpm > (int32_t)PROJECT_BPM_MAX ? PROJECT_BPM_MAX
                                                              : (uint32_t)bpm);
}
