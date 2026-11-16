/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Pitch, timing and timbre effects on the simulated SMK-37 (seam A, tests/vt_sim.h). A row's effect column, in
 * FamiTracker notation, runs at the NES frame rate (60.1 Hz): 0xy arpeggio, 1xx / 2xx slides, 3xx portamento, 4xy
 * vibrato, Axy volume slide, Vxx duty or mode, Gxx note delay, Sxx note cut. The lasting ones run until replaced
 * or cleared with parameter 00. The expected values come from FamiTracker's NTSC period table and vibrato table
 * and the 2A03's pulse mixer. A note's frame 0 already has the effect applied once, as in FamiTracker.
 *   tests/vt_effect_test */
#include <math.h>
#include "vt_sim.h"

#define K2 1u                                /* encoders: K2 cursor, K3 track, K7 volume, K8 effect; in the editor */
#define K3 2u                                /* (PATCH) K2 step, K3 sequence, K5 value, K6 length, K7 loop, K8 */
#define K5 4u                                /* release */
#define K6 5u
#define K7 6u
#define K8 7u
#define KEY_FS3 6u                           /* keybed key 0 = C3 */
#define KEY_C4 12u
#define KEY_C5 24u
#define KEY_C6 36u
#define CPU_HZ 1789773.0
#define C5 0x0D5                             /* FamiTracker's NTSC pulse periods */
#define E5 0x0A9
#define G5 0x08E
#define C6 0x06A
#define E6 0x054
#define G6 0x046
#define FRAME (44100.0 / 60.0988)            /* a NES NTSC frame: 733.8 samples */
#define R (44100.0 * 60 / (120 * 4))         /* a row at 120 BPM: 7.5 frames */
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

static uint32_t onset(const int32_t *a, uint32_t n, uint32_t from)   /* the first sample from from on that leaves
                                                                      * the level there */
{
    uint32_t i;
    for (i = from + 1u; i < n; i++)
        if (a[i] != a[from])
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

/* the pulse's timer period in frame k of a note starting at s0 (its middle 70 %): CPU / (16 f) - 1; -1 if no
 * steady tone */
static double period(const int32_t *a, double s0, uint32_t k)
{
    double f = frequency(a + (uint32_t)(s0 + (k + 0.15) * FRAME), (uint32_t)(0.7 * FRAME));
    return f > 0 ? CPU_HZ / (16.0 * f) - 1.0 : -1.0;
}

/* frames k0 .. k0 + n - 1 of a note at s0 have the periods want[]; what: the periods got */
static int periods(const int32_t *a, double s0, uint32_t k0, const int32_t *want, uint32_t n, char *what, size_t sz)
{
    uint32_t k;
    int ok = 1;
    size_t o = strlen(what);
    for (k = 0; k < n; k++) {
        double p = period(a, s0, k0 + k);
        ok &= fabs(p - want[k]) < 0.5;
        if (o + 8u < sz)
            o += (size_t)snprintf(what + o, sz - o, " %.0f", p);
    }
    return ok;
}

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

static double frame_p2p(const int32_t *a, double s0, uint32_t k)   /* frame k of a note at s0: its middle half */
{
    return p2p(a, s0 + (k + 0.25) * FRAME, s0 + (k + 0.75) * FRAME);
}

static double pulse_level(uint32_t v) { return v ? 95.88 / (8128.0 / v + 100.0) : 0; }   /* the 2A03's mixer */

/* frames k0 .. of a note at s0 at the pulse volumes want[] (their levels relative to frame ref at volume vref) */
static int volumes(const int32_t *a, double s0, uint32_t k0, const uint32_t *want, uint32_t n, double ref,
                   uint32_t vref, char *what, size_t sz)
{
    uint32_t k;
    int ok = 1;
    size_t o = strlen(what);
    for (k = 0; k < n; k++) {
        double got = frame_p2p(a, s0, k0 + k) / ref * pulse_level(vref);
        uint32_t v = 0;
        while (v < 15u && fabs(pulse_level(v + 1u) - got) < fabs(pulse_level(v) - got))
            v++;
        ok &= v == want[k] && fabs(got / pulse_level(want[k]) - 1) < 0.03;
        if (o + 4u < sz)
            o += (size_t)snprintf(what + o, sz - o, " %u", v);
    }
    return ok;
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

static double correlation(const int32_t *a, double b, double e, uint32_t lag)   /* of a with itself lag later */
{
    double ma = 0, sab = 0, saa = 0, sbb = 0;
    uint32_t i, n = (uint32_t)(e - b);
    for (i = (uint32_t)b; i < (uint32_t)e + lag; i++)
        ma += a[i];
    ma /= n + lag;
    for (i = (uint32_t)b; i < (uint32_t)e; i++) {
        double x = a[i] - ma, y = a[i + lag] - ma;
        sab += x * y;
        saa += x * x;
        sbb += y * y;
    }
    return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0;
}

/* --- the controls --- */

static void button(uint32_t b)
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

static void shift(int on)                    /* KNOB BANK held / let go */
{
    sim_button(SMK_BTN_KNOB_BANK, on);
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

static void at(uint32_t r)                   /* the cursor to row r */
{
    turn(K2, -64);
    turn(K2, (int32_t)r);
}

static void new_pattern(void)                /* the track's next pattern (empty), the cursor on row 00 */
{
    shift(1);
    button(SMK_BTN_RIGHT);
    shift(0);
    at(0);
}

static void note(uint32_t r, uint32_t key)   /* row r: the key's note (step entry, REC) */
{
    at(r);
    button(SMK_BTN_SEQ_REC);
    press(key);
    button(SMK_BTN_SEQ_REC);
}

static void cut(uint32_t r)                  /* row r: a cut (REC, shift + a white key) */
{
    at(r);
    button(SMK_BTN_SEQ_REC);
    shift(1);
    press(KEY_C4);
    shift(0);
    button(SMK_BTN_SEQ_REC);
}

static void volume(uint32_t r, int32_t v)    /* row r: volume v (K7 down to 0, then up) */
{
    at(r);
    turn(K7, -16);
    turn(K7, v);
}

static void no_ins(uint32_t r)               /* row r: its instrument emptied (shift + K6) */
{
    at(r);
    shift(1);
    turn(K6, -1);
    shift(0);
}

static void effect(uint32_t r, char c, uint32_t p)   /* row r: effect c with parameter p (shift + K8, K8) */
{
    static const char LETTERS[] = " 01234ABDFGSV";
    at(r);
    shift(1);
    turn(K8, -20);
    turn(K8, (int32_t)(strchr(LETTERS, c) - LETTERS));
    shift(0);
    turn(K8, -255);
    turn(K8, (int32_t)p);
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
    shift(1);
    tap(i);
    shift(0);
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
    turn(K7, -64);
    turn(K7, loop == NONE ? 0 : (int32_t)loop + 1);
    turn(K8, -64);
    turn(K8, release == NONE ? 0 : (int32_t)release + 1);
    button(SMK_BTN_SEQ);
    select_ins(i);
}

static int32_t *play(uint32_t ms, uint32_t *n, double *s0)   /* the pattern played for ms from row 00: its audio, s0
                                                              * its first sound */
{
    int32_t *a;
    sim_run_ms(100);
    free(left(n));
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(ms);
    button(SMK_BTN_SEQ_PLAY);
    a = left(n);
    *s0 = onset(a, *n, 0);
    return a;
}

int main(void)
{
    char what[200];
    int32_t *a;
    uint32_t n;
    double s0;

    sim_power_on();
    sim_run_ms(100);

    /* 0xy arpeggio on track 1 (a plain tone): row 00 C-6 047, 02 000, 04 C-6 040 */
    new_pattern();
    note(0, KEY_C6);
    effect(0, '0', 0x47);
    effect(2, '0', 0x00);
    note(4, KEY_C6);
    effect(4, '0', 0x40);
    a = play(1300, &n, &s0);
    {
        static const int32_t ARP047[6] = {C6, E6, G6, C6, E6, G6}, STOP[5] = {C6, C6, C6, C6, C6},
                             ARP040[6] = {C6, E6, C6, E6, C6, E6};
        snprintf(what, sizeof what, "047: C-6 E-6 G-6 a frame each, looping:");
        check(periods(a, s0, 0, ARP047, 6, what, sizeof what), what);
        snprintf(what, sizeof what, "000 on row 02: C-6 from then on:");
        check(periods(a, s0 + 2 * R, 0, STOP, 5, what, sizeof what), what);
        snprintf(what, sizeof what, "040 (no y): C-6 E-6 C-6 E-6:");
        check(periods(a, s0 + 4 * R, 0, ARP040, 6, what, sizeof what), what);
    }
    free(a);

    /* 1xx / 2xx: row 00 C-6 102 (the period 2 lower a frame), 01 100, 02 C-6 202 (2 higher a frame), 03 200 */
    new_pattern();
    note(0, KEY_C6);
    effect(0, '1', 0x02);
    effect(1, '1', 0x00);
    note(2, KEY_C6);
    effect(2, '2', 0x02);
    effect(3, '2', 0x00);
    a = play(700, &n, &s0);
    {
        static const int32_t UP[7] = {104, 102, 100, 98, 96, 94, 92}, HELD_UP[5] = {90, 90, 90, 90, 90},
                             DOWN[7] = {108, 110, 112, 114, 116, 118, 120}, HELD_DOWN[5] = {122, 122, 122, 122, 122};
        snprintf(what, sizeof what, "102 on C-6: 2 lower a frame:");
        check(periods(a, s0, 0, UP, 7, what, sizeof what), what);
        snprintf(what, sizeof what, "100 on row 01: the slide stops where it is:");
        check(periods(a, s0, 8, HELD_UP, 5, what, sizeof what), what);
        snprintf(what, sizeof what, "row 02 C-6 202: from C-6 again, 2 higher a frame:");
        check(periods(a, s0 + 2 * R, 0, DOWN, 7, what, sizeof what), what);
        snprintf(what, sizeof what, "200 on row 03: stops:");
        check(periods(a, s0 + 2 * R, 8, HELD_DOWN, 5, what, sizeof what), what);
    }
    free(a);

    /* 0xy replaced by 1xx: row 00 C-6 047, 01 101: the slide goes on from the arpeggio's note then (E-6, frame 7) */
    new_pattern();
    note(0, KEY_C6);
    effect(0, '0', 0x47);
    effect(1, '1', 0x01);
    a = play(400, &n, &s0);
    {
        static const int32_t ON[5] = {E6, E6 - 1, E6 - 2, E6 - 3, E6 - 4};
        snprintf(what, sizeof what, "047, then 101 on row 01: from E-6 (frame 7) 1 lower a frame:");
        check(periods(a, s0, 7, ON, 5, what, sizeof what), what);
    }
    free(a);

    /* 4xy vibrato: row 00 C-6 486 (speed 8: 8 frames a cycle; depth 6: FamiTracker's 10.0, 7 9 6 0 -7 -9 -6 0
     * off the period), 02 400 */
    new_pattern();
    note(0, KEY_C6);
    effect(0, '4', 0x86);
    effect(2, '4', 0x00);
    a = play(600, &n, &s0);
    {
        static const int32_t VIB[12] = {99, 97, 100, 106, 113, 115, 112, 106, 99, 97, 100, 106},
                             STOP[5] = {C6, C6, C6, C6, C6};
        snprintf(what, sizeof what, "486 on C-6: a period of 8 frames, 7 9 6 0 -7 -9 -6 0:");
        check(periods(a, s0, 0, VIB, 12, what, sizeof what), what);
        snprintf(what, sizeof what, "400 on row 02: C-6 steady:");
        check(periods(a, s0 + 2 * R, 0, STOP, 5, what, sizeof what), what);
    }
    free(a);

    /* Vxx: row 00 C-6 V00 (12.5 %), 01 V03 (75 %), 02 C-6 (a new note keeps 75 %), 03 V01 (25 %) */
    new_pattern();
    note(0, KEY_C6);
    effect(0, 'V', 0x00);
    effect(1, 'V', 0x03);
    note(2, KEY_C6);
    effect(3, 'V', 0x01);
    a = play(700, &n, &s0);
    {
        double d[4];
        uint32_t k;
        for (k = 0; k < 4u; k++)
            d[k] = duty(a, s0 + k * R + 0.1 * R, s0 + k * R + 0.9 * R);
        snprintf(what, sizeof what, "V00 V03, a new note, V01: duty %.3f %.3f %.3f %.3f", d[0], d[1], d[2], d[3]);
        check(fabs(d[0] - 0.125) < 0.03 && fabs(d[1] - 0.75) < 0.03 && fabs(d[2] - 0.75) < 0.03 &&
                  fabs(d[3] - 0.25) < 0.03,
              what);
    }
    free(a);

    /* Gxx / Sxx: row 00 C-6, 01 ---, 02 C-6 G03, 03 ---, 04 C-6 S05, 06 C-6, 07 S02, 08 C-6 S00 */
    new_pattern();
    note(0, KEY_C6);
    cut(1);
    note(2, KEY_C6);
    effect(2, 'G', 0x03);
    cut(3);
    note(4, KEY_C6);
    effect(4, 'S', 0x05);
    note(6, KEY_C6);
    effect(7, 'S', 0x02);
    note(8, KEY_C6);
    effect(8, 'S', 0x00);
    a = play(1200, &n, &s0);
    {
        double g = (onset(a, n, (uint32_t)(s0 + 2 * R + 100)) - (s0 + 2 * R)) / FRAME,
               s = (quiet_from(a, (uint32_t)(s0 + 6 * R)) - (s0 + 4 * R)) / FRAME,
               h = (quiet_from(a, (uint32_t)(s0 + 8 * R)) - (s0 + 7 * R)) / FRAME;
        snprintf(what, sizeof what, "G03 on row 02: the note 3 frames after the row's start (%.2f)", g);
        check(fabs(g - 3) < 0.1, what);
        snprintf(what, sizeof what, "S05 on row 04: its note cut 5 frames after the row's start (%.2f)", s);
        check(fabs(s - 5) < 0.1, what);
        snprintf(what, sizeof what, "S02 on row 07: the held note cut 2 frames into it (%.2f)", h);
        check(fabs(h - 2) < 0.1, what);
        snprintf(what, sizeof what, "S00 on row 08 with C-6: silent (%.0f)", p2p(a, s0 + 8 * R + 100, s0 + 9 * R));
        check(p2p(a, s0 + 8 * R + 100, s0 + 9 * R) == 0, what);
    }
    free(a);

    /* Axy: row 00 C-6 F A08 (a volume step down a frame), 01 A00, 04 C-6 4 A80 (one up a frame, up to 15),
     * 08 C-6 1 A01 (an eighth down a frame: volume 1 while any eighth is left, then silent) */
    new_pattern();
    note(0, KEY_C6);
    volume(0, 15);
    effect(0, 'A', 0x08);
    effect(1, 'A', 0x00);
    note(4, KEY_C6);
    volume(4, 4);
    effect(4, 'A', 0x80);
    note(8, KEY_C6);
    volume(8, 1);
    effect(8, 'A', 0x01);
    a = play(1400, &n, &s0);
    {
        static const uint32_t DOWN[7] = {14, 13, 12, 11, 10, 9, 8}, HELD[5] = {7, 7, 7, 7, 7},
                              UP[12] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15};
        double ref = frame_p2p(a, s0, 0);
        snprintf(what, sizeof what, "F A08: a step down a frame:");
        check(volumes(a, s0, 0, DOWN, 7, ref, 14, what, sizeof what), what);
        snprintf(what, sizeof what, "A00 on row 01: it stays:");
        check(volumes(a, s0, 8, HELD, 5, ref, 14, what, sizeof what), what);
        snprintf(what, sizeof what, "row 04 C-6 4 A80: a step up a frame, up to 15:");
        check(volumes(a, s0 + 4 * R, 0, UP, 12, ref, 14, what, sizeof what), what);
        {
            static const uint32_t FADE[7] = {1, 1, 1, 1, 1, 1, 1};
            double q = (quiet_from(a, (uint32_t)(s0 + 9 * R)) - (s0 + 8 * R)) / FRAME;
            snprintf(what, sizeof what, "row 08 C-6 1 A01: volume 1 for 7 frames, silent from frame 7 (%.2f):", q);
            check(volumes(a, s0 + 8 * R, 0, FADE, 7, ref, 14, what, sizeof what) && fabs(q - 7) < 0.1, what);
        }
    }
    free(a);

    /* the instruments below: on track 1, each from the factory LEAD (00) as edit_ins copies it */
    select_ins(0);

    /* 3xx: an instrument whose volume falls 15 to 8 on frame 1 (a new start shows as 15 again); row 00 C-5,
     * 01 C-6 310 (glides 16 a frame from C-5, not started anew), 02 300, 03 C-5 (started anew) */
    {
        static const int8_t V15_8[2] = {15, 8};
        edit_ins(5, PROJECT_SEQ_VOL, V15_8, 2, NONE, NONE);
    }
    new_pattern();
    note(0, KEY_C5);
    note(1, KEY_C6);
    effect(1, '3', 0x10);
    effect(2, '3', 0x00);
    note(3, KEY_C5);
    a = play(800, &n, &s0);
    {
        static const int32_t GLIDE[8] = {197, 181, 165, 149, 133, 117, C6, C6}, START[3] = {C5, C5, C5};
        double l8 = frame_p2p(a, s0, 2);
        uint32_t k, ok = 1;
        snprintf(what, sizeof what, "C-5, then C-6 310 on row 01: from C-5 16 a frame to C-6:");
        check(periods(a, s0, 8, GLIDE, 8, what, sizeof what), what);
        for (k = 7; k < 15u; k++)
            ok &= fabs(frame_p2p(a, s0, k) / l8 - 1) < 0.05;
        check(ok, "3xx: not started anew (the volume stays at 8 through the glide)");
        snprintf(what, sizeof what, "300 on row 02, C-5 on row 03: started anew (volume 15 %.0f > 8 %.0f), at C-5:",
                 frame_p2p(a, s0 + 3 * R, 0), l8);
        check(frame_p2p(a, s0 + 3 * R, 0) > 1.3 * l8 && periods(a, s0 + 3 * R, 0, START, 3, what, sizeof what),
              what);
    }
    free(a);

    /* with the instrument's sequences, as in FamiTracker: an arpeggio sequence running wins over 0xy (12 12 12, no
     * loop: C-6 3 frames, then 047 on C-5); Axy scales with the volume sequence (8 x row volume / 15) */
    {
        static const int8_t ARP12[3] = {12, 12, 12}, V8[1] = {8};
        edit_ins(6, PROJECT_SEQ_ARP, ARP12, 3, NONE, NONE);
        new_pattern();
        note(0, KEY_C5);
        effect(0, '0', 0x47);
        edit_ins(7, PROJECT_SEQ_VOL, V8, 1, NONE, NONE);
        note(2, KEY_C6);
        volume(2, 15);
        effect(2, 'A', 0x08);
    }
    a = play(500, &n, &s0);
    {
        static const int32_t ARP[7] = {C6, C6, C6, C5, E5, G5, C5};
        static const uint32_t VOL[7] = {7, 6, 6, 5, 5, 4, 4};   /* 8 x 14 / 15, 8 x 13 / 15, ... */
        snprintf(what, sizeof what, "arpeggio 12 12 12 with 047 on C-5: the sequence, then the effect:");
        check(periods(a, s0, 0, ARP, 7, what, sizeof what), what);
        snprintf(what, sizeof what, "volume sequence 8 with F A08: 8 x (14, 13, ..) / 15:");
        check(volumes(a, s0 + 2 * R, 0, VOL, 7, frame_p2p(a, s0, 6), 15, what, sizeof what), what);
    }
    free(a);

    /* Vxx on the noise (track 4, plain notes: the instrument column emptied): V01 short mode (93 steps: it
     * repeats), V00 long; F#3 is its period 9 (254 CPU clocks: 93 steps 582.04 samples) */
    new_pattern();                           /* (track 1 silent) */
    turn(K3, 3);
    new_pattern();
    note(0, KEY_FS3);
    effect(0, 'V', 0x01);
    no_ins(0);
    note(2, KEY_FS3);
    effect(2, 'V', 0x00);
    no_ins(2);
    a = play(500, &n, &s0);
    {
        double cs = correlation(a, s0 + 1000, s0 + 4000, 582),
               cl = correlation(a, s0 + 2 * R + 1000, s0 + 2 * R + 4000, 582);
        snprintf(what, sizeof what, "noise V01: short mode, repeating (%.2f); V00: long (%.2f)", cs, cl);
        check(cs > 0.8 && fabs(cl) < 0.3, what);
    }
    free(a);

    printf(fails ? "V-TRACK EFFECTS: %d FAILED\n" : "V-TRACK EFFECTS: ok\n", fails);
    return fails != 0;
}
