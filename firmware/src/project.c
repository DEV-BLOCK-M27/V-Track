/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The project model: what a piece of music is, as operations on it. The app edits it, the player (in the audio ISR)
 * reads it:
 *   project_init()                      a new project: every pattern empty, PROJECT_ROWS_NEW rows, 120 BPM
 *   project_pattern(track)              the pattern track 0..3 plays and edits, 0 .. PROJECT_PATTERNS - 1 (the order
 *                                       list's place, #10); every call below means that pattern
 *   project_set_pattern(track, p)       selects it, kept within 0 .. PROJECT_PATTERNS - 1
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
 *   project_bpm(), project_set_bpm(b)   the tempo, one row a sixteenth; kept within PROJECT_BPM_MIN .. MAX
 *   project_ins_chip(i)                 the chip instrument 0..15 belongs to (ADR 0002), PROJECT_CHIP_NONE: a free slot
 *   project_ins_name(i)                 its name ("" for a free slot)
 *   project_ins_new(i, chip, name)      slot i becomes an instrument of the chip, every sequence empty
 *   project_seq(i, s)                   its sequence s (PROJECT_SEQ_*), read-only: len steps (0 = none, 64 at most),
 *                                       v[0 .. len-1], loop and release a step or PROJECT_SEQ_NONE
 *   project_set_seq(i, s, v, n, loop, release)  sets it (n kept to 64; a loop or release past the end: none)
 * An NES instrument's sequences: volume 0..15, arpeggio (semitones added to the note), pitch (added to the period
 * each frame: + lower), duty 0..3 (a pulse) or mode 0..1 (the noise). A new project has the factory instruments 00
 * LEAD, 01 BASS, 02 KICK (the triangle), 03 SNARE and 04 HAT (the noise); 05 to 0F are free.
 * Each field is one byte (the effect: one half-word), so the player reads it whole while the app writes it. */
#define PROJECT_TRACKS 4u
#define PROJECT_ROWS 64u                 /* a pattern's room, its longest */
#define PROJECT_ROWS_MIN 16u
#define PROJECT_PATTERNS 32u             /* a track's patterns */
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
    volatile uint8_t pattern[PROJECT_TRACKS];   /* the pattern each track plays */
    volatile uint16_t bpm;
} project;

static uint32_t project_ins_chip(uint32_t i)
{
    return i < PROJECT_INSTRUMENTS ? project_pat.ins_def[i].chip : PROJECT_CHIP_NONE;
}

static const char *project_ins_name(uint32_t i) { return i < PROJECT_INSTRUMENTS ? project_pat.ins_def[i].name : ""; }

static void project_ins_new(uint32_t i, uint32_t chip, const char *name)
{
    project_ins_t *d;
    uint32_t s, k;
    if (i >= PROJECT_INSTRUMENTS)
        return;
    d = &project_pat.ins_def[i];
    d->chip = (uint8_t)chip;
    for (k = 0; k + 1u < PROJECT_NAME && name[k]; k++)
        d->name[k] = name[k];
    for (; k < PROJECT_NAME; k++)
        d->name[k] = 0;
    for (s = 0; s < PROJECT_SEQS; s++)
        d->seq[s].len = 0, d->seq[s].loop = d->seq[s].release = PROJECT_SEQ_NONE;
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
        project.pattern[t] = 0;
    }
    project.bpm = 120;
}

static uint32_t project_pattern(uint32_t track) { return track < PROJECT_TRACKS ? project.pattern[track] : 0u; }

static void project_set_pattern(uint32_t track, int32_t p)
{
    if (track < PROJECT_TRACKS)
        project.pattern[track] = (uint8_t)(p < 0                            ? 0u
                                           : p >= (int32_t)PROJECT_PATTERNS ? PROJECT_PATTERNS - 1u
                                                                            : (uint32_t)p);
}

static uint32_t project_rows(uint32_t track)
{
    return track < PROJECT_TRACKS ? project_pat.rows[track][project.pattern[track]] : 0u;
}

static void project_set_rows(uint32_t track, int32_t n)
{
    if (track < PROJECT_TRACKS)
        project_pat.rows[track][project.pattern[track]] =
            (uint8_t)(n < (int32_t)PROJECT_ROWS_MIN ? PROJECT_ROWS_MIN
                      : n > (int32_t)PROJECT_ROWS   ? PROJECT_ROWS
                                                    : (uint32_t)n);
}

static uint32_t project_field(uint32_t track, uint32_t row, uint32_t f)
{
    if (row >= project_rows(track))
        return PROJECT_EMPTY;
    uint32_t p = project.pattern[track];
    return f == PROJECT_F_NOTE ? project_pat.note[track][p][row]
           : f == PROJECT_F_INS ? project_pat.ins[track][p][row]
           : f == PROJECT_F_VOL ? project_pat.vol[track][p][row]
           : f == PROJECT_F_EFFECT  ? project_pat.effect[track][p][row]
                                : PROJECT_EMPTY;
}

static int project_effect_ok(uint32_t v)     /* an effect: a letter of PROJECT_EFFECT_LETTERS, a parameter */
{
    const char *c;
    for (c = PROJECT_EFFECT_LETTERS; *c; c++)
        if ((uint32_t)(uint8_t)*c == v >> 8)
            return v >> 16 == 0;
    return 0;
}

static void project_set_field(uint32_t track, uint32_t row, uint32_t f, uint32_t v)
{
    uint32_t p;
    if (row >= project_rows(track))
        return;
    p = project.pattern[track];
    if (f == PROJECT_F_NOTE && (v == PROJECT_EMPTY || PROJECT_IS_NOTE(v) || v == PROJECT_CUT || v == PROJECT_RELEASE))
        project_pat.note[track][p][row] = (uint8_t)v;
    if (f == PROJECT_F_INS && v <= PROJECT_INS(PROJECT_INSTRUMENTS - 1u))
        project_pat.ins[track][p][row] = (uint8_t)v;
    if (f == PROJECT_F_VOL && v <= PROJECT_VOL(PROJECT_VOLUMES - 1u))
        project_pat.vol[track][p][row] = (uint8_t)v;
    if (f == PROJECT_F_EFFECT && (v == PROJECT_EMPTY || project_effect_ok(v)))
        project_pat.effect[track][p][row] = (uint16_t)v;
}

static uint32_t project_bpm(void) { return project.bpm; }

static void project_set_bpm(int32_t bpm)
{
    project.bpm = (uint16_t)(bpm < (int32_t)PROJECT_BPM_MIN   ? PROJECT_BPM_MIN
                             : bpm > (int32_t)PROJECT_BPM_MAX ? PROJECT_BPM_MAX
                                                              : (uint32_t)bpm);
}
