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

static struct {                          /* the patterns: 40 KiB, in the pool (gfx.c's cv_px is there too) */
    volatile uint8_t note[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS],
        ins[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS], vol[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS];
    volatile uint16_t effect[PROJECT_TRACKS][PROJECT_PATTERNS][PROJECT_ROWS];
    volatile uint8_t rows[PROJECT_TRACKS][PROJECT_PATTERNS];   /* their lengths */
} project_pat __attribute__((section(".pool")));

static struct {
    volatile uint8_t pattern[PROJECT_TRACKS];   /* the pattern each track plays */
    volatile uint16_t bpm;
} project;

static void project_init(void)
{
    uint32_t t, p, r;
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
