/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* USB MIDI on the simulated SMK-37 (seam A, tests/vt_sim.h): a note on from the host plays the selected track as a
 * key does, its note off ends it. Playing a pattern sends each track's notes on MIDI channels 1 to 4, with note offs
 * (a new note, a cut, a release, stop); a live note goes out on its track's channel too. Playing sends MIDI start, a
 * clock of 24 pulses a quarter (6 a row, the first with row 0) and stop. Incoming MIDI clock drives the rows instead
 * of the tempo (the first pulse after a start is row 0's, every 6th the next row's); MIDI start and stop start and
 * stop playing; V-Track sends no clock of its own meanwhile. Half a second without a pulse, the tempo drives again,
 * the row's pulses counted (no clock sent for a MIDI start's playing). SEQ PLAY always sends start, its stop stop.
 *   tests/vt_midi_test */
#include <math.h>
#include "vt_sim.h"

#define K2 1u                                /* encoders: K2 cursor, K3 track */
#define K3 2u
#define K4 3u                                /* K4 tempo */
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_CS4 13u
#define KEY_E4 16u
#define KEY_G4 19u
#define HZ_C4 (1789773.0 / (16.0 * (0x1AB + 1)))   /* FamiTracker's NTSC period for C-4: 261.36 Hz */

static int fails;
static void check(int ok, const char *what)
{
    printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* the left channel of the audio rendered since the last call, from frame skip on */
static int32_t *left(uint32_t skip, uint32_t *n)
{
    uint32_t frames, i;
    const int32_t *a = sim_audio(&frames);
    int32_t *l;
    *n = frames > skip ? frames - skip : 0;
    l = malloc((*n + 1u) * sizeof *l);
    for (i = 0; i < *n; i++)
        l[i] = a[2u * (skip + i)];
    return l;
}

static int silent(const int32_t *a, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (a[i])
            return 0;
    return 1;
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

static int at(double got, double want) { return fabs(got / want - 1) < 0.002; }

static void turn(uint32_t e, int32_t steps)
{
    sim_encoder(e, steps);
    sim_run_ms(50);
}

static void button(uint32_t b)               /* a button pressed and released */
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

static void press(uint32_t key)              /* a key pressed and released */
{
    sim_key(key, 127);
    sim_run_ms(40);
    sim_key(key, 0);
    sim_run_ms(40);
}

static void shift_press(uint32_t key)        /* .. with KNOB BANK (shift) held: a cut (white), a release (black) */
{
    sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    press(key);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
}

static void tap(uint32_t p)                  /* a pad hit and released: the cursor to row p */
{
    sim_pad(p, 100);
    sim_run_ms(40);
    sim_pad(p, 0);
    sim_run_ms(40);
}

/* the notes on and off sent since the last call, as "c+nn/vel" (on) and "c-nn" (off), c the channel 1..16,
 * space-separated */
static const char *notes_out(void)
{
    static char s[2048];
    uint32_t p[256], n = sim_midi_out(p, 256), i, o = 0;
    s[0] = 0;
    for (i = 0; i < n && o < sizeof s - 16u; i++) {
        uint32_t cin = p[i] & 15u, st = p[i] >> 8 & 0xFFu, d1 = p[i] >> 16 & 0xFFu, d2 = p[i] >> 24;
        if (cin == 9u && st >> 4 == 9u && d2)
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%s%u+%u/%u", o ? " " : "", (st & 15u) + 1u, d1, d2);
        else if ((cin == 8u && st >> 4 == 8u) || (cin == 9u && st >> 4 == 9u))
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%s%u-%u", o ? " " : "", (st & 15u) + 1u, d1);
    }
    return s;
}

/* everything sent since the last call: the notes as notes_out, "start" "stop", "." a clock pulse */
static const char *midi_out(void)
{
    static char s[4096];
    uint32_t p[512], n = sim_midi_out(p, 512), i, o = 0;
    s[0] = 0;
    for (i = 0; i < n && o < sizeof s - 16u; i++) {
        uint32_t cin = p[i] & 15u, st = p[i] >> 8 & 0xFFu, d1 = p[i] >> 16 & 0xFFu, d2 = p[i] >> 24;
        const char *sep = o ? " " : "";
        if (cin == 0xFu && st == 0xF8u)
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%s.", sep);
        else if (cin == 0xFu && st == 0xFAu)
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%sstart", sep);
        else if (cin == 0xFu && st == 0xFCu)
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%sstop", sep);
        else if (cin == 9u && st >> 4 == 9u && d2)
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%s%u+%u/%u", sep, (st & 15u) + 1u, d1, d2);
        else if ((cin == 8u && st >> 4 == 8u) || (cin == 9u && st >> 4 == 9u))
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%s%u-%u", sep, (st & 15u) + 1u, d1);
        else
            o += (uint32_t)snprintf(s + o, sizeof s - o, "%s?%08x", sep, p[i]);
    }
    return s;
}

static uint32_t count(const char *s, const char *tok)   /* the tokens tok in s */
{
    const char *p;
    uint32_t n = 0, k = (uint32_t)strlen(tok);
    for (p = s; *p; p++)
        if (!strncmp(p, tok, k) && (p[k] == ' ' || !p[k]) && (p == s || p[-1] == ' '))
            n++;
    return n;
}

static void notes_are(const char *want, const char *what)
{
    char line[160];
    const char *got = notes_out();
    snprintf(line, sizeof line, "%s: [%s]", what, got);
    check(!strcmp(got, want), line);
    if (strcmp(got, want))
        printf("    want [%s]\n", want);
}

int main(void)
{
    char what[160];
    int32_t *a;
    uint32_t n;
    double f;

    sim_power_on();
    sim_run_ms(100);
    free(left(0, &n));

    /* notes in: the selected track (2, NES Pulse 2) plays them, any channel */
    turn(K3, 1);
    sim_midi_in(0x95, 60, 100);              /* C-4 on channel 6 */
    sim_run_ms(500);
    a = left(44100u / 10u, &n);
    f = frequency(a, n);
    snprintf(what, sizeof what, "MIDI note on C-4: track 2 plays one tone at %.2f Hz (%.2f Hz)", f, HZ_C4);
    check(at(f, HZ_C4), what);
    free(a);
    sim_midi_in(0x85, 60, 0);
    sim_run_ms(300);
    a = left(44100u / 20u, &n);
    check(n > 10000u && silent(a, n), "MIDI note off C-4: silent");
    free(a);
    sim_midi_in(0x90, 62, 90);               /* a note on with velocity 0 is a note off */
    sim_run_ms(200);
    sim_midi_in(0x90, 62, 0);
    sim_run_ms(300);
    a = left(44100u * 250u / 1000u, &n);
    check(n > 10000u && silent(a, n), "MIDI note on with velocity 0: a note off, silent");
    free(a);

    /* a pattern: track 1 C-4 E-4 cut, track 2 G-4, track 3 C-4 release, track 4 E-4 (velocity recording: F) */
    button(SMK_BTN_SEQ_REC);
    turn(K3, -3);
    tap(0);
    press(KEY_C4);
    press(KEY_E4);
    shift_press(KEY_C4);
    turn(K3, 1);
    tap(0);
    press(KEY_G4);
    turn(K3, 1);
    tap(0);
    press(KEY_C4);
    shift_press(KEY_CS4);
    turn(K3, 1);
    tap(0);
    press(KEY_E4);
    button(SMK_BTN_SEQ_REC);
    notes_out();                             /* (the live notes entering it) */

    /* played at 120 BPM: a row 125 ms */
    button(SMK_BTN_SEQ_PLAY);                /* (row 0 from about 30 ms on) */
    notes_are("1+60/127 2+67/127 3+60/127 4+64/127", "playing, row 0: each track's note on its channel");
    sim_run_ms(110);
    notes_are("1-60 1+64/127 3-60", "row 1: track 1's next note, track 3's release");
    sim_run_ms(125);
    notes_are("1-64", "row 2: track 1's cut");
    sim_run_ms(125);
    notes_are("", "row 3: nothing");
    button(SMK_BTN_SEQ_PLAY);
    notes_are("2-67 4-64", "stopped: the notes still on, off");

    /* a live note: on its track's channel (track 4 selected) */
    sim_key(KEY_G4, 100);
    sim_run_ms(50);
    sim_key(KEY_G4, 0);
    sim_run_ms(50);
    sim_midi_in(0x90, 72, 100);
    sim_run_ms(50);
    sim_midi_in(0x80, 72, 0);
    sim_run_ms(50);
    notes_are("4+67/127 4-67 4+72/127 4-72", "live notes, a key and a MIDI note: on track 4's channel");

    /* V-Track's own clock: start, 6 pulses a row (the first with row 0), stop; none while stopped */
    sim_run_ms(500);
    midi_out();
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(110);                         /* (row 1 at 125 ms) */
    {
        const char *got = midi_out();
        const char *want = "start . 1+60/127 2+67/127 3+60/127 4+64/127 . . . . . . 1-60 1+64/127 3-60";
        snprintf(what, sizeof what, "SEQ PLAY: start, a pulse, row 0, 6 pulses, row 1 (%s)", got);
        check(!strncmp(got, want, strlen(want)), what);
        if (strncmp(got, want, strlen(want)))
            printf("    want [%s ...]\n", want);
    }
    sim_run_ms(1000);
    {
        const char *got = midi_out();
        uint32_t pulses = count(got, ".");
        snprintf(what, sizeof what, "playing at 120 BPM: %u pulses a second (48)", pulses);
        check(pulses == 48u || pulses == 49u, what);
    }
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms(500);
    {
        const char *got = midi_out();
        snprintf(what, sizeof what, "SEQ PLAY again: stop, then no clock (%.40s)", got);
        check(count(got, "stop") == 1u && !strchr(strstr(got, "stop") ? strstr(got, "stop") : got, '.'), what);
    }

    /* following MIDI clock at 120 BPM, the project's tempo 90 BPM (a row 167 ms): pulses 20.8 ms apart */
    turn(K4, -30);
    midi_out();
    {
        uint32_t i;
        const char *got;
        int ok = 1;
        sim_midi_in(0xFA, 0, 0);
        sim_run_ms(3);
        got = midi_out();
        snprintf(what, sizeof what, "MIDI start: row 0 plays, no start or clock sent (%s)", got);
        check(!strcmp(got, "1+60/127 2+67/127 3+60/127 4+64/127"), what);
        for (i = 0; i < 6u; i++) {       /* pulses 0 .. 5: row 0's */
            sim_midi_in(0xF8, 0, 0);
            sim_run_ms(i % 3u == 2u ? 20 : 21);
        }
        got = midi_out();
        snprintf(what, sizeof what, "6 pulses (125 ms): still row 0 (%s)", got);
        check(!strcmp(got, ""), what);
        sim_midi_in(0xF8, 0, 0);         /* pulse 6: row 1 */
        sim_run_ms(3);
        got = midi_out();
        snprintf(what, sizeof what, "the 7th pulse: row 1 at once (%s)", got);
        check(!strcmp(got, "1-60 1+64/127 3-60"), what);
        sim_run_ms(18);
        for (i = 7; i < 12u; i++) {      /* pulses 7 .. 11 */
            sim_midi_in(0xF8, 0, 0);
            sim_run_ms(i % 3u == 2u ? 20 : 21);
        }
        ok &= !strcmp(midi_out(), "");
        sim_midi_in(0xF8, 0, 0);         /* pulse 12: row 2 */
        sim_run_ms(3);
        got = midi_out();
        ok &= !strcmp(got, "1-64");
        snprintf(what, sizeof what, "pulses 7 to 12: row 2 with the 13th (%s)", got);
        check(ok, what);
        sim_run_ms(18);
        sim_midi_in(0xFC, 0, 0);         /* stop */
        sim_run_ms(3);
        got = midi_out();
        snprintf(what, sizeof what, "MIDI stop: the notes off, no stop sent (%s)", got);
        check(!strcmp(got, "2-67 4-64"), what);
        for (i = 0; i < 24u; i++) {      /* clock on, stopped: nothing plays */
            sim_midi_in(0xF8, 0, 0);
            sim_run_ms(i % 3u == 2u ? 20 : 21);
        }
        got = midi_out();
        snprintf(what, sizeof what, "pulses while stopped: nothing plays (%s)", got);
        check(!strcmp(got, ""), what);
        button(SMK_BTN_SEQ_PLAY);        /* SEQ PLAY 20 ms after the last pulse: V-Track's own playing, sent */
        got = midi_out();
        snprintf(what, sizeof what, "SEQ PLAY just after pulses while stopped: start and a pulse sent (%.40s)", got);
        check(!strncmp(got, "start . 1+60/127", 16), what);
        sim_midi_in(0xF8, 0, 0);         /* a pulse in meanwhile: it drives the rows, ... */
        sim_run_ms(20);
        button(SMK_BTN_SEQ_PLAY);
        got = midi_out();
        snprintf(what, sizeof what, "SEQ PLAY again, a pulse in before: stop sent (%.40s)", got);
        check(count(got, "stop") == 1u, what);

        sim_midi_in(0xFA, 0, 0);         /* started again, then the clock stops: the tempo drives after 500 ms */
        sim_run_ms(3);
        for (i = 0; i < 3u; i++) {
            sim_midi_in(0xF8, 0, 0);
            sim_run_ms(i % 3u == 2u ? 20 : 21);
        }
        midi_out();                      /* (the last pulse 20 ms ago, row 0's third: 4 pulses of the row to come) */
        sim_run_ms(540);
        got = midi_out();
        snprintf(what, sizeof what, "no pulse for 560 ms: still row 0 (%s)", got);
        check(!strcmp(got, ""), what);
        sim_run_ms(80);                  /* 500 ms, then the row's 4 pulses left at 90 BPM: 111 ms */
        got = midi_out();
        snprintf(what, sizeof what, "the tempo plays the rest of the row: row 1 by 640 ms, no clock (%.40s)", got);
        check(!strcmp(got, "1-60 1+64/127 3-60"), what);
        button(SMK_BTN_SEQ_PLAY);
    }

    printf(fails ? "V-TRACK MIDI: %d FAILED\n" : "V-TRACK MIDI: ok\n", fails);
    return fails != 0;
}
