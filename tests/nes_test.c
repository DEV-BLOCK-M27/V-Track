/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Seam B: the NES chip core (firmware/src/nes.c) alone. Per frame the channel's register state in, a block of
 * samples out; checked against the 2A03 (NTSC): the four duty waveforms, period register -> frequency, and no
 * aliasing on a high note.
 *   tests/nes_test */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NES_RATE 44100u
#include "../firmware/src/nes.c"

#define RATE 44100u
#define CPU_HZ 1789773.0                     /* the 2A03's NTSC CPU clock */
#define FRAME 735u                           /* samples per NES frame (~60 Hz) */

static int fails;
static void check(int ok, const char *what)
{
    printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* pulse 1 with this register state, n samples rendered in frames of FRAME (the register state set each frame) */
static int32_t *render_pulse(uint16_t period, uint8_t duty, uint8_t volume, uint32_t n)
{
    static nes_t chip;
    int32_t *a = calloc(n, sizeof *a);
    nes_pulse_t r = {period, duty, volume, 1};
    uint32_t i;
    nes_init(&chip);
    for (i = 0; i < n; i += FRAME) {
        nes_set_pulse(&chip, 0, &r);
        nes_render(&chip, a + i, n - i < FRAME ? n - i : FRAME);
    }
    return a;
}

static void range(const int32_t *a, uint32_t n, int32_t *lo, int32_t *hi)
{
    uint32_t i;
    *lo = *hi = a[0];
    for (i = 1; i < n; i++) {
        if (a[i] < *lo)
            *lo = a[i];
        if (a[i] > *hi)
            *hi = a[i];
    }
}

/* ------------------------------------------------------------ duty --- */
static int cmp32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* the duty waveforms: high for 1, 2, 4, 6 of the sequencer's 8 steps, in one piece (one rise a period); the step
 * between low and high the 2A03 pulse mixer's for volume 15 alone: 95.88 / (8128 / 15 + 100) = 0.1494 */
static void test_duty(void)
{
    static const double want[4] = {0.125, 0.25, 0.5, 0.75};   /* 2A03 duty 0..3 */
    const double periods = 44100.0 / (CPU_HZ / (16.0 * 0x1AC)), level = 0.1494 * 32767;
    char what[120];
    uint32_t d;
    for (d = 0; d < 4u; d++) {
        uint32_t n = RATE, i, high = 0, rises = 0, skip = 256, nl = 0, nh = 0;
        int32_t *a = render_pulse(0x1AB, (uint8_t)d, 15, n), lo, hi, *l = malloc(n * sizeof *l), *h = malloc(n * sizeof *h);
        double frac, mid, step;
        range(a + skip, n - skip, &lo, &hi);
        mid = (lo + hi) / 2.0;
        for (i = skip; i < n; i++) {
            high += a[i] > mid;
            rises += a[i - 1] <= mid && a[i] > mid;
            if (a[i] > mid)
                h[nh++] = a[i];
            else
                l[nl++] = a[i];
        }
        qsort(l, nl, sizeof *l, cmp32);
        qsort(h, nh, sizeof *h, cmp32);
        step = h[nh / 2] - l[nl / 2];              /* the medians: the flat parts, not the edges' ringing */
        frac = (double)high / (n - skip);
        snprintf(what, sizeof what, "duty %u: high %.1f %% (2A03: %.1f %%), %u rises in %.0f periods, step %.0f (%.0f)",
                 d, 100 * frac, 100 * want[d], rises, (n - skip) / periods, step, level);
        check(fabs(frac - want[d]) < 0.01 && fabs(rises - (n - skip) / periods) < 2 && fabs(step / level - 1) < 0.01,
              what);
        free(a);
        free(l);
        free(h);
    }
}

/* ------------------------------------------------------- frequency --- */
/* the frequency of a, from its rising crossings of the mean (interpolated between samples) */
static double frequency(const int32_t *a, uint32_t n)
{
    double mean = 0, first = -1, last = 0;
    uint32_t i, crossings = 0;
    for (i = 0; i < n; i++)
        mean += a[i];
    mean /= n;
    for (i = 1; i < n; i++)
        if (a[i - 1] <= mean && a[i] > mean) {
            double t = i - 1 + (mean - a[i - 1]) / (a[i] - a[i - 1]);
            if (first < 0)
                first = t;
            else
                crossings++;
            last = t;
        }
    return crossings ? crossings * (double)RATE / (last - first) : 0;
}

static void test_period(void)
{
    /* 2047 = the lowest; 0x1AB C4 and 0x0FD A4 (FamiTracker's table); 8 = the highest not muted */
    static const uint16_t periods[] = {0x7FF, 0x400, 0x1AB, 0x0FD, 0x080, 0x040, 0x020, 0x010, 0x008};
    char what[96];
    uint32_t i;
    for (i = 0; i < sizeof periods / sizeof *periods; i++) {
        uint32_t n = 2u * RATE, skip = 256;
        int32_t *a = render_pulse(periods[i], 2, 15, n);
        double want = CPU_HZ / (16.0 * (periods[i] + 1)), got = frequency(a + skip, n - skip);
        snprintf(what, sizeof what, "period $%03X: %.2f Hz (2A03 NTSC: %.2f Hz)", periods[i], got, want);
        check(fabs(got / want - 1) < 0.0005, what);
        free(a);
    }
    {   /* periods below 8: the sweep unit mutes the channel */
        uint32_t n = RATE / 10u, k, quiet = 1;
        for (i = 0; i < 8u; i++) {
            int32_t *a = render_pulse((uint16_t)i, 2, 15, n);
            for (k = 0; k < n; k++)
                quiet &= a[k] == 0;
            free(a);
        }
        check(quiet, "periods 0..7: muted");
    }
}

/* ------------------------------------------------------- aliasing --- */
#define FFT_N 32768u
static void fft(double *re, double *im, uint32_t n)    /* in place, radix 2 */
{
    uint32_t i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for (i = 0; i < n; i += len)
            for (j = 0; j < len / 2; j++) {
                double wr = cos(a * j), wi = sin(a * j);
                double *ur = &re[i + j], *ui = &im[i + j], *vr = &re[i + j + len / 2], *vi = &im[i + j + len / 2];
                double xr = *vr * wr - *vi * wi, xi = *vr * wi + *vi * wr;
                *vr = *ur - xr; *vi = *ui - xi;
                *ur += xr; *ui += xi;
            }
    }
}

/* A held high note: its energy outside its own harmonics below the band edge (aliases folded back from above
 * Nyquist land there) and above the band edge, each relative to the whole, in dB. A Blackman-Harris window keeps
 * the harmonics' own leakage below -90 dB. */
#define BAND_EDGE 20000.0
static void spectrum_check(uint16_t period, uint8_t duty)
{
    static double re[FFT_N], im[FFT_N];
    uint32_t i, skip = 4096;
    int32_t *a = render_pulse(period, duty, 15, FFT_N + skip);
    double f0 = CPU_HZ / (16.0 * (period + 1)), mean = 0, total = 0, inharm = 0, above = 0, bin = (double)RATE / FFT_N;
    char what[120];
    for (i = 0; i < FFT_N; i++)
        mean += a[skip + i];
    mean /= FFT_N;
    for (i = 0; i < FFT_N; i++) {
        double x = 2 * M_PI * i / FFT_N;
        double w = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
        re[i] = (a[skip + i] - mean) * w;
        im[i] = 0;
    }
    free(a);
    fft(re, im, FFT_N);
    for (i = 1; i < FFT_N / 2; i++) {
        double f = i * bin, e = re[i] * re[i] + im[i] * im[i], h = f / f0;
        total += e;
        if (f > BAND_EDGE)
            above += e;
        else if (fabs(h - floor(h + 0.5)) * f0 > 6 * bin)   /* not within 6 bins of a harmonic */
            inharm += e;
    }
    inharm = 10 * log10(inharm / total + 1e-30);
    above = 10 * log10(above / total + 1e-30);
    snprintf(what, sizeof what, "period $%03X duty %u (%.0f Hz): aliases %.1f dB, above %.0f kHz %.1f dB", period,
             duty, f0, inharm, BAND_EDGE / 1000, above);
    check(inharm < -60 && above < -60, what);
}

static void test_aliasing(void)
{
    spectrum_check(0x01A, 2);              /* ~C8, 4.1 kHz */
    spectrum_check(0x01A, 0);
    spectrum_check(0x00D, 1);              /* ~8 kHz */
    spectrum_check(0x035, 3);              /* ~C7, 2.1 kHz */
}

int main(void)
{
    test_duty();
    test_period();
    test_aliasing();
    printf(fails ? "NES CHIP CORE: %d FAILED\n" : "NES CHIP CORE: ok\n", fails);
    return fails != 0;
}
