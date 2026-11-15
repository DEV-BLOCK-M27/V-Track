/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* One NES Pulse note on the simulated SMK-37 (seam A, tests/vt_sim.h): a key plays track 1, NES Pulse 1, at the
 * 2A03's pitch for that note while it is held, and is silent after release; a second key takes the channel over
 * (one note at a time).
 *   tests/vt_note_test [OUT_DIR]   (OUT_DIR: a held C4 as note.wav, for listening) */
#include <math.h>
#include "vt_sim.h"

#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_E4 16u
#define HZ_C4 (1789773.0 / (16.0 * (0x1AB + 1)))   /* FamiTracker's NTSC period for C-4: 261.36 Hz */
#define HZ_E4 (1789773.0 / (16.0 * (0x152 + 1)))   /* E-4: 329.97 Hz */

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

static void write_wav(const char *path, const int32_t *a, uint32_t n)   /* 16-bit mono */
{
    FILE *f = fopen(path, "wb");
    uint32_t i, bytes = 2u * n;
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0\x44\xac\0\0\x88\x58\x01\0\x02\0\x10\0data....";
    if (!f)
        return;
    h[4] = (uint8_t)(bytes + 36u); h[5] = (uint8_t)((bytes + 36u) >> 8); h[6] = (uint8_t)((bytes + 36u) >> 16);
    h[7] = (uint8_t)((bytes + 36u) >> 24);
    h[40] = (uint8_t)bytes; h[41] = (uint8_t)(bytes >> 8); h[42] = (uint8_t)(bytes >> 16); h[43] = (uint8_t)(bytes >> 24);
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        int16_t s = (int16_t)(a[i] >> 8);    /* 24-bit -> 16-bit */
        uint8_t b[2] = {(uint8_t)s, (uint8_t)((uint16_t)s >> 8)};
        fwrite(b, 1, 2, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    char what[96], path[512];
    int32_t *a;
    uint32_t n;
    double f;

    sim_power_on();
    sim_run_ms(100);
    a = left(0, &n);
    check(silent(a, n), "power-on: silent");
    free(a);

    /* C4 on track 1: NES Pulse 1 at the 2A03's C4 while held */
    sim_key(KEY_C4, 100);
    sim_run_ms(1000);
    a = left(44100u / 10u, &n);              /* from 100 ms on */
    f = frequency(a, n);
    snprintf(what, sizeof what, "C4 held: one tone at %.2f Hz (2A03 C-4: %.2f Hz)", f, HZ_C4);
    check(at(f, HZ_C4), what);
    if (argc > 1) {
        snprintf(path, sizeof path, "%s/note.wav", argv[1]);
        write_wav(path, a, n);
    }
    free(a);

    sim_key(KEY_C4, 0);
    sim_run_ms(300);
    a = left(44100u / 20u, &n);              /* from 50 ms after the release on */
    check(n > 10000u && silent(a, n), "C4 released: silent");
    free(a);

    /* a second key while the first is held takes the channel over */
    sim_key(KEY_C4, 100);
    sim_run_ms(300);
    sim_key(KEY_E4, 100);
    sim_run_ms(600);
    a = left(44100u * 400u / 1000u, &n);     /* from 100 ms after E4 on */
    f = frequency(a, n);
    snprintf(what, sizeof what, "C4 held, E4 pressed: one tone at %.2f Hz (2A03 E-4: %.2f Hz)", f, HZ_E4);
    check(at(f, HZ_E4), what);
    free(a);

    sim_key(KEY_C4, 0);                      /* the key that lost the channel: E4 plays on */
    sim_run_ms(400);
    a = left(44100u / 10u, &n);
    f = frequency(a, n);
    snprintf(what, sizeof what, "C4 released, E4 held: E4 plays on at %.2f Hz", f);
    check(at(f, HZ_E4), what);
    free(a);

    sim_key(KEY_E4, 0);
    sim_run_ms(300);
    a = left(44100u / 20u, &n);
    check(n > 10000u && silent(a, n), "E4 released: silent");
    free(a);

    printf(fails ? "V-TRACK NOTE: %d FAILED\n" : "V-TRACK NOTE: ok\n", fails);
    return fails != 0;
}
