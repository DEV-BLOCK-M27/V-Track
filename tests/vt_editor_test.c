/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The instrument editor (PATCH) on the simulated SMK-37 (seam A, tests/vt_sim.h). PATCH opens it for the selected
 * instrument: its volume, arpeggio, pitch and duty sequences as four bar graphs. K2 moves along the steps, K3 picks
 * the sequence, K5 sets the step's value, K6 the length, K7 the loop point, K8 the release point; K4 the name's
 * character at the name cursor, shift + K4 the name cursor. The pads pick the instrument, shift + a free slot's pad
 * copies it there. The keys play it while editing. SEQ goes back to the pattern view.
 *   tests/vt_editor_test [OUT_DIR]   (OUT_DIR: the editor's screen as editor.ppm) */
#include <math.h>
#include "vt_sim.h"

#define K2 1u                                /* encoders: K2 step, K3 sequence, K4 name, K5 value, K6 length, */
#define K3 2u                                /* K7 loop, K8 release */
#define K4 3u
#define K5 4u
#define K6 5u
#define K7 6u
#define K8 7u
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_C6 36u
#define FRAME (44100.0 / 60.0988)            /* a NES NTSC frame: 733.8 samples */

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

static uint32_t onset(const int32_t *a, uint32_t n)   /* the first sample that leaves the level a starts at */
{
    uint32_t i;
    for (i = 1; i < n; i++)
        if (a[i] != a[0])
            return i;
    return n;
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

static double frame_p2p(const int32_t *a, double s0, uint32_t k)   /* frame k of a note from s0: its middle half */
{
    return p2p(a, s0 + (k + 0.25) * FRAME, s0 + (k + 0.75) * FRAME);
}

static int32_t *hold(uint32_t key, uint32_t ms, uint32_t *n)   /* key held for ms: the audio from the press on */
{
    free(left(n));
    sim_key(key, 100);
    sim_run_ms(ms);
    sim_key(key, 0);
    return left(n);
}

static uint32_t quiet_from(const int32_t *a, uint32_t n)   /* the first sample from which a stays at one level */
{
    uint32_t i = n;
    while (i > 1u && a[i - 1u] == a[n - 1u])
        i--;
    return i;
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

static int near(double got, double want, double tol) { return fabs(got / want - 1) < tol; }

/* the bars' lit pixels in the panel whose label is lbl, down to the label below: what the graph shows */
static uint32_t bars(const char *lbl, const char *below)
{
    int32_t a[4], b[4];
    uint32_t x, y, n = 0;
    const uint16_t *f = sim_frame();
    if (!sim_screen_box(lbl, a) || !sim_screen_box(below, b))
        return 0;
    for (y = (uint32_t)a[3] + 2u; y < (uint32_t)b[1] - 2u; y++)
        for (x = (uint32_t)a[2] + 4u; x < 240u; x++)   /* (right of the label and the step's value) */
            n += (f[y * 240u + x] & 31u) >= 24u;   /* bright blue (the bars), not the grounds */
    return n;
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

static void turn(uint32_t e, int32_t steps)
{
    sim_encoder(e, steps);
    sim_run_ms(40);
}

static void press(uint32_t key)
{
    sim_key(key, 100);
    sim_run_ms(40);
    sim_key(key, 0);
    sim_run_ms(40);
}

/* in the editor: sequence s set to v[0..n-1], loop and release a step or -1 (none), with the encoders */
static void edit_seq(uint32_t s, const int8_t *v, uint32_t n, int32_t loop, int32_t release)
{
    uint32_t k;
    turn(K3, -4);
    turn(K3, (int32_t)s);
    turn(K6, -64);                           /* length 0 */
    turn(K2, -64);
    for (k = 0; k < n; k++) {                /* each step: down to its least, up to v */
        turn(K5, -255);
        turn(K5, v[k] - LEAST[s]);
        turn(K2, 1);
    }
    turn(K7, -64);                           /* none, then up to the step */
    turn(K7, loop + 1);
    turn(K8, -64);
    turn(K8, release + 1);
}

int main(int argc, char **argv)
{
    char what[160];

    sim_power_on();
    sim_run_ms(100);

    /* PATCH with no instrument selected: the editor on 00 LEAD, its four sequences as bar graphs */
    button(SMK_BTN_PATCH);
    check(sim_button_led(SMK_BTN_PATCH), "PATCH: the editor, its LED lit");
    check(sim_screen_text("INS00") && sim_screen_text("LEAD"), "the header: INS00 LEAD");
    check(sim_screen_text("VOL") && sim_screen_text("ARP") && sim_screen_text("PITCH") && sim_screen_text("DUTY"),
          "the four sequences: VOL ARP PITCH DUTY");
    check(sim_screen_text("LEN 13") && sim_screen_text("REL 07"), "VOL: LEN 13, REL 07 (LEAD's volume)");
    check(sim_screen_tidy(what, sizeof what), "no text cut or overlapping");
    printf("  (%s)\n", what);
    if (argc > 1) {
        snprintf(what, sizeof what, "%s/editor.ppm", argv[1]);
        sim_save_ppm(what);
    }

    /* editing a step changes the next note: LEAD's volume step 00 from 15 to 3 */
    {
        int32_t *a;
        uint32_t n, k;
        double s0, before, after, held, louder;
        sim_run_ms(100);
        a = hold(KEY_C6, 200, &n);
        s0 = onset(a, n);
        before = frame_p2p(a, s0, 0);
        free(a);
        k = bars("VOL", "ARP");
        turn(K5, -12);
        check(sim_screen_text("3"), "K5 -12 on step 00: its value 3 shown");
        snprintf(what, sizeof what, "its bar shorter (%u lit pixels, were %u)", bars("VOL", "ARP"), k);
        check(k > 0 && bars("VOL", "ARP") < k, what);
        sim_run_ms(100);
        a = hold(KEY_C6, 200, &n);
        s0 = onset(a, n);
        after = frame_p2p(a, s0, 0);
        free(a);
        snprintf(what, sizeof what, "the next note's first frame: quieter (%.0f, was %.0f)", after, before);
        check(s0 < n && after > 0 && after < 0.4 * before, what);

        /* a key held while editing: step 07 (the release point, held at 8) up to 15 is heard at once */
        turn(K2, 7);
        sim_run_ms(100);
        free(left(&n));
        sim_key(KEY_C6, 100);
        sim_run_ms(300);
        turn(K5, 7);
        sim_run_ms(300);
        sim_key(KEY_C6, 0);
        a = left(&n);
        held = p2p(a, 0.15 * 44100, 0.29 * 44100);
        louder = p2p(a, 0.40 * 44100, 0.60 * 44100);
        snprintf(what, sizeof what, "held: step 07 8 -> 15 louder at once (%.0f, was %.0f)", louder, held);
        check(held > 0 && louder > 1.5 * held, what);
        free(a);
        sim_run_ms(100);
    }

    button(SMK_BTN_SEQ);
    check(!sim_button_led(SMK_BTN_PATCH) && sim_screen_text("0C"), "SEQ: back to the pattern view");

    /* loop and release: BASS's volume 15 9 6 3 0, looping at 00, released at 01 (as LOOPREL in the NES instruments
     * test); row 00 C-6 with it, row 01 a release */
    {
        static const int8_t LOOPREL[] = {15, 9, 6, 3, 0};
        int32_t *a;
        uint32_t n, k, ok;
        double s0, R = 44100.0 * 60 / (120 * 4), r1, lo;
        button(SMK_BTN_PATCH);
        tap(1);
        check(sim_screen_text("INS01") && sim_screen_text("BASS"), "pad 2 in the editor: INS01 BASS");
        edit_seq(PROJECT_SEQ_VOL, LOOPREL, 5, 0, 1);
        check(sim_screen_text("LEN 05") && sim_screen_text("LOOP 00") && sim_screen_text("REL 01"),
              "K6, K7, K8: LEN 05, LOOP 00, REL 01");
        button(SMK_BTN_SEQ);
        button(SMK_BTN_SEQ_REC);
        press(KEY_C6);
        sim_button(SMK_BTN_KNOB_BANK, 1);
        sim_run_ms(30);
        press(KEY_C6 - 2u);                  /* shift + a black key: a release */
        sim_button(SMK_BTN_KNOB_BANK, 0);
        sim_run_ms(30);
        button(SMK_BTN_SEQ_REC);
        sim_run_ms(100);
        free(left(&n));
        button(SMK_BTN_SEQ_PLAY);
        sim_run_ms(500);
        button(SMK_BTN_SEQ_PLAY);
        a = left(&n);
        s0 = onset(a, n);
        for (ok = 1, k = 0; k < 6u; k++)     /* row 00: 15 9 15 9 ... */
            ok &= near(frame_p2p(a, s0, k), frame_p2p(a, s0, k % 2u), 0.03);
        check(s0 < n && ok && frame_p2p(a, s0, 1) < 0.8 * frame_p2p(a, s0, 0), "held: 15 9 looping, a frame each");
        r1 = s0 + R;
        lo = frame_p2p(a, s0, 1);
        k = quiet_from(a, (uint32_t)(s0 + 3 * R));
        snprintf(what, sizeof what, "released: 6 3 0 (%.0f < %.0f), then silent (%.1f frames after it)",
                 p2p(a, r1 + 200, r1 + 0.75 * FRAME), lo, (k - r1) / FRAME);
        check(p2p(a, r1 + 200, r1 + 0.75 * FRAME) < lo && p2p(a, r1 + 200, r1 + 0.75 * FRAME) > 0 && k > r1 &&
                  k < r1 + 3.5 * FRAME,
              what);
        free(a);
    }

    /* renaming: BASS's fifth character (shift + K4 to it) from a space to 2 (K4: space, A to Z, 0 to 9, -) */
    button(SMK_BTN_PATCH);
    check(sim_screen_text("INS01") && sim_screen_text("BASS"), "PATCH again: the editor on BASS, the selected");
    sim_button(SMK_BTN_KNOB_BANK, 1);
    turn(K4, 4);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    turn(K4, 29);
    check(sim_screen_text("BASS2") && !sim_screen_text("BASS"), "K4 at the fifth character: BASS2 in the editor");
    button(SMK_BTN_SEQ);
    button(SMK_BTN_NOTE_REPEAT);
    check(sim_screen_text("INS01") && sim_screen_text("BASS2"), "the instrument picker: INS01 BASS2");
    button(SMK_BTN_NOTE_REPEAT);

    /* copying: shift + pad 8 (a free slot) in the editor on BASS2: INS07 BASS2, sounding as INS01 does */
    {
        int32_t *a;
        uint32_t n, k, ok;
        double s0, l1[10], l7[10], d1[10], d7[10];
        uint32_t c1[10], c7[10];
        button(SMK_BTN_PATCH);
        sim_button(SMK_BTN_KNOB_BANK, 1);
        tap(7);
        sim_button(SMK_BTN_KNOB_BANK, 0);
        check(sim_screen_text("INS07") && sim_screen_text("BASS2") && sim_screen_text("LOOP 00") &&
                  sim_screen_text("REL 01"),
              "shift + pad 8 (free): the copy INS07 BASS2 edited, its volume LOOP 00 REL 01");
        check(sim_pad_led(7) != 0u && sim_pad_led(1) != 0u && sim_pad_led(7) != sim_pad_led(1),
              "pad 8 lit now (the edited one), pad 2 too");
        sim_button(SMK_BTN_KNOB_BANK, 1);
        tap(0);
        sim_button(SMK_BTN_KNOB_BANK, 0);
        check(sim_screen_text("INS07") && !sim_screen_text("INS00"), "shift + pad 1 (LEAD, not free): no copy");
        a = hold(KEY_C6, 250, &n);
        s0 = onset(a, n);
        for (k = 0; k < 10u; k++) {
            l7[k] = frame_p2p(a, s0, k);
            d7[k] = duty(a, s0 + (k + 0.1) * FRAME, s0 + (k + 0.9) * FRAME);
            c7[k] = crossings(a, s0 + (k + 0.1) * FRAME, s0 + (k + 0.9) * FRAME);
        }
        free(a);
        tap(1);
        sim_run_ms(100);
        a = hold(KEY_C6, 250, &n);
        s0 = onset(a, n);
        for (k = 0; k < 10u; k++) {
            l1[k] = frame_p2p(a, s0, k);
            d1[k] = duty(a, s0 + (k + 0.1) * FRAME, s0 + (k + 0.9) * FRAME);
            c1[k] = crossings(a, s0 + (k + 0.1) * FRAME, s0 + (k + 0.9) * FRAME);
        }
        free(a);
        for (ok = 1, k = 0; k < 10u; k++)
            ok &= l1[k] > 0 && near(l7[k], l1[k], 0.02);
        snprintf(what, sizeof what, "INS07 and INS01: the same levels, frame by frame (%.0f %.0f .. / %.0f %.0f ..)",
                 l7[0], l7[1], l1[0], l1[1]);
        check(ok, what);
        for (ok = 1, k = 0; k < 10u; k++)
            ok &= c1[k] > 0 && (c7[k] + 1u >= c1[k] && c7[k] <= c1[k] + 1u) && fabs(d7[k] - d1[k]) < 0.03;
        snprintf(what, sizeof what, "INS07 and INS01: the same pitch and duty, frame by frame (%u %.2f / %u %.2f)",
                 c7[0], d7[0], c1[0], d1[0]);
        check(ok, what);
        button(SMK_BTN_SEQ);
    }

    printf(fails ? "V-TRACK EDITOR: %d FAILED\n" : "V-TRACK EDITOR: ok\n", fails);
    return fails != 0;
}
