/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* A pattern played and step-entered on the simulated SMK-37 (seam A, tests/vt_sim.h). With SEQ REC on and the
 * sequencer stopped, a key writes its note into the cursor row and the cursor moves down a row; KNOB BANK (shift)
 * with a white key writes a cut, with a black key a release. SEQ PLAY starts and stops looped playback of the
 * pattern on all four tracks, one row a sixteenth at the tempo K4 sets. A row's note takes the track's channel, a
 * cut or a release silences it (no instruments yet: a release is a cut), an empty row keeps what plays. The pads
 * show the 16 rows of the cursor's page (empty, note, cursor, playing row), a tap moves the cursor there; the
 * screen lists the selected track's rows.
 *   tests/vt_pattern_test [OUT_DIR]   (OUT_DIR: the screen after the entry as pattern.ppm) */
#include <math.h>
#include "vt_sim.h"

#define K3 2u                                /* encoders: K3 track select, K4 tempo */
#define K4 3u
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_CS4 13u                          /* (a black key) */
#define KEY_E4 16u
#define KEY_G4 19u
#define KEY_C6 36u
#define CPU_HZ 1789773.0
#define HZ_C4 (CPU_HZ / (16.0 * (0x1AB + 1)))   /* FamiTracker's NTSC pulse periods: C-4 261.36 Hz */
#define HZ_E4 (CPU_HZ / (16.0 * (0x152 + 1)))   /* E-4 329.97 Hz */
#define HZ_G4 (CPU_HZ / (16.0 * (0x11C + 1)))   /* G-4 391.84 Hz */

static int fails;
static void check(int ok, const char *what)
{
    printf("%-84s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* the left channel of the audio rendered since the last call */
static int32_t *left(uint32_t *n)
{
    uint32_t i;
    const int32_t *a = sim_audio(n);
    int32_t *l = malloc((*n + 1u) * sizeof *l);
    for (i = 0; i < *n; i++)
        l[i] = a[2u * i];
    return l;
}

static int steady(const int32_t *a, uint32_t n)   /* one level throughout: no sound */
{
    uint32_t i;
    for (i = 1; i < n; i++)
        if (a[i] != a[0])
            return 0;
    return n > 0;
}

/* the frequency of a, from its rising crossings of the mean (interpolated); 0 if it is not one steady tone: the
 * periods between crossings must agree within 2 % */
static double frequency(const int32_t *a, uint32_t n)
{
    double mean = 0, first = -1, last = 0, prev = 0, lo = 1e9, hi = 0;
    uint32_t i, crossings = 0;
    for (i = 0; i < n; i++)
        mean += a[i];
    mean /= n ? n : 1;
    for (i = 1; i < n; i++)
        if (a[i - 1] <= mean && a[i] > mean) {
            double t = i - 1 + (mean - a[i - 1]) / (a[i] - a[i - 1]);
            if (first < 0) {
                first = t;
            } else {
                crossings++;
                lo = fmin(lo, t - prev);
                hi = fmax(hi, t - prev);
            }
            prev = last = t;
        }
    if (!crossings || hi > lo * 1.02)
        return 0;
    return crossings * 44100.0 / (last - first);
}

static int at(double got, double want) { return fabs(got / want - 1) < 0.002; }

static uint32_t onset(const int32_t *a, uint32_t n)   /* the first sample that leaves the level a starts at */
{
    uint32_t i;
    for (i = 1; i < n; i++)
        if (a[i] != a[0])
            return i;
    return n;
}

static void press(uint32_t key)               /* a key pressed and released */
{
    sim_key(key, 100);
    sim_run_ms(40);
    sim_key(key, 0);
    sim_run_ms(40);
}

static void tap(uint32_t p)                  /* a pad hit and released */
{
    sim_pad(p, 100);
    sim_run_ms(40);
    sim_pad(p, 0);
    sim_run_ms(40);
}

static void shifted(uint32_t key)            /* a key pressed and released with KNOB BANK (shift) held */
{
    sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    press(key);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
}

static void button(uint32_t b)                /* a button pressed and released */
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

/* rows k0 .. k1-1 of the audio a, a row R samples long from s0 on, 4 ms off each end: one tone at hz (0: silent) */
static int rows_play(const int32_t *a, uint32_t n, double s0, double R, uint32_t k0, uint32_t k1, double hz)
{
    uint32_t b = (uint32_t)(s0 + k0 * R) + 176u, e = (uint32_t)(s0 + k1 * R) - 176u;
    if (e > n || b >= e)
        return 0;
    return hz ? at(frequency(a + b, e - b), hz) : steady(a + b, e - b);
}

static double level(const int32_t *a, uint32_t n, double hz)   /* the amplitude at hz (Hann window, Goertzel) */
{
    double w = 2 * M_PI * hz / 44100.0, c = 2 * cos(w), s1 = 0, s2 = 0, mean = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        mean += a[i];
    mean /= n ? n : 1;
    for (i = 0; i < n; i++) {
        double s = (a[i] - mean) * (0.5 - 0.5 * cos(2 * M_PI * i / n)) + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

static int pads_are(uint32_t want, uint32_t mask)   /* the pads in mask show colour want, the others not */
{
    uint32_t p;
    for (p = 0; p < 16u; p++)
        if ((sim_pad_led(p) == want) != (mask >> p & 1u))
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    char what[160];
    int32_t *a;
    uint32_t n, s0, note, cursor, empty;
    double R = 44100.0 * 60 / (120 * 4);      /* a sixteenth at 120 BPM: 5512.5 samples */

    sim_power_on();
    sim_run_ms(100);
    sim_button(SMK_BTN_KNOB_BANK, 1);        /* shift + SEQ REC: velocity recording off (vt_record_test's) */
    sim_run_ms(30);
    sim_button(SMK_BTN_SEQ_REC, 1);
    sim_run_ms(30);
    sim_button(SMK_BTN_SEQ_REC, 0);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);

    /* step entry: REC on, stopped; C4, E4, G4 go to rows 1, 2, 3 of track 1 */
    button(SMK_BTN_SEQ_REC);
    press(KEY_C4);
    press(KEY_E4);
    press(KEY_G4);
    /* row 4 empty; a cut on row 5 (shift + a white key), row 6 empty; C4 on row 7 (a tap on pad 7 moves the cursor
     * there), a release on row 8 (shift + a black key) */
    tap(4);
    shifted(KEY_C6);
    tap(6);
    press(KEY_C4);
    shifted(KEY_CS4);
    check(sim_button_led(SMK_BTN_SEQ_REC), "SEQ REC on: its LED lit");
    button(SMK_BTN_SEQ_REC);
    sim_run_ms(200);
    a = left(&n);
    free(a);
    check(!sim_button_led(SMK_BTN_SEQ_REC), "SEQ REC off: its LED dark");

    /* the pads: rows 1 to 16; note, cut and release rows one colour, the cursor (row 9) another, empty rows a third */
    note = sim_pad_led(0);
    cursor = sim_pad_led(8);
    empty = sim_pad_led(3);
    check(note != cursor && note != empty && cursor != empty && empty != 0,
          "pads: notes, the cursor, empty rows: three colours, empty rows lit too");
    check(pads_are(note, 0xD7u), "pads 1 2 3 5 7 8 (C4 E4 G4 --- C4 ===): the note colour");
    check(pads_are(cursor, 0x100u), "pad 9: the cursor (moved down after the release)");
    check(pads_are(empty, 0xFE28u), "pads 4 6 10 to 16: empty");
    tap(10);
    check(pads_are(cursor, 0x400u) && pads_are(empty, 0xFB28u), "pad 11 tapped: the cursor there, pad 9 empty");
    tap(8);

    /* the screen: the rows of track 1 */
    check(sim_screen_text("00") && sim_screen_text("0C") && !sim_screen_text("0D"), "the screen lists rows 00 to 0C");
    check(sim_screen_text("C-4") && sim_screen_text("E-4") && sim_screen_text("G-4") && sim_screen_text("---") &&
              sim_screen_text("===") && sim_screen_text("..."),
          "the screen shows C-4 E-4 G-4 --- === and empty rows (...)");
    check(sim_screen_text("120 BPM"), "the screen shows the tempo: 120 BPM");
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/pattern.ppm", argv[1]);
        sim_save_ppm(path);
    }

    /* play: the three notes a sixteenth apart at 120 BPM, then G4 held through the empty rows; the playing row walks
     * the pads, a sixteenth a row */
    a = left(&n);
    free(a);
    button(SMK_BTN_SEQ_PLAY);
    check(sim_button_led(SMK_BTN_SEQ_PLAY), "SEQ PLAY: its LED lit");
    {
        uint32_t ms, p, row = 16, rows = 0, ok = 1, t0 = 0, lo = 1000, hi = 0;
        for (ms = 0; ms < 1000u; ms++) {
            uint32_t at_row = 16, k = 0;
            sim_run_ms(1);
            for (p = 0; p < 16u; p++) {   /* the playing row: the one pad in none of the three colours */
                uint32_t c = sim_pad_led(p);
                if (c != note && c != cursor && c != empty) {
                    at_row = p;
                    k++;
                }
            }
            if (k > 1u || (k == 0u && row < 16u))
                ok = 0;
            if (k == 1u && at_row != row) {
                if (at_row != (row == 16u ? 0u : row + 1u))
                    ok = 0;
                if (row < 16u && rows > 0u) {   /* a whole row's time on the pads */
                    lo = ms - t0 < lo ? ms - t0 : lo;
                    hi = ms - t0 > hi ? ms - t0 : hi;
                }
                if (row < 16u)
                    rows++;
                row = at_row;
                t0 = ms;
            }
        }
        snprintf(what, sizeof what, "the playing row walks pads 1, 2, 3 ... (%u rows), a row %u to %u ms on each", rows,
                 lo, hi);
        check(ok && rows >= 7u && lo >= 125u - 16u && hi <= 125u + 16u, what);
    }
    button(SMK_BTN_SEQ_PLAY);
    check(!sim_button_led(SMK_BTN_SEQ_PLAY), "SEQ PLAY again: stopped, its LED dark");
    check(pads_are(note, 0xD7u) && pads_are(cursor, 0x100u) && pads_are(empty, 0xFE28u),
          "stopped: the pads show notes, the cursor and empty rows again");
    a = left(&n);
    s0 = onset(a, n);
    check(s0 < 44100u / 20u, "SEQ PLAY: the pattern starts within 50 ms");
    snprintf(what, sizeof what, "row 1: C4 for a sixteenth at 120 BPM (%.1f ms)", R / 44.1);
    check(rows_play(a, n, s0, R, 0, 1, HZ_C4), what);
    check(rows_play(a, n, s0, R, 1, 2, HZ_E4), "row 2: E4 for a sixteenth");
    check(rows_play(a, n, s0, R, 2, 3, HZ_G4), "row 3: G4 for a sixteenth");
    check(rows_play(a, n, s0, R, 3, 4, HZ_G4), "row 4, empty: G4 sounds on");
    check(rows_play(a, n, s0, R, 4, 6, 0), "row 5, a cut: silent, and through row 6, empty");
    check(rows_play(a, n, s0, R, 6, 7, HZ_C4), "row 7: C4");
    check(rows_play(a, n, s0, R, 7, 8, 0), "row 8, a release (no instrument: as a cut): silent");
    free(a);

    /* K4: the tempo; stopped, then playing */
    sim_encoder(K4, 60);
    sim_run_ms(50);
    check(sim_screen_text("180 BPM") && !sim_screen_text("120 BPM"), "K4 +60: the screen shows 180 BPM");
    sim_run_ms(200);
    a = left(&n);
    free(a);
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(800);
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(100);
    a = left(&n);
    s0 = onset(a, n);
    R = 44100.0 * 60 / (180 * 4);
    snprintf(what, sizeof what, "180 BPM: rows 1 to 4 C4 E4 G4 G4, a sixteenth each (%.1f ms)", R / 44.1);
    check(rows_play(a, n, s0, R, 0, 1, HZ_C4) && rows_play(a, n, s0, R, 1, 2, HZ_E4) &&
              rows_play(a, n, s0, R, 2, 4, HZ_G4), what);
    check(rows_play(a, n, s0, R, 4, 6, 0) && rows_play(a, n, s0, R, 6, 7, HZ_C4) && rows_play(a, n, s0, R, 7, 8, 0),
          "180 BPM: rows 5 6 silent, row 7 C4, row 8 silent");
    free(a);

    sim_run_ms(100);
    a = left(&n);
    free(a);
    button(SMK_BTN_SEQ_PLAY);                /* K4 +60 while row 1 plays: row 7's C4 lasts a sixteenth at 240 BPM */
    sim_encoder(K4, 60);
    sim_run_ms(800);
    check(sim_screen_text("240 BPM"), "K4 +60 while playing: the screen shows 240 BPM");
    button(SMK_BTN_SEQ_PLAY);
    a = left(&n);
    {
        uint32_t i = onset(a, n), j, k;
        while (i + 441u < n && !steady(a + i, 441u))   /* the cut: 10 ms of one level */
            i++;
        for (j = i; j + 1u < n && a[j + 1u] == a[i]; j++)   /* row 7's C4 begins */
            ;
        for (k = j + 1u; k + 1u < n && !steady(a + k, 441u); k++)   /* and ends at the release */
            ;
        snprintf(what, sizeof what, "row 7's C4 at 240 BPM: %.1f ms (a sixteenth: 62.5 ms)", (k - j) / 44.1);
        check(fabs((k - j) / 44.1 - 62.5) < 4.0, what);
    }
    free(a);

    /* all four tracks through the one 2A03: E4 on track 2 and G4 on track 3 in row 1 with track 1's C4, the noise
     * (C6) on track 4 from row 2, where track 1 plays E4 */
    sim_encoder(K4, -120);
    button(SMK_BTN_SEQ_REC);
    for (n = 1; n < 4u; n++) {
        sim_encoder(K3, 1);
        sim_run_ms(50);
        tap(n == 3u ? 1u : 0u);
        press(n == 1u ? KEY_E4 : n == 2u ? KEY_G4 : KEY_C6);
    }
    button(SMK_BTN_SEQ_REC);
    sim_run_ms(100);
    {
        char line[128];
        sim_screen_line("01", line, sizeof line);
        check(sim_screen_text("120 BPM") && sim_screen_text("NES \xB7 NOISE") &&
                  !strcmp(line, "01 C-6 .. . ... E-4 ... ..."),
              "track 4 selected: the screen lists its rows (C-6 on row 2, track 1's E-4 beside it), 120 BPM");
    }
    a = left(&n);
    free(a);
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(600);
    button(SMK_BTN_SEQ_PLAY);
    a = left(&n);
    s0 = onset(a, n);
    R = 44100.0 * 60 / (120 * 4);
    {
        uint32_t b = s0 + 441u, b2 = (uint32_t)(s0 + R) + 441u, w = (uint32_t)R - 882u;
        double c4 = level(a + b, w, HZ_C4), e4 = level(a + b, w, HZ_E4), g4 = level(a + b, w, HZ_G4),
               gap = fmax(level(a + b, w, 295), level(a + b, w, 361)), c4b = level(a + b2, w, HZ_C4),
               e4b = level(a + b2, w, HZ_E4), g4b = level(a + b2, w, HZ_G4),
               gapb = (level(a + b2, w, 600) + level(a + b2, w, 1400) + level(a + b2, w, 1800)) / 3;
        snprintf(what, sizeof what, "row 1: C4, E4, G4 together (levels %.0f %.0f %.0f, between them %.0f)", c4, e4,
                 g4, gap);
        check(fmin(c4, fmin(e4, g4)) > 10 * gap, what);
        snprintf(what, sizeof what, "row 2: E4 (tracks 1 and 2) and G4 sound on, C4 is gone (%.0f %.0f %.0f)", e4b,
                 g4b, c4b);
        check(e4b > e4 && g4b > g4 / 2 && c4b < c4 / 3, what);
        /* between E4's and G4's harmonics: the noise (E4 and G4 alone leave < 0.01 % of E4's level there) */
        snprintf(what, sizeof what, "row 2: the noise joins (at 600, 1400, 1800 Hz: %.1f %% of E4's level)",
                 100 * gapb / e4b);
        check(gapb > 0.02 * e4b, what);
    }
    free(a);

    printf(fails ? "V-TRACK PATTERN: %d FAILED\n" : "V-TRACK PATTERN: ok\n", fails);
    return fails != 0;
}
