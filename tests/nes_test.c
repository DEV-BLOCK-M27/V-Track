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

/* every channel's register state */
typedef struct {
    nes_pulse_t p[2];
    nes_triangle_t t;
    nes_noise_t n;
} regs_t;
static const regs_t NONE;                          /* every channel off */

/* from power-on, n samples rendered in frames of FRAME, the register state set each frame */
static nes_t chip;
static int32_t *render(const regs_t *r, uint32_t n)
{
    int32_t *a = calloc(n, sizeof *a);
    uint32_t i;
    nes_init(&chip);
    for (i = 0; i < n; i += FRAME) {
        nes_set_pulse(&chip, 0, &r->p[0]);
        nes_set_pulse(&chip, 1, &r->p[1]);
        nes_set_triangle(&chip, &r->t);
        nes_set_noise(&chip, &r->n);
        nes_render(&chip, a + i, n - i < FRAME ? n - i : FRAME);
    }
    return a;
}

/* pulse 1 with this register state alone */
static int32_t *render_pulse(uint16_t period, uint8_t duty, uint8_t volume, uint32_t n)
{
    regs_t r = NONE;
    r.p[0] = (nes_pulse_t){period, duty, volume, 1};
    return render(&r, n);
}

/* the 2A03's mixer (nesdev "APU Mixer"), Q15: pulse 95.88 / (8128 / (p1 + p2) + 100), triangle / noise / DMC
 * 159.79 / (1 / (t / 8227 + n / 12241 + d / 22638) + 100); 0 for no input */
static double pulse_mix(int p) { return p ? 95.88 / (8128.0 / p + 100) * 32767 : 0; }
static double tnd_mix(int t, int n) { return t || n ? 159.79 / (1 / (t / 8227.0 + n / 12241.0) + 100) * 32767 : 0; }
static int nearest_tnd(double v, int n)    /* the triangle level 0..15 whose mix with noise n is nearest to v */
{
    int k, best = 0;
    for (k = 1; k < 16; k++)
        if (fabs(tnd_mix(k, n) - v) < fabs(tnd_mix(best, n) - v))
            best = k;
    return best;
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
#define SPECTRUM_SKIP 4096u
/* a: FFT_N + SPECTRUM_SKIP samples of a tone at f0 (freed); its aliases and the energy above the band edge in dB */
static void spectrum(int32_t *a, double f0, double *alias, double *band)
{
    static double re[FFT_N], im[FFT_N];
    uint32_t i, skip = SPECTRUM_SKIP;
    double mean = 0, total = 0, inharm = 0, above = 0, bin = (double)RATE / FFT_N;
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
    *alias = 10 * log10(inharm / total + 1e-30);
    *band = 10 * log10(above / total + 1e-30);
}

static void spectrum_check(uint16_t period, uint8_t duty)
{
    double f0 = CPU_HZ / (16.0 * (period + 1)), inharm, above;
    char what[120];
    spectrum(render_pulse(period, duty, 15, FFT_N + SPECTRUM_SKIP), f0, &inharm, &above);
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

/* ------------------------------------------------------- triangle --- */
/* the plateaus of a staircase: runs of one level (within 3 of a step's mid-value), each its level index and length */
typedef struct { int k; uint32_t len; } run_t;
static uint32_t plateaus(const int32_t *a, uint32_t n, int noise, run_t *runs, uint32_t max)
{
    uint32_t i, nr = 0;
    for (i = 0; i < n; i++) {
        int k = nearest_tnd(a[i], noise);
        if (fabs(a[i] - tnd_mix(k, noise)) > 3)
            continue;                                  /* an edge's ringing */
        if (nr && runs[nr - 1].k == k)
            runs[nr - 1].len++;
        else if (nr < max)
            runs[nr++] = (run_t){k, 1};
    }
    return nr;
}

/* the triangle's 32-step staircase 15 .. 0 0 .. 15, each step at the 2A03 mixer's level for it; there is no volume:
 * it always spans the whole 0..15 */
static void test_triangle_steps(void)
{
    static run_t runs[2048];
    regs_t r = NONE;
    uint32_t n = RATE, nr, i, ok = 1, down = 0, up = 0, lo = 0, hi = 0, mid = 0;
    int32_t *a;
    char what[120];
    r.t = (nes_triangle_t){0x7FF, 1};              /* 2048 CPU clocks a step: ~50 samples */
    a = render(&r, n);
    nr = plateaus(a + 256, n - 256, 0, runs, 2048);
    for (i = 2; i + 1 < nr; i++) {                 /* (the first and last runs are cut off) */
        int d = runs[i].k - runs[i - 1].k;
        if (runs[i].k == 0 || runs[i].k == 15) {   /* the turns: two steps at 0, two at 15 */
            ok &= runs[i].len > 75u && runs[i].len < 105u;   /* (the edges ring for a few samples) */
            runs[i].k ? hi++ : lo++;
        } else {
            ok &= runs[i].len > 30u && runs[i].len < 52u;
            mid++;
        }
        ok &= d == 1 || d == -1;
        d < 0 ? down++ : up++;
    }
    snprintf(what, sizeof what, "triangle: %u plateaus, %u down %u up, %u turns at 0, %u at 15, at the mixer's levels",
             nr, down, up, lo, hi);
    check(ok && nr > 500u && lo > 15u && hi > 15u && mid > 400u, what);
    free(a);
}

static void test_triangle_period(void)
{
    static const uint16_t periods[] = {0x7FF, 0x400, 0x1AB, 0x0FD, 0x040, 0x010, 0x008};
    char what[96];
    uint32_t i;
    for (i = 0; i < sizeof periods / sizeof *periods; i++) {
        uint32_t n = 2u * RATE, skip = 256;
        regs_t r = NONE;
        int32_t *a;
        double want = CPU_HZ / (32.0 * (periods[i] + 1)), got;
        r.t = (nes_triangle_t){periods[i], 1};
        a = render(&r, n);
        got = frequency(a + skip, n - skip);
        snprintf(what, sizeof what, "triangle period $%03X: %.2f Hz (2A03 NTSC: %.2f Hz)", periods[i], got, want);
        check(fabs(got / want - 1) < 0.0005, what);
        free(a);
    }
}

/* high triangle notes: as for the pulse, nothing between the harmonics, nothing above the band edge */
static void test_triangle_aliasing(void)
{
    static const uint16_t periods[] = {0x01A, 0x00D, 0x006};      /* ~C7 2.1 kHz, ~4 kHz, ~8 kHz */
    char what[120];
    uint32_t i;
    for (i = 0; i < sizeof periods / sizeof *periods; i++) {
        regs_t r = NONE;
        double f0 = CPU_HZ / (32.0 * (periods[i] + 1)), inharm, above;
        r.t = (nes_triangle_t){periods[i], 1};
        spectrum(render(&r, FFT_N + SPECTRUM_SKIP), f0, &inharm, &above);
        snprintf(what, sizeof what, "triangle period $%03X (%.0f Hz): aliases %.1f dB, above %.0f kHz %.1f dB",
                 periods[i], f0, inharm, BAND_EDGE / 1000, above);
        check(inharm < -60 && above < -60, what);
    }
}

/* disabled (the linear counter at 0), the triangle's sequencer stops where it is: its level stays, no tone */
static void test_triangle_halt(void)
{
    regs_t r = NONE;
    uint32_t i, n = RATE / 2u, steady = 1;
    int32_t *a = calloc(n, sizeof *a), level;
    nes_init(&chip);
    r.t = (nes_triangle_t){0x0FD, 1};
    for (i = 0; i < n; i += FRAME) {
        r.t.enable = i < n / 2u;
        nes_set_triangle(&chip, &r.t);
        nes_render(&chip, a + i, n - i < FRAME ? n - i : FRAME);
    }
    level = a[n / 2u + FRAME];
    for (i = n / 2u + FRAME; i < n; i++)
        steady &= a[i] == level;
    check(steady && fabs(level - tnd_mix(nearest_tnd(level, 0), 0)) <= 2,
          "triangle disabled: it holds its step's level, no tone");
    {   /* power-on: silent until it first plays */
        int32_t *z = calloc(FRAME, sizeof *z);
        nes_init(&chip);
        nes_render(&chip, z, FRAME);
        for (i = 0, steady = 1; i < FRAME; i++)
            steady &= z[i] == 0;
        check(steady, "triangle at power-on: silent");
        free(z);
    }
    free(a);
}

/* the medians of a's samples above and below mid: its two levels, the edges left out */
static void levels(const int32_t *a, uint32_t n, double mid, int32_t *lo, int32_t *hi)
{
    int32_t *h = malloc(n * sizeof *h), *l = malloc(n * sizeof *l);
    uint32_t i, nh = 0, nl = 0;
    for (i = 0; i < n; i++) {
        if (a[i] > mid)
            h[nh++] = a[i];
        else
            l[nl++] = a[i];
    }
    qsort(h, nh, sizeof *h, cmp32);
    qsort(l, nl, sizeof *l, cmp32);
    *hi = nh ? h[nh / 2] : 0;
    *lo = nl ? l[nl / 2] : 0;
    free(h);
    free(l);
}

/* ---------------------------------------------------------- noise --- */
/* the 2A03's noise periods, NTSC, in CPU clocks (nesdev "APU Noise") */
static const uint16_t NOISE_NTSC[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};

static int32_t *render_noise(uint8_t period, uint8_t mode, uint8_t volume, uint32_t n)
{
    regs_t r = NONE;
    r.n = (nes_noise_t){period, mode, volume, 1};
    return render(&r, n);
}

/* the times of a two-level signal's edges (crossings of mid, interpolated), up to max; their number */
static uint32_t edges(const int32_t *a, uint32_t n, double mid, double *e, uint32_t max)
{
    uint32_t i, ne = 0;
    for (i = 1; i < n && ne < max; i++)
        if ((a[i - 1] <= mid) != (a[i] <= mid))
            e[ne++] = i - 1 + (mid - a[i - 1]) / (a[i] - a[i - 1]);
    return ne;
}

/* the 16 periods. The slow ones (5 samples a step and more): every edge on one grid of steps, its spacing (fitted)
 * the period. The fast ones: short mode repeats every 93 steps, a tone at CPU / (93 period) with nothing between its
 * harmonics */
static void test_noise_periods(void)
{
    static double e[40000];
    char what[120];
    uint32_t p;
    for (p = 0; p < 16u; p++) {
        double step = RATE * NOISE_NTSC[p] / CPU_HZ;
        if (step > 4.9) {
            uint32_t n = 2u * RATE, ne, i, ok = 1;
            int32_t *a = render_noise((uint8_t)p, 0, 15, n);
            double worst = 0, sk = 0, sx = 0, skk = 0, skx = 0, fit, x0;
            ne = edges(a + 256, n - 256, tnd_mix(0, 15) / 2, e, 40000);
            for (i = 0; i < ne; i++) {                 /* least squares: e = x0 + k fit, k the step number */
                double k = floor((e[i] - e[0]) / step + 0.5);
                sk += k; sx += e[i]; skk += k * k; skx += k * e[i];
            }
            fit = (ne * skx - sk * sx) / (ne * skk - sk * sk);
            x0 = (sx - fit * sk) / ne;
            for (i = 0; i < ne; i++) {
                double k = (e[i] - x0) / fit;
                worst = fmax(worst, fabs(k - floor(k + 0.5)) * fit);
            }
            ok = ne > 100u && worst < 0.05 && fabs(fit / step - 1) < 0.0005;
            snprintf(what, sizeof what, "noise period %2u (%4u clocks): %u edges, step %.4f (%.4f), all within %.3f",
                     p, NOISE_NTSC[p], ne, fit, step, worst);
            check(ok, what);
            free(a);
        }
        if (step < 9) {
            double f0 = CPU_HZ / (93.0 * NOISE_NTSC[p]), inharm, above;
            spectrum(render_noise((uint8_t)p, 1, 15, FFT_N + SPECTRUM_SKIP), f0, &inharm, &above);
            snprintf(what, sizeof what, "noise period %2u short (%.1f Hz): aliases %.1f dB, above %.0f kHz %.1f dB", p,
                     f0, inharm, BAND_EDGE / 1000, above);
            check(inharm < -60 && above < -60, what);
        }
    }
}

/* the bits the noise played: its level at the middle of each step (on the grid of its edges), 1 = high */
static uint32_t noise_bits(uint8_t period, uint8_t mode, uint32_t steps, uint8_t *bits)
{
    double step = RATE * NOISE_NTSC[period] / CPU_HZ, e0;
    uint32_t n = (uint32_t)((steps + 8u) * step) + 1024u, i;
    int32_t *a = render_noise(period, mode, 15, n);
    edges(a + 256, n - 256, tnd_mix(0, 15) / 2, &e0, 1);
    e0 += 256;
    for (i = 0; i < steps; i++) {
        uint32_t t = (uint32_t)(e0 + (i + 0.5) * step);
        if (t >= n)
            break;
        bits[i] = a[t] > tnd_mix(0, 15) / 2;
    }
    free(a);
    return i;
}

/* the 2A03's LFSR: 15 bits from 1, feedback bit 0 ^ bit 1 (long) or bit 6 (short) into bit 14; high when bit 0 is
 * clear. Its output from power-on, n steps */
static void lfsr(uint8_t mode, uint8_t *bits, uint32_t n)
{
    uint32_t x = 1, i;
    for (i = 0; i < n; i++) {
        uint32_t fb = (x ^ x >> (mode ? 6 : 1)) & 1u;
        x = x >> 1 | fb << 14;
        bits[i] = !(x & 1u);
    }
}

/* the decoded bits: a stretch of the LFSR's output, repeating every 32767 (long) or 93 (short) steps and no less */
static void test_noise_lfsr(void)
{
    static uint8_t got[70000], want[2 * 70000];
    static const uint32_t length[2] = {32767, 93};
    char what[120];
    uint32_t m;
    for (m = 0; m < 2u; m++) {
        uint32_t n = noise_bits(8, (uint8_t)m, 2u * length[m] + 200u, got), i, off, at = ~0u, rep, shortest = 0;
        lfsr((uint8_t)m, want, 2u * 70000u);
        for (off = 0; off < 70000u && at == ~0u; off++) {
            for (i = 0; i < n && got[i] == want[off + i]; i++)
                ;
            if (i == n)
                at = off;
        }
        for (rep = 1; rep <= length[m] && !shortest; rep++) {
            for (i = 0; i + rep < n && got[i] == got[i + rep]; i++)
                ;
            if (i + rep == n)
                shortest = rep;
        }
        snprintf(what, sizeof what, "noise %s mode: %u steps, the 2A03 LFSR's output (at step %d), repeating every %u",
                 m ? "short" : "long", n, (int)at, shortest);
        check(n == 2u * length[m] + 200u && at != ~0u && shortest == length[m], what);
    }
}

/* the noise's level is its 4-bit volume through the TND mixer; volume 0 or disabled: silent */
static void test_noise_volume(void)
{
    static const uint8_t vols[] = {1, 4, 8, 15};
    char what[120];
    uint32_t v, i;
    for (v = 0; v < sizeof vols; v++) {
        uint32_t n = RATE / 4u;
        int32_t *a = render_noise(11, 0, vols[v], n), lo, hi;
        levels(a + 256, n - 256, tnd_mix(0, vols[v]) / 2, &lo, &hi);
        snprintf(what, sizeof what, "noise volume %2u: high %d (TND mixer: %.0f), low %d", vols[v], hi,
                 tnd_mix(0, vols[v]), lo);
        check(fabs(hi - tnd_mix(0, vols[v])) <= 2 && abs(lo) <= 2, what);
        free(a);
    }
    {
        int32_t *a = render_noise(11, 0, 0, RATE / 10u), *b;
        regs_t r = NONE;
        uint32_t quiet = 1;
        r.n = (nes_noise_t){11, 0, 15, 0};
        b = render(&r, RATE / 10u);
        for (i = 0; i < RATE / 10u; i++)
            quiet &= a[i] == 0 && b[i] == 0;
        check(quiet, "noise volume 0, noise disabled: silent");
        free(a);
        free(b);
    }
}

/* ---------------------------------------------------------- mixer --- */
/* the two pulses: one mixer for both, 95.88 / (8128 / (p1 + p2) + 100), not the sum of each alone */
static void test_mix_pulse(void)
{
    static const uint8_t v2[] = {15, 8, 1};
    char what[120];
    uint32_t k, n = RATE / 2u;
    for (k = 0; k < sizeof v2; k++) {
        regs_t r = NONE;
        int32_t *a, lo, hi;
        r.p[0] = (nes_pulse_t){0x1AB, 2, 15, 1};     /* the same period and duty from power-on: in phase */
        r.p[1] = (nes_pulse_t){0x1AB, 2, v2[k], 1};
        a = render(&r, n);
        levels(a + 256, n - 256, pulse_mix(15 + v2[k]) / 2, &lo, &hi);
        snprintf(what, sizeof what, "pulse 15 + pulse %2u: step %d (2A03 mixer: %.0f; each alone summed: %.0f)", v2[k],
                 hi - lo, pulse_mix(15 + v2[k]), pulse_mix(15) + pulse_mix(v2[k]));
        check(abs(lo) <= 2 && fabs(hi - lo - pulse_mix(15 + v2[k])) <= 2, what);
        free(a);
    }
}

/* a channel at volume 0 keeps its timer running, as on the 2A03: pulse 2 silent for a while, then back, is still in
 * phase with pulse 1 (the same period from power-on): one step of the two together */
static void test_mix_volume0_phase(void)
{
    regs_t r = NONE;
    uint32_t i, n = RATE;
    int32_t *a = calloc(n, sizeof *a), lo, hi;
    char what[120];
    nes_init(&chip);
    for (i = 0; i < n; i += FRAME) {
        r.p[0] = (nes_pulse_t){0x1AB, 2, 15, 1};
        r.p[1] = (nes_pulse_t){0x1AB, 2, (uint8_t)(i >= 7u * FRAME && i < 20u * FRAME ? 0 : 15), 1};
        nes_set_pulse(&chip, 0, &r.p[0]);
        nes_set_pulse(&chip, 1, &r.p[1]);
        nes_render(&chip, a + i, n - i < FRAME ? n - i : FRAME);
    }
    levels(a + 21u * FRAME, n - 21u * FRAME, pulse_mix(30) / 2, &lo, &hi);
    snprintf(what, sizeof what, "pulse 2 at volume 0 for 13 frames, back: step %d, in phase (2A03 mixer: %.0f)", hi - lo,
             pulse_mix(30));
    check(abs(lo) <= 2 && fabs(hi - lo - pulse_mix(30)) <= 2, what);
    free(a);
}

/* the triangle held at a level t, then the noise over it: its step tnd(t, n) - tnd(t, 0) depends on t (one
 * mixer for triangle and noise); then a pulse over it: the pulse group adds to the TND group */
static void test_mix_tnd(void)
{
    regs_t r = NONE;
    uint32_t i, n = RATE, part = 20u * FRAME;
    int32_t *a = calloc(n, sizeof *a), lo, hi, t;
    char what[140];
    nes_init(&chip);
    for (i = 0; i < n; i += FRAME) {
        r.t = (nes_triangle_t){0x0E0, i < 3u * FRAME};       /* 3 frames, then held */
        r.n = (nes_noise_t){11, 0, 15, i >= part && i < 2u * part};
        r.p[0] = (nes_pulse_t){0x1AB, 2, 15, i >= 2u * part};
        nes_set_pulse(&chip, 0, &r.p[0]);
        nes_set_triangle(&chip, &r.t);
        nes_set_noise(&chip, &r.n);
        nes_render(&chip, a + i, n - i < FRAME ? n - i : FRAME);
    }
    t = nearest_tnd(a[part - 1u], 0);
    snprintf(what, sizeof what, "triangle held at step %d: %d (TND mixer: %.0f)", t, a[part - 1u], tnd_mix(t, 0));
    check(t >= 3 && fabs(a[part - 1u] - tnd_mix(t, 0)) <= 2, what);
    levels(a + part + 256u, part - 256u, (tnd_mix(t, 0) + tnd_mix(t, 15)) / 2, &lo, &hi);
    snprintf(what, sizeof what, "noise 15 over it: %d .. %d (TND mixer: %.0f .. %.0f; noise alone a step of %.0f)", lo,
             hi, tnd_mix(t, 0), tnd_mix(t, 15), tnd_mix(0, 15));
    check(fabs(lo - tnd_mix(t, 0)) <= 2 && fabs(hi - tnd_mix(t, 15)) <= 2, what);
    levels(a + 2u * part + 256u, n - 2u * part - 256u, tnd_mix(t, 0) + pulse_mix(15) / 2, &lo, &hi);
    snprintf(what, sizeof what, "pulse 15 over it: %d .. %d (pulse + TND mixer: %.0f .. %.0f)", lo, hi, tnd_mix(t, 0),
             tnd_mix(t, 0) + pulse_mix(15));
    check(fabs(lo - tnd_mix(t, 0)) <= 2 && fabs(hi - tnd_mix(t, 0) - pulse_mix(15)) <= 2, what);
    free(a);
}

int main(void)
{
    test_duty();
    test_period();
    test_aliasing();
    test_triangle_steps();
    test_triangle_period();
    test_triangle_halt();
    test_triangle_aliasing();
    test_noise_periods();
    test_noise_lfsr();
    test_noise_volume();
    test_mix_pulse();
    test_mix_volume0_phase();
    test_mix_tnd();
    printf(fails ? "NES CHIP CORE: %d FAILED\n" : "NES CHIP CORE: ok\n", fails);
    return fails != 0;
}
