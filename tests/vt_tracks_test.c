/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The four tracks on the simulated SMK-37 (seam A, tests/vt_sim.h): a new project's tracks 1 to 4 are NES Pulse 1,
 * Pulse 2, Triangle and Noise. K3 selects the track, the screen names its chip and channel, the keys play it live:
 * the pulses and the triangle at the 2A03's pitch for the note, the noise at one of its 16 periods (a higher key, a
 * shorter period). A note keeps the track it was played on until its key is released.
 *   tests/vt_tracks_test [OUT_DIR]   (OUT_DIR: the screen on track 3 as tracks.ppm) */
#include <math.h>
#include "vt_sim.h"

#define K3 2u                                /* encoder K3: track select */
#define KEY_C3 0u                            /* keybed key 0 = C3 */
#define KEY_C4 12u
#define KEY_E4 16u
#define CPU_HZ 1789773.0
#define HZ_C4 (CPU_HZ / (16.0 * (0x1AB + 1)))   /* FamiTracker's NTSC pulse period for C-4: 261.36 Hz */
#define HZ_E4 (CPU_HZ / (16.0 * (0x152 + 1)))   /* E-4: 329.97 Hz */
#define HZ_C4_TRI (CPU_HZ / (32.0 * (0x0D5 + 1)))   /* the triangle's C-4 (period $0D5): 261.36 Hz */

static const char *const NAME[4] = {"NES \xB7 PULSE 1", "NES \xB7 PULSE 2", "NES \xB7 TRIANGLE", "NES \xB7 NOISE"};
static const char *const SHOWN[4] = {"NES \u00B7 PULSE 1", "NES \u00B7 PULSE 2", "NES \u00B7 TRIANGLE",
                                     "NES \u00B7 NOISE"};
                                             /* (the screen's text is Latin-1, the report UTF-8) */

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

static int steady(const int32_t *a, uint32_t n)   /* one level throughout: no sound (0, or a held triangle step) */
{
    uint32_t i;
    for (i = 1; i < n; i++)
        if (a[i] != a[0])
            return 0;
    return 1;
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

static double crossings_per_s(const int32_t *a, uint32_t n)   /* crossings of the mean, either way, a second */
{
    double mean = 0;
    uint32_t i, c = 0;
    for (i = 0; i < n; i++)
        mean += a[i];
    mean /= n ? n : 1;
    for (i = 1; i < n; i++)
        c += (a[i - 1] <= mean) != (a[i] <= mean);
    return n ? c * 44100.0 / n : 0;
}

static int at(double got, double want) { return fabs(got / want - 1) < 0.002; }

static int named(uint32_t t)                 /* the screen names track t's chip and channel, and no other's */
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (sim_screen_text(NAME[i]) != (i == t))
            return 0;
    return 1;
}

/* key k held for ms: the tone it plays (frequency, 0 if not one tone), then released: steady after */
static double play(uint32_t k, uint32_t ms, int32_t **a, uint32_t *n)
{
    sim_key(k, 100);
    sim_run_ms(ms);
    *a = left(44100u / 10u, n);              /* from 100 ms on */
    return frequency(*a, *n);
}

static int released(uint32_t k)              /* key k up: from 50 ms on, steady */
{
    int32_t *a;
    uint32_t n;
    int ok;
    sim_key(k, 0);
    sim_run_ms(300);
    a = left(44100u / 20u, &n);
    ok = n > 10000u && steady(a, n);
    free(a);
    return ok;
}

int main(int argc, char **argv)
{
    char what[120], path[512];
    int32_t *a;
    uint32_t n, t;
    double f;

    sim_power_on();
    sim_run_ms(100);
    sim_audio(&n);
    check(named(0), "power-on: track 1, the screen shows NES \u00B7 PULSE 1");

    /* K3 walks the tracks; the pulses and the triangle play C4 at the 2A03's pitch, silent after release */
    for (t = 0; t < 3u; t++) {
        if (t) {
            sim_encoder(K3, 1);
            sim_run_ms(50);
            snprintf(what, sizeof what, "K3 +1: track %u, the screen shows %s", t + 1u, SHOWN[t]);
            check(named(t), what);
        }
        f = play(KEY_C4, 600, &a, &n);
        snprintf(what, sizeof what, "track %u: C4 held, one tone at %.2f Hz (2A03 C-4: %.2f Hz)", t + 1u, f,
                 t < 2u ? HZ_C4 : HZ_C4_TRI);
        check(at(f, t < 2u ? HZ_C4 : HZ_C4_TRI), what);
        free(a);
        snprintf(what, sizeof what, "track %u: C4 released, no sound", t + 1u);
        check(released(KEY_C4), what);
    }

    /* the noise: a higher key, a shorter period */
    sim_encoder(K3, 1);
    sim_run_ms(50);
    check(named(3), "K3 +1: track 4, the screen shows NES \u00B7 NOISE");
    {
        double lo, hi;
        play(KEY_C3, 600, &a, &n);
        lo = crossings_per_s(a, n);
        check(!steady(a, n), "track 4: C3 held, the noise sounds");
        free(a);
        check(released(KEY_C3), "track 4: C3 released, no sound");
        play(KEY_C4, 600, &a, &n);
        hi = crossings_per_s(a, n);
        free(a);
        check(released(KEY_C4), "track 4: C4 released, no sound");
        snprintf(what, sizeof what, "track 4: C4's noise %.0f crossings a second, C3's %.0f: a shorter period", hi, lo);
        check(hi > 10 * lo && lo > 100, what);
    }

    /* K3 stops at the ends */
    sim_encoder(K3, 2);
    sim_run_ms(50);
    check(named(3), "K3 +2 on track 4: still track 4");
    sim_encoder(K3, -5);
    sim_run_ms(50);
    check(named(0), "K3 -5: track 1");

    /* a note keeps its track: C4 held on track 1, K3 to track 2, E4 there; C4 released: E4 alone */
    sim_audio(&n);
    sim_key(KEY_C4, 100);
    sim_run_ms(200);
    sim_encoder(K3, 1);
    sim_run_ms(50);
    sim_key(KEY_E4, 100);
    sim_run_ms(300);
    sim_key(KEY_C4, 0);
    sim_run_ms(400);
    a = left(44100u * 650u / 1000u, &n);     /* from 100 ms after C4's release on */
    f = frequency(a, n);
    snprintf(what, sizeof what, "C4 on track 1, E4 on track 2, C4 released: E4 alone at %.2f Hz", f);
    check(at(f, HZ_E4), what);
    free(a);
    check(released(KEY_E4), "E4 released: no sound");

    if (argc > 1) {
        sim_encoder(K3, 1);
        sim_run_ms(50);
        snprintf(path, sizeof path, "%s/tracks.ppm", argv[1]);
        sim_save_ppm(path);
    }
    printf(fails ? "V-TRACK TRACKS: %d FAILED\n" : "V-TRACK TRACKS: ok\n", fails);
    return fails != 0;
}
