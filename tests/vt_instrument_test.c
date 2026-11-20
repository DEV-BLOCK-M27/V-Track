/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* NES instruments on the simulated SMK-37 (seam A, tests/vt_sim.h). An instrument's volume, arpeggio, pitch and
 * duty (mode) sequences advance a step a NES frame (60.1 Hz) from the note on; the row volume scales the volume
 * sequence (row x sequence / 15, at least 1); a release sends every sequence past its release point, a cut silences
 * at once. NOTE REPEAT switches the pads to instrument mode: the instruments of the track's chip lit, a tap selects
 * one, which the keys then play and step entry writes into the instrument column. The factory KICK on the triangle,
 * SNARE and HAT on the noise sound as drums: short, decaying on their own.
 *   tests/vt_instrument_test [OUT_DIR]   (OUT_DIR: the screen in instrument mode as instrument.ppm) */
#include <math.h>
#include "vt_sim.h"

#define K2 1u                                /* encoders: K2 cursor, K3 track, K7 volume; in the editor (PATCH) */
#define K3 2u                                /* K2 step, K3 sequence, K5 value, K6 length, K7 loop, K8 release */
#define K5 4u
#define K6 5u
#define K7 6u
#define K8 7u
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_C6 36u
#define CPU_HZ 1789773.0
#define HZ_C4 (CPU_HZ / (16.0 * (0x1AB + 1)))   /* FamiTracker's NTSC pulse periods */
#define HZ_E4 (CPU_HZ / (16.0 * (0x152 + 1)))
#define HZ_G4 (CPU_HZ / (16.0 * (0x11C + 1)))
#define FRAME (44100.0 / 60.0988)            /* a NES NTSC frame: 733.8 samples */
#define NONE PROJECT_SEQ_NONE

static const int32_t LEAST[4] = {0, -24, -127, 0};   /* each sequence's least value: volume, arpeggio, pitch, duty */

static int fails;
static void check(int ok, const char *what)
{
    printf("%-84s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static int32_t *left(uint32_t *n)            /* the left channel of the audio rendered since the last call */
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

static uint32_t onset(const int32_t *a, uint32_t n)   /* the first sample that leaves the level a starts at */
{
    uint32_t i;
    for (i = 1; i < n; i++)
        if (a[i] != a[0])
            return i;
    return n;
}

static uint32_t quiet_from(const int32_t *a, uint32_t n)   /* the first sample from which a stays at one level */
{
    uint32_t i = n;
    while (i > 1u && a[i - 1u] == a[n - 1u])
        i--;
    return i;
}

/* the frequency of a, from its rising crossings of the mean (interpolated); 0 if it is not one steady tone */
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

static int near(double got, double want, double tol) { return want ? fabs(got / want - 1) < tol : fabs(got) < tol; }

static double p2p(const int32_t *a, double b, double e)   /* peak to peak of samples b .. e */
{
    int32_t lo = a[(uint32_t)b], hi = lo;
    uint32_t i;
    for (i = (uint32_t)b; i < (uint32_t)e; i++) {
        lo = a[i] < lo ? a[i] : lo;
        hi = a[i] > hi ? a[i] : hi;
    }
    return hi - lo;
}

/* frame k of a note starting at s0: its middle half (away from the steps) */
static double frame_p2p(const int32_t *a, double s0, uint32_t k)
{
    return p2p(a, s0 + (k + 0.25) * FRAME, s0 + (k + 0.75) * FRAME);
}

static double duty(const int32_t *a, double b, double e)   /* the share of samples b .. e above their mean */
{
    double mean = 0;
    uint32_t i, up = 0;
    for (i = (uint32_t)b; i < (uint32_t)e; i++)
        mean += a[i];
    mean /= e - b;
    for (i = (uint32_t)b; i < (uint32_t)e; i++)
        up += a[i] > mean;
    return up / (e - b);
}

static uint32_t crossings(const int32_t *a, double b, double e)   /* rising crossings of the mean in b .. e */
{
    double mean = 0;
    uint32_t i, c = 0;
    for (i = (uint32_t)b; i < (uint32_t)e; i++)
        mean += a[i];
    mean /= e - b;
    for (i = (uint32_t)b + 1u; i < (uint32_t)e; i++)
        c += a[i - 1] <= mean && a[i] > mean;
    return c;
}

static void button(uint32_t b)
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

static void tap(uint32_t p)
{
    sim_pad(p, 100);
    sim_run_ms(40);
    sim_pad(p, 0);
    sim_run_ms(40);
}

static void press(uint32_t key)
{
    sim_key(key, 100);
    sim_run_ms(40);
    sim_key(key, 0);
    sim_run_ms(40);
}

static void turn(uint32_t e, int32_t steps)
{
    sim_encoder(e, steps);
    sim_run_ms(40);
}

static void select_ins(uint32_t i)           /* instrument mode, pad i tapped, back to pattern mode */
{
    button(SMK_BTN_NOTE_REPEAT);
    tap(i);
    button(SMK_BTN_NOTE_REPEAT);
}

/* the instrument editor (PATCH): free slot i becomes a copy of the selected instrument (shift + its pad), every
 * sequence emptied, then its sequence s set to v[0..n-1], loop and release a step or NONE. Back to the pattern view,
 * i selected */
static void edit_ins(uint32_t i, uint32_t s, const int8_t *v, uint32_t n, uint32_t loop, uint32_t release)
{
    uint32_t k;
    button(SMK_BTN_PATCH);
    sim_button(SMK_BTN_KNOB_BANK, 1);
    tap(i);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    for (k = 0; k < 4u; k++) {               /* each sequence: length 0 */
        turn(K3, k ? 1 : -4);
        turn(K6, -64);
    }
    turn(K3, -4);
    turn(K3, (int32_t)s);
    turn(K2, -64);
    for (k = 0; k < n; k++) {                /* each step: down to its least, up to v */
        turn(K5, -255);
        turn(K5, v[k] - LEAST[s]);
        turn(K2, 1);
    }
    turn(K7, -64);                           /* none, then up to the step */
    turn(K7, loop == NONE ? 0 : (int32_t)loop + 1);
    turn(K8, -64);
    turn(K8, release == NONE ? 0 : (int32_t)release + 1);
    button(SMK_BTN_SEQ);
}

static int32_t *hold(uint32_t key, uint32_t ms, uint32_t *n)   /* key held for ms: the audio from the press on */
{
    free(left(n));
    sim_key(key, 100);
    sim_run_ms(ms);
    sim_key(key, 0);
    return left(n);
}

static void line_is(const char *row, const char *want, const char *what)
{
    char got[160], w[200];
    sim_screen_line(row, got, sizeof got);
    snprintf(w, sizeof w, "%s: \"%s\"", what, got);
    check(!strcmp(got, want), w);
}

int main(int argc, char **argv)
{
    static const int8_t FADE[] = {15, 12, 9, 6, 3, 0}, ARP[] = {0, 4, 7}, DUTY[] = {0, 1, 2, 3}, FULL[] = {15},
                        HALF[] = {8}, ONE[] = {1}, LOOPREL[] = {15, 9, 6, 3, 0};
    char what[160];
    int32_t *a;
    uint32_t n, k, lit, sel, ok;
    double s0, l[6], R = 44100.0 * 60 / (120 * 4);

    sim_power_on();
    sim_run_ms(100);
    sim_button(SMK_BTN_KNOB_BANK, 1);        /* shift + SEQ REC: velocity recording off (vt_record_test's) */
    sim_run_ms(30);
    sim_button(SMK_BTN_SEQ_REC, 1);
    sim_run_ms(30);
    sim_button(SMK_BTN_SEQ_REC, 0);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);

    /* instrument mode: NOTE REPEAT; the factory instruments 00 to 04 lit, the free slots dark */
    button(SMK_BTN_NOTE_REPEAT);
    check(sim_button_led(SMK_BTN_NOTE_REPEAT), "NOTE REPEAT: instrument mode, its LED lit");
    lit = sim_pad_led(0);
    for (ok = lit != 0, k = 1; k < 16u; k++)
        ok &= k < 5u ? sim_pad_led(k) == lit : sim_pad_led(k) == 0u;
    check(ok, "pads 1 to 5: the factory instruments lit; pads 6 to 16 (free) dark");
    check(!sim_screen_text("INS00"), "no instrument selected yet: none in the header");
    tap(2);
    sel = sim_pad_led(2);
    check(sel != lit && sel != 0u && sim_pad_led(0) == lit, "pad 3 tapped: KICK selected, its pad another colour");
    check(sim_screen_text("INS02") && sim_screen_text("KICK"), "the header: INS02 KICK");
    tap(9);
    check(sim_pad_led(2) == sel && sim_pad_led(9) == 0u && sim_screen_text("INS02"),
          "pad 10 (a free slot) tapped: nothing selected, KICK stays");
    check(sim_screen_text("00") && sim_screen_text("0C"), "instrument mode: the rows stay on the screen");
    if (argc > 1) {
        snprintf(what, sizeof what, "%s/instrument.ppm", argv[1]);
        sim_save_ppm(what);
    }
    tap(4);
    button(SMK_BTN_SEQ_REC);
    press(KEY_C4);
    line_is("00", "00 C-4 04 . ... ... ... ...", "HAT selected, REC: a key writes C-4 with instrument 04");
    check(sim_screen_text("INS04") && sim_screen_text("HAT"), "the header: INS04 HAT");
    tap(3);
    check(sim_screen_text("SNARE") && sim_screen_text("REC") && sim_screen_tidy(what, sizeof what),
          "SNARE selected with REC on: the header's texts not cut or overlapping");
    tap(4);
    button(SMK_BTN_NOTE_REPEAT);
    check(!sim_button_led(SMK_BTN_NOTE_REPEAT) && sim_pad_led(5) != 0u && sim_pad_led(15) != 0u,
          "NOTE REPEAT again: pattern mode, the pads show rows (empty rows lit)");
    turn(K2, -1);
    turn(6u - 1u, 1);                        /* K6 on the cursor row's instrument 04: 05 */
    line_is("00", "00 C-4 05 . ... ... ... ...", "K6 +1 on row 00: instrument 05");
    turn(6u - 1u, -1);
    button(SMK_BTN_SEQ_REC);
    sim_run_ms(100);
    free(left(&n));

    /* a decaying volume sequence (15 12 9 6 3 0) fades a step a frame (C6 on Pulse 1) */
    edit_ins(5, PROJECT_SEQ_VOL, FADE, 6, NONE, NONE);
    select_ins(5);
    a = hold(KEY_C6, 300, &n);
    s0 = onset(a, n);
    for (k = 0; k < 6u; k++)
        l[k] = frame_p2p(a, s0, k);
    check(s0 < n && l[0] > l[1] && l[1] > l[2] && l[2] > l[3] && l[3] > l[4] && l[4] > 0,
          "volume 15 12 9 6 3: five falling levels, a frame each");
    check(near(p2p(a, s0 + 1.1 * FRAME, s0 + 1.5 * FRAME), p2p(a, s0 + 1.5 * FRAME, s0 + 1.9 * FRAME), 0.05),
          "one level within a frame");
    snprintf(what, sizeof what, "volume 0 from frame 6 on: silent (quiet from %.1f frames)",
             (quiet_from(a, n) - s0) / FRAME);
    check(quiet_from(a, n) < s0 + 5.5 * FRAME, what);
    free(a);

    /* an arpeggio 0 4 7 looping at 0: C E G C E G, a frame each, on one channel */
    edit_ins(6, PROJECT_SEQ_ARP, ARP, 3, 0, NONE);
    select_ins(6);
    a = hold(KEY_C4, 200, &n);
    s0 = onset(a, n);
    for (ok = 1, k = 0; k < 6u; k++) {
        double f = frequency(a + (uint32_t)(s0 + (k + 0.1) * FRAME), (uint32_t)(0.8 * FRAME)),
               want = k % 3u == 0 ? HZ_C4 : k % 3u == 1 ? HZ_E4 : HZ_G4;
        ok &= near(f, want, 0.01);
    }
    check(ok, "arpeggio 0 4 7 looped: C-4 E-4 G-4 C-4 E-4 G-4, a frame each");
    free(a);

    /* a duty sequence 0 1 2 3 looping: 12.5 25 50 75 % a frame each (C6 on Pulse 1) */
    edit_ins(7, PROJECT_SEQ_DUTY, DUTY, 4, 0, NONE);
    select_ins(7);
    a = hold(KEY_C6, 200, &n);
    s0 = onset(a, n);
    for (ok = 1, k = 0; k < 8u; k++) {
        double d = duty(a, s0 + (k + 0.15) * FRAME, s0 + (k + 0.85) * FRAME);
        ok &= fabs(d - (k % 4u == 0 ? 0.125 : k % 4u == 1 ? 0.25 : k % 4u == 2 ? 0.5 : 0.75)) < 0.05;
    }
    check(ok, "duty 0 1 2 3 looped: 12.5 25 50 75 %, a frame each");
    free(a);

    /* row volume x sequence volume / 15, at least 1: on P01 of track 1, C6 rows with
     * row 00 FULL (15) at row volume 8, 01 HALF (8) at F, 02 ONE (1) at 1, 03 FULL at 1, 04 FULL at F (a row
     * volume holds until the next, so each row has its own) */
    edit_ins(8, PROJECT_SEQ_VOL, FULL, 1, NONE, NONE);
    edit_ins(9, PROJECT_SEQ_VOL, HALF, 1, NONE, NONE);
    edit_ins(10, PROJECT_SEQ_VOL, ONE, 1, NONE, NONE);
    sim_button(SMK_BTN_KNOB_BANK, 1);
    button(SMK_BTN_RIGHT);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    turn(K2, -64);
    button(SMK_BTN_SEQ_REC);
    {
        static const uint8_t INS[5] = {8, 9, 10, 8, 8};
        static const int32_t VOL[5] = {8, 15, 1, 1, 15};   /* K7 all the way down (0), then up to it */
        for (k = 0; k < 5u; k++) {
            select_ins(INS[k]);
            press(KEY_C6);
            turn(K2, -1);
            turn(K7, -16);
            turn(K7, VOL[k]);
            turn(K2, 1);
        }
    }
    button(SMK_BTN_SEQ_REC);
    line_is("00", "00 C-6 08 8 ... ... ... ...", "row 00: C-6 08 8");
    line_is("02", "02 C-6 0A 1 ... ... ... ...", "row 02: C-6 0A 1");
    line_is("04", "04 C-6 08 F ... ... ... ...", "row 04: C-6 08 F");
    sim_run_ms(100);
    free(left(&n));
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(800);
    button(SMK_BTN_SEQ_PLAY);
    a = left(&n);
    s0 = onset(a, n);
    for (k = 0; k < 5u; k++)
        l[k] = p2p(a, s0 + k * R + 1000, s0 + k * R + 4000);
    snprintf(what, sizeof what, "row volume 8 x sequence 15 = sequence 8 x row F (%.0f, %.0f), below 15 (%.0f)",
             l[0], l[1], l[4]);
    check(near(l[0], l[1], 0.02) && l[4] > 1.5 * l[0], what);
    snprintf(what, sizeof what, "row volume 1 x sequence 1: audible, at volume 1 (%.0f, %.0f)", l[2], l[3]);
    check(l[2] > 0 && near(l[2], l[3], 0.02) && l[2] < 0.5 * l[0], what);
    free(a);

    /* release and cut: on P02, LOOPREL (15 9, looping at 0, released at 1; then 6 3 0):
     * row 00 C-6 LOOPREL, 02 ===, 04 C-6, 06 --- */
    edit_ins(11, PROJECT_SEQ_VOL, LOOPREL, 5, 0, 1);
    sim_button(SMK_BTN_KNOB_BANK, 1);
    button(SMK_BTN_RIGHT);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    turn(K2, -64);
    select_ins(11);
    button(SMK_BTN_SEQ_REC);
    press(KEY_C6);
    turn(K2, 1);
    sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    press(KEY_C6 - 2u);                      /* shift + a black key: a release */
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
    turn(K2, 1);
    press(KEY_C6);
    turn(K2, 1);
    sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    press(KEY_C6);                           /* shift + a white key: a cut */
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
    button(SMK_BTN_SEQ_REC);
    line_is("02", "02 === .. . ... ... ... ...", "row 02: a release");
    line_is("06", "06 --- .. . ... ... ... ...", "row 06: a cut");
    sim_run_ms(100);
    free(left(&n));
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(1000);
    button(SMK_BTN_SEQ_PLAY);
    a = left(&n);
    s0 = onset(a, n);
    for (ok = 1, k = 0; k < 12u; k++)        /* rows 00 and 01: 15 9 15 9 ... */
        ok &= near(frame_p2p(a, s0, k), frame_p2p(a, s0, k % 2u), 0.03);
    check(ok && frame_p2p(a, s0, 1) < 0.8 * frame_p2p(a, s0, 0), "before the release: 15 9 looping, a frame each");
    {
        double r2 = s0 + 2 * R, lo = frame_p2p(a, s0, 1);
        uint32_t q = quiet_from(a, (uint32_t)(s0 + 4 * R));
        snprintf(what, sizeof what, "the release: 6 3 0 from it on (%.0f < %.0f), then silent (%.1f frames after it)",
                 p2p(a, r2 + 200, r2 + 0.75 * FRAME), lo, (q - r2) / FRAME);
        check(p2p(a, r2 + 200, r2 + 0.75 * FRAME) < lo && p2p(a, r2 + 200, r2 + 0.75 * FRAME) > 0 && q > r2 &&
                  q < r2 + 3.5 * FRAME && steady(a + q, (uint32_t)(s0 + 4 * R) - q),
              what);
        check(p2p(a, s0 + 4 * R + 200, s0 + 4 * R + 1000) > lo, "row 04: the note again, looping (still sounding)");
        check(steady(a + (uint32_t)(s0 + 6 * R + 176), (uint32_t)(2 * R - 352)), "row 06: the cut silences at once");
    }
    free(a);

    /* the factory drums: KICK on the triangle (track 3) falls in pitch and stops; SNARE and HAT on the noise
     * (track 4) stop on their own, the hat sooner; held keys all */
    turn(K3, 2);
    select_ins(2);
    a = hold(KEY_C4, 400, &n);
    s0 = onset(a, n);
    snprintf(what, sizeof what, "KICK on the triangle: its pitch falls (%u, then %u crossings a frame)",
             crossings(a, s0 + 0.1 * FRAME, s0 + 1.9 * FRAME), crossings(a, s0 + 4.1 * FRAME, s0 + 5.9 * FRAME));
    check(s0 < n && crossings(a, s0 + 0.1 * FRAME, s0 + 1.9 * FRAME) >=
                        1.5 * crossings(a, s0 + 4.1 * FRAME, s0 + 5.9 * FRAME),
          what);
    snprintf(what, sizeof what, "KICK: silent after 8 frames though held (quiet %.1f frames on)",
             (quiet_from(a, n) - s0) / FRAME);
    check(quiet_from(a, n) < s0 + 8 * FRAME, what);
    free(a);
    turn(K3, 1);
    select_ins(3);
    a = hold(KEY_C4, 400, &n);
    s0 = onset(a, n);
    {
        double snare = (quiet_from(a, n) - s0) / FRAME;
        free(a);
        select_ins(4);
        a = hold(KEY_C4, 400, &n);
        s0 = onset(a, n);
        double hat = (quiet_from(a, n) - s0) / FRAME;
        snprintf(what, sizeof what, "SNARE and HAT on the noise: noise, silent though held (%.1f, %.1f frames)",
                 snare, hat);
        check(s0 < n && frequency(a + (uint32_t)s0, (uint32_t)(2 * FRAME)) == 0 && snare > 6 && snare < 11 &&
                  hat > 2 && hat < snare - 2,
              what);
    }
    free(a);

    printf(fails ? "V-TRACK INSTRUMENTS: %d FAILED\n" : "V-TRACK INSTRUMENTS: ok\n", fails);
    return fails != 0;
}
