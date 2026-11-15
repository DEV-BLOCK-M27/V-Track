/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The project model: what a piece of music is, as operations on it. The app edits it, the player (in the audio ISR)
 * reads it:
 *   project_init()                      a new project: every pattern empty, PROJECT_ROWS_NEW rows, 120 BPM
 *   project_rows(track)                 the length of track 0..3's pattern in rows
 *   project_note(track, row)            the note column of a row: PROJECT_EMPTY, PROJECT_CUT, PROJECT_RELEASE or
 *                                       a note, PROJECT_NOTE(MIDI number)
 *   project_set_note(track, row, v)     writes it (anything else is refused: the row stays as it was)
 *   project_bpm(), project_set_bpm(b)   the tempo, one row a sixteenth; kept within PROJECT_BPM_MIN .. MAX
 * So far: one pattern a track, the note column only. A row's note column is one byte, so the player reads it
 * whole while the app writes it. */
#define PROJECT_TRACKS 4u
#define PROJECT_ROWS 64u                 /* a pattern's room: 16 to 64 rows (#6 makes the length settable) */
#define PROJECT_ROWS_NEW 16u             /* a new pattern's length: one page, all of it on the pads */
#define PROJECT_BPM_MIN 30u
#define PROJECT_BPM_MAX 300u
#define PROJECT_EMPTY 0u                 /* no change */
#define PROJECT_NOTE(m) ((m) + 1u)       /* MIDI 0..127 -> 1..128 */
#define PROJECT_RELEASE 0xFEu            /* === */
#define PROJECT_CUT 0xFFu                /* --- */
#define PROJECT_IS_NOTE(v) ((v) >= 1u && (v) <= 128u)

static struct {
    volatile uint8_t note[PROJECT_TRACKS][PROJECT_ROWS];
    volatile uint16_t bpm;
    uint8_t rows;                        /* the patterns' length */
} project;

static void project_init(void)
{
    uint32_t t, r;
    for (t = 0; t < PROJECT_TRACKS; t++)
        for (r = 0; r < PROJECT_ROWS; r++)
            project.note[t][r] = PROJECT_EMPTY;
    project.bpm = 120;
    project.rows = PROJECT_ROWS_NEW;
}

static uint32_t project_rows(uint32_t track) { return track < PROJECT_TRACKS ? project.rows : 0u; }

static uint32_t project_note(uint32_t track, uint32_t row)
{
    return row < project_rows(track) ? project.note[track][row] : PROJECT_EMPTY;
}

static void project_set_note(uint32_t track, uint32_t row, uint32_t v)
{
    if (row < project_rows(track) &&
        (v == PROJECT_EMPTY || PROJECT_IS_NOTE(v) || v == PROJECT_CUT || v == PROJECT_RELEASE))
        project.note[track][row] = (uint8_t)v;
}

static uint32_t project_bpm(void) { return project.bpm; }

static void project_set_bpm(int32_t bpm)
{
    project.bpm = (uint16_t)(bpm < (int32_t)PROJECT_BPM_MIN   ? PROJECT_BPM_MIN
                             : bpm > (int32_t)PROJECT_BPM_MAX ? PROJECT_BPM_MAX
                                                              : (uint32_t)bpm);
}
