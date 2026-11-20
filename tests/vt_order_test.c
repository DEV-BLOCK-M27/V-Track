/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The order list and song playback on the simulated SMK-37 (seam A, tests/vt_sim.h). ARP opens the order list:
 * each entry names one pattern per track. > adds an entry after the cursor's, shift + > duplicates it, shift + <
 * deletes it, shift + K2 moves it. SEQ PLAY runs the order list from the current entry (the cursor's) top to bottom
 * and loops at its end; shift + SEQ PLAY switches the pattern-loop mode (the current entry over and over). Fxx sets
 * the tempo from its row on, Bxx jumps to entry xx, Dxx goes to the next entry's row xx. The order list and its pads
 * show the playing entry. Each entry plays its own note (track 1, row 0) for its 16 rows at 300 BPM.
 *   tests/vt_order_test [OUT_DIR]   (OUT_DIR: the order list's screen as order.ppm) */
#include <math.h>
#include "vt_sim.h"

#define K2 1u                                /* encoders: K2 cursor (the entry, the row), K4 tempo, K8 effect */
#define K4 3u
#define K8 7u
#define FX_B 7                               /* shift + K8: the effect letters (none) 0 1 2 3 4 A B D F G S V */
#define FX_D 8
#define FX_F 9
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_E4 16u
#define KEY_G4 19u
#define CPU_HZ 1789773.0
#define HZ_C4 (CPU_HZ / (16.0 * (0x1AB + 1)))   /* FamiTracker's NTSC pulse periods */
#define HZ_E4 (CPU_HZ / (16.0 * (0x152 + 1)))
#define HZ_G4 (CPU_HZ / (16.0 * (0x11C + 1)))
#define R (44100.0 * 60 / (300 * 4))         /* a row at 300 BPM: 50 ms */
#define E (16 * R)                           /* an entry of 16-row patterns: 800 ms */

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

static void drop_audio(void)
{
    uint32_t n;
    free(left(&n));
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

static uint32_t onset(const int32_t *a, uint32_t n)   /* the first sample that leaves the level a starts at */
{
    uint32_t i;
    for (i = 1; i < n; i++)
        if (a[i] != a[0])
            return i;
    return n;
}

/* the time b .. e (samples from s0, 4 ms off each end) plays one tone; its frequency in hz, 0 if none */
static double tone(const int32_t *a, uint32_t n, uint32_t s0, double b, double e)
{
    uint32_t i = s0 + (uint32_t)b + 176u, j = s0 + (uint32_t)e - 176u;
    return j <= n && i < j ? frequency(a + i, j - i) : -1;
}

static int at(double got, double want) { return fabs(got / want - 1) < 0.002; }

static void button(uint32_t b)               /* a button pressed and released */
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

static void press(uint32_t key)              /* a key pressed and released */
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

static void turn(uint32_t e, int32_t steps)
{
    sim_encoder(e, steps);
    sim_run_ms(40);
}

static void shifted(uint32_t b)              /* a button pressed and released with KNOB BANK (shift) held */
{
    sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    button(b);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
}

static void shift_turn(uint32_t e, int32_t steps)   /* an encoder turned with KNOB BANK (shift) held */
{
    sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    turn(e, steps);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
}

static void enter_row0(uint32_t key)         /* pattern view: the key's note into row 0 of the entry's track 1 */
{
    tap(0);
    button(SMK_BTN_SEQ_REC);
    press(key);
    button(SMK_BTN_SEQ_REC);
}

/* the audio's entries k0 .. k1 - 1 from s0 play the tones hz[] (one an entry); what gets the tones heard */
static int entries_play(const int32_t *a, uint32_t n, uint32_t s0, uint32_t k0, const double *hz, uint32_t k,
                        char *what, size_t sz)
{
    uint32_t i;
    int ok = 1;
    size_t o = strlen(what);
    for (i = 0; i < k; i++) {
        double f = tone(a, n, s0, (k0 + i) * E, (k0 + i + 1) * E);
        ok &= at(f, hz[i]);
        o += (size_t)snprintf(what + o, sz - o, " %.0f", f);
    }
    return ok;
}

static void go_entry(uint32_t e)             /* the pattern view on entry e (via the order list) */
{
    button(SMK_BTN_ARP);
    turn(K2, -64);
    turn(K2, (int32_t)e);
    button(SMK_BTN_SEQ);
}

static void effect(uint32_t row, int32_t letter, int32_t p)   /* pattern view: track 1's row gets effect letter p
                                                               * (letter 0: none) */
{
    tap(row);
    shift_turn(K8, -20);
    shift_turn(K8, letter);
    turn(K8, p);
}

static uint16_t line_bg(uint32_t i)         /* the ground of the screen's line i (under the header), at its right end */
{
    return sim_frame()[(30u + i * 16u + 8u) * 240u + 236u];
}

/* SEQ PLAY for k + 1/2 entries' time, then stopped: the entries played the tones hz[] from the start on */
static void plays(const double *hz, uint32_t k, const char *label)
{
    char what[200];
    int32_t *a;
    uint32_t n, s0;
    sim_run_ms(100);
    drop_audio();
    button(SMK_BTN_SEQ_PLAY);
    sim_run_ms((uint32_t)((k + 0.5) * E / 44.1));
    button(SMK_BTN_SEQ_PLAY);
    a = left(&n);
    s0 = onset(a, n);
    snprintf(what, sizeof what, "%s (Hz:", label);
    check(entries_play(a, n, s0, 0, hz, k, what, sizeof what - 2), strcat(what, ")"));
    free(a);
}

/* the screen's line from text first (its first place on the screen, top down) rightwards is want */
static int line_is(const char *first, const char *want)
{
    char line[64];
    return sim_screen_line(first, line, sizeof line) && !strcmp(line, want);
}

int main(int argc, char **argv)
{
    char what[200];

    sim_power_on();
    sim_run_ms(100);
    turn(K4, 180);                           /* 300 BPM */

    /* three entries: C4, E4, G4 in row 0 of their own pattern on track 1 */
    enter_row0(KEY_C4);
    button(SMK_BTN_ARP);
    check(sim_screen_text("ORDER"), "ARP: the order list");
    check(sim_button_led(SMK_BTN_ARP), "ARP: its LED lit in the order list");
    button(SMK_BTN_RIGHT);                   /* > : an entry added after the cursor, the cursor on it */
    button(SMK_BTN_SEQ);
    check(sim_screen_text("P01"), "> added entry 01 with new patterns: the pattern view edits P01");
    enter_row0(KEY_E4);
    button(SMK_BTN_ARP);
    button(SMK_BTN_RIGHT);
    button(SMK_BTN_SEQ);
    check(sim_screen_text("P02"), "> again: entry 02, P02");
    enter_row0(KEY_G4);
    button(SMK_BTN_ARP);
    {
        char line[64];
        sim_screen_line("01", line, sizeof line);
        snprintf(what, sizeof what, "the order list: entry 01 names P01, the first pattern unnamed, on each track (%s)",
                 line);
        check(!strcmp(line, "01 01 01 01 01"), what);
        sim_screen_line("02", line, sizeof line);
        check(!strcmp(line, "02 02 02 02 02"), "entry 02: P02 on each track");
        check(sim_screen_tidy(what, sizeof what), what);
    }
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/order.ppm", argv[1]);
        sim_save_ppm(path);
    }

    /* played from entry 00: C4, E4, G4 an entry each, then C4 again */
    turn(K2, -2);
    {
        static const double HZ[4] = {HZ_C4, HZ_E4, HZ_G4, HZ_C4};
        plays(HZ, 4, "entries 00 01 02, then 00 again: C4 E4 G4 C4");
    }

    /* shift + > on entry 01: a copy of it after it (C4 E4 E4 G4); shift + < on entry 00: deleted (E4 E4 G4);
     * shift + K2 -2 on entry 02: it moves to the top (G4 E4 E4) */
    turn(K2, 1);
    shifted(SMK_BTN_RIGHT);
    check(line_is("02", "02 01 01 01 01") && line_is("03", "03 02 02 02 02"),
          "shift + > on entry 01: entry 02 a copy of it, the old 02 now 03");
    turn(K2, -2);
    shifted(SMK_BTN_LEFT);
    check(line_is("00", "00 01 01 01 01") && !sim_screen_text("03"), "shift + < on entry 00: deleted, 3 entries left");
    turn(K2, 2);
    shift_turn(K2, -2);
    check(line_is("00", "00 02 02 02 02") && line_is("01", "01 01 01 01 01"),
          "shift + K2 -2 on entry 02: it moves to the top");
    {
        static const double HZ[4] = {HZ_G4, HZ_E4, HZ_E4, HZ_G4};
        plays(HZ, 4, "after duplicate, delete, move: G4 E4 E4, then G4 again");
    }
    button(SMK_BTN_LEFT);
    check(line_is("02", "02 01 01 01 01"), "< alone: nothing deleted");
    shifted(SMK_BTN_LEFT);
    shifted(SMK_BTN_LEFT);
    shifted(SMK_BTN_LEFT);
    check(line_is("00", "00 01 01 01 01") && sim_pad_led(0) != 0 && sim_pad_led(1) == 0,
          "shift + < three times: down to one entry (its pad lit, pad 2 dark), the last one not deleted");

    /* > twice: entries 01 and 02 name the first patterns unnamed, P00 (C4) and P02 (G4): E4 C4 G4. Played from
     * entry 02 (the cursor's): G4, then from the top E4 C4 */
    button(SMK_BTN_RIGHT);
    button(SMK_BTN_RIGHT);
    check(line_is("00", "00 01 01 01 01") && line_is("02", "02 02 02 02 02"),
          "> twice: entries 01 (P00) and 02 (P02) added, the cursor on 02");
    {
        static const double HZ[3] = {HZ_G4, HZ_E4, HZ_C4};
        plays(HZ, 3, "SEQ PLAY on entry 02: from there (G4), then the order list from the top (E4 C4)");
    }

    /* shift + SEQ PLAY: the pattern-loop mode; the cursor's entry again and again */
    shifted(SMK_BTN_SEQ_PLAY);
    check(sim_screen_text("LOOP") && !sim_screen_text("SONG") && !sim_button_led(SMK_BTN_SEQ_PLAY),
          "shift + SEQ PLAY: the pattern-loop mode (LOOP on the screen), not playing");
    turn(K2, -1);
    {
        static const double HZ[3] = {HZ_C4, HZ_C4, HZ_C4};
        plays(HZ, 3, "LOOP on entry 01: C4 C4 C4");
    }
    {
        int32_t *a;
        uint32_t n, s0;
        sim_run_ms(100);
        drop_audio();
        button(SMK_BTN_SEQ_PLAY);
        sim_run_ms((uint32_t)(0.5 * E / 44.1));
        turn(K2, -1);                        /* while entry 01 loops: the cursor to entry 00 */
        sim_run_ms((uint32_t)(2.9 * E / 44.1));
        button(SMK_BTN_SEQ_PLAY);
        a = left(&n);
        s0 = onset(a, n);
        {
            static const double HZ[3] = {HZ_C4, HZ_E4, HZ_E4};
            snprintf(what, sizeof what, "LOOP, the cursor moved to entry 00 while 01 plays: 01 ends, then 00 loops (Hz:");
            check(entries_play(a, n, s0, 0, HZ, 3, what, sizeof what - 2), strcat(what, ")"));
        }
        free(a);
    }
    sim_button(SMK_BTN_KNOB_BANK, 1);       /* (shift + SEQ PLAY while in the pattern view) */
    button(SMK_BTN_SEQ);
    check(sim_screen_text("LOOP"), "the pattern view: LOOP in its header");
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
    shifted(SMK_BTN_SEQ_PLAY);
    check(!sim_screen_text("LOOP"), "shift + SEQ PLAY again: the song mode (no LOOP in the header)");
    button(SMK_BTN_ARP);
    check(sim_screen_text("SONG"), "the order list: SONG");

    /* entries 00 E4 (P01), 01 C4 (P00), 02 G4 (P02). F78 in entry 00's row 8: rows 8 to 15 at 120 BPM, a row 2.5
     * times as long; entry 01's C4 starts then and the tempo stays; the project's tempo (the header) does not change */
    go_entry(0);
    effect(8, FX_F, 0x78);
    check(sim_screen_text("F78") && sim_screen_text("300 BPM"), "F78 in entry 00's row 8; the header: 300 BPM");
    button(SMK_BTN_ARP);
    turn(K2, -64);
    {
        int32_t *a;
        uint32_t n, s0;
        double R2 = 2.5 * R, t1 = 8 * R + 8 * R2;   /* entry 01 from t1 on */
        sim_run_ms(100);
        drop_audio();
        button(SMK_BTN_SEQ_PLAY);
        sim_run_ms((uint32_t)((t1 + 6 * R2) / 44.1));
        button(SMK_BTN_SEQ_PLAY);
        a = left(&n);
        s0 = onset(a, n);
        snprintf(what, sizeof what, "F78 at row 8: entry 00's E4 lasts 8 rows at 300 BPM + 8 at 120 (%.0f ms) (%.0f Hz)",
                 t1 / 44.1, tone(a, n, s0, 8 * R + 5 * R2, t1));
        check(at(tone(a, n, s0, 8 * R + 5 * R2, t1), HZ_E4), what);
        snprintf(what, sizeof what, "then entry 01's C4, its rows at 120 BPM (%.0f Hz)",
                 tone(a, n, s0, t1, t1 + 5 * R2));
        check(at(tone(a, n, s0, t1, t1 + 5 * R2), HZ_C4), what);
        free(a);
    }
    check(sim_screen_text("300 BPM"), "after playing: the project's tempo still 300 BPM");
    go_entry(0);
    effect(8, FX_F, 0x06);                   /* F01 .. F1F: FamiTracker's speed, which V-Track has not: nothing */
    button(SMK_BTN_ARP);
    turn(K2, -64);
    {
        static const double HZ[2] = {HZ_E4, HZ_C4};
        plays(HZ, 2, "F06 (FamiTracker's speed) in entry 00's row 8: the tempo stays, E4 C4 an entry each");
    }
    go_entry(0);
    effect(8, 0, 0);
    check(!sim_screen_text("F78") && !sim_screen_text("F06"), "the F effect taken out again");

    /* B00 in entry 01's row 4: after it entry 00 again (E4 16 rows, C4 5 rows, E4 ...), entry 02 never */
    go_entry(1);
    effect(4, FX_B, 0);
    check(sim_screen_text("B00"), "B00 in entry 01's row 4");
    button(SMK_BTN_ARP);
    turn(K2, -64);
    {
        int32_t *a;
        uint32_t n, s0;
        sim_run_ms(100);
        drop_audio();
        button(SMK_BTN_SEQ_PLAY);
        sim_run_ms((uint32_t)(44 * R / 44.1));
        button(SMK_BTN_SEQ_PLAY);
        a = left(&n);
        s0 = onset(a, n);
        snprintf(what, sizeof what, "B00: E4 (16 rows) C4 (5 rows) E4 (16 rows) C4 (Hz: %.0f %.0f %.0f %.0f)",
                 tone(a, n, s0, 0, 16 * R), tone(a, n, s0, 16 * R, 21 * R), tone(a, n, s0, 21 * R, 37 * R),
                 tone(a, n, s0, 37 * R, 42 * R));
        check(at(tone(a, n, s0, 0, 16 * R), HZ_E4) && at(tone(a, n, s0, 16 * R, 21 * R), HZ_C4) &&
                  at(tone(a, n, s0, 21 * R, 37 * R), HZ_E4) && at(tone(a, n, s0, 37 * R, 42 * R), HZ_C4),
              what);
        free(a);
    }

    /* D04 there instead: after row 4 entry 02 from its row 4 (its G4 in row 0 skipped: C4 sounds on 12 rows), then
     * entry 00 */
    go_entry(1);
    effect(4, FX_D, 4);
    check(sim_screen_text("D04"), "D04 in entry 01's row 4");
    button(SMK_BTN_ARP);
    turn(K2, -64);
    {
        int32_t *a;
        uint32_t n, s0;
        sim_run_ms(100);
        drop_audio();
        button(SMK_BTN_SEQ_PLAY);
        sim_run_ms((uint32_t)(40 * R / 44.1));
        button(SMK_BTN_SEQ_PLAY);
        a = left(&n);
        s0 = onset(a, n);
        snprintf(what, sizeof what, "D04: E4 (16 rows), C4 (5 + 12 rows: entry 02 from row 4), E4 (Hz: %.0f %.0f %.0f)",
                 tone(a, n, s0, 0, 16 * R), tone(a, n, s0, 16 * R, 33 * R), tone(a, n, s0, 33 * R, 38 * R));
        check(at(tone(a, n, s0, 0, 16 * R), HZ_E4) && at(tone(a, n, s0, 16 * R, 33 * R), HZ_C4) &&
                  at(tone(a, n, s0, 33 * R, 38 * R), HZ_E4),
              what);
        free(a);
    }
    go_entry(1);
    effect(4, 0, 0);

    /* the entry playing deleted (the last, 02, the cursor's): the next row is entry 00's first, its E4 */
    button(SMK_BTN_ARP);
    turn(K2, 2);
    {
        int32_t *a;
        uint32_t n, s0;
        double t = 0;
        sim_run_ms(100);
        drop_audio();
        button(SMK_BTN_SEQ_PLAY);
        sim_run_ms((uint32_t)(0.5 * E / 44.1));
        a = left(&n);
        s0 = onset(a, n);
        t = n - s0;                          /* (time played so far, samples) */
        free(a);
        shifted(SMK_BTN_LEFT);
        sim_run_ms((uint32_t)(4 * R / 44.1));
        button(SMK_BTN_SEQ_PLAY);
        a = left(&n);
        t += 60 * 44.1;                      /* (shift + < pressed after 60 ms) */
        {
            double b = ceil(t / R + 1) * R - t;   /* the second row start after the delete, from the audio's start */
            snprintf(what, sizeof what, "entry 02 deleted while it plays: entry 00 from its first row (E4) (%.0f Hz)",
                     tone(a, n, 0, b, b + 2 * R));
            check(at(tone(a, n, 0, b, b + 2 * R), HZ_E4), what);
        }
        free(a);
    }
    button(SMK_BTN_RIGHT);                   /* (02 back: P02 again, G4) */
    check(line_is("02", "02 02 02 02 02"), "> : entry 02 back (P02)");
    turn(K2, -64);

    /* the order list while playing from entry 00 (the cursor's): the playing entry's line has a ground of its own
     * (the cursor's line with it: another one) and its pad another colour; both move on with each entry */
    button(SMK_BTN_ARP);
    turn(K2, -64);
    {
        uint16_t idle = line_bg(0), none = line_bg(1);
        uint32_t pad_cursor = sim_pad_led(0), pad_entry = sim_pad_led(1), pad_playing = 0, k, j, p, ms = 0, ok;
        button(SMK_BTN_SEQ_PLAY);
        for (k = 0; k < 3u; k++) {
            uint16_t g[3];
            uint32_t mid = (uint32_t)((k + 0.5) * E / 44.1);   /* the middle of entry k */
            sim_run_ms(mid - ms);
            ms = mid;
            for (j = 0; j < 3u; j++)
                g[j] = line_bg(j);
            pad_playing = sim_pad_led(k);
            snprintf(what, sizeof what, "playing entry %02u: its line's ground (%04X %04X %04X), its pad (%06X)", k, g[0],
                     g[1], g[2], pad_playing);
            if (k == 0u)
                ok = g[0] != idle && g[0] != none && g[1] == none && g[2] == none;
            else
                ok = g[0] == idle && g[k] != idle && g[k] != none && g[3u - k] == none;
            ok &= pad_playing != pad_cursor && pad_playing != pad_entry && pad_playing != 0;
            for (j = 0; j < 3u; j++)
                if (j != k)
                    ok &= sim_pad_led(j) == (j ? pad_entry : pad_cursor);
            check(ok, what);
        }
        go_entry(1);                         /* the pattern view on entry 01 while entry 02 plays */
        for (ok = 1, p = 0; p < 16u; p++)
            ok &= sim_pad_led(p) != pad_playing;
        check(ok, "the pattern view on entry 01 while 02 plays: no pad shows a playing row");
        button(SMK_BTN_SEQ_PLAY);
    }

    printf(fails ? "V-TRACK ORDER: %d FAILED\n" : "V-TRACK ORDER: ok\n", fails);
    return fails != 0;
}
