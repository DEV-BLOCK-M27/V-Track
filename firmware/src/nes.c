/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The NES chip core: the 2A03's channels (NTSC) at register level. Seam B (tests/nes_test.c):
 *   nes_init(c)                    power-on: every channel silent
 *   nes_set_pulse(c, ch, &regs)    once per frame: pulse ch (0 = Pulse 1, 1 = Pulse 2) gets this register state
 *   nes_set_triangle(c, &regs)     the same for the triangle
 *   nes_set_noise(c, &regs)        and for the noise
 *   nes_render(c, out, n)          n mono samples at NES_RATE, Q15 (1.0 = the 2A03 mixer's full scale)
 * It knows nothing of tracks, rows or instruments: the player (player.c) turns those into register states.
 * The channels are mixed by the 2A03's non-linear pulse and TND mixers (no DMC: d = 0), every change of the mix a
 * band-limited step. A disabled channel stops its timer until it is enabled again (on the 2A03 only the triangle's
 * does; the others' phase is the player's to set). */
#ifndef NES_RATE
#define NES_RATE FS                  /* the output rate (vtrack.c) */
#endif
#define NES_CPU 1789773u             /* the 2A03's CPU clock, NTSC */
#define NES_FRAC 20                  /* sample times: Q20 */
#define NES_CHUNK 64u                /* samples rendered at a time */
/* samples per CPU clock, Q32 >> 8 (so 2 * 2048 periods of it fit 32 bits) */
#define NES_K ((uint32_t)((((uint64_t)NES_RATE << 32) / NES_CPU) >> 8))
#include "nes_blep.h"                /* NES_BLEP: the band-limited step's residual (tools/gen_nes_blep.py) */

typedef struct {
    uint16_t period;                 /* the 11-bit timer period t: f = CPU / (16 (t + 1)); t < 8 is muted */
    uint8_t duty;                    /* 0..3: 12.5, 25, 50, 75 % */
    uint8_t volume;                  /* 0..15 (constant volume) */
    uint8_t enable;                  /* the channel's bit in $4015 */
} nes_pulse_t;

typedef struct {
    uint16_t period;                 /* the 11-bit timer period t: f = CPU / (32 (t + 1)); t < 2 stops it
                                      * (ultrasonic) */
    uint8_t enable;                  /* the channel's bit in $4015 with the linear counter running; off, the
                                      * sequencer stops where it is and its level stays. No volume: none on the 2A03 */
} nes_triangle_t;

typedef struct {
    uint8_t period;                  /* 0..15: one of the 16 NTSC periods (NES_NOISE_PERIOD) */
    uint8_t mode;                    /* 0 long (the 15-bit LFSR, 32767 steps), 1 short (93 steps: metallic) */
    uint8_t volume;                  /* 0..15 (constant volume) */
    uint8_t enable;                  /* the channel's bit in $4015 */
} nes_noise_t;

typedef struct {                     /* what every channel's timer has */
    uint32_t step;                   /* samples per sequencer step, Q20 */
    uint32_t next;                   /* the next sequencer step, Q20 from the start of the chunk */
    uint8_t seq;                     /* the sequencer's step */
    uint8_t out;                     /* the channel's output now, 0..15 */
    uint8_t run;                     /* the timer runs this chunk */
} nes_timer_t;

typedef struct {
    nes_timer_t tm;                  /* step: 2 (t + 1) CPU clocks; seq 0..7 */
    nes_pulse_t r;
} nes_pulse_ch_t;

typedef struct {
    nes_timer_t tm;                  /* step: t + 1 CPU clocks; seq 0..31 */
    nes_triangle_t r;
} nes_tri_ch_t;

typedef struct {
    nes_timer_t tm;                  /* step: the period's CPU clocks */
    nes_noise_t r;
    uint16_t lfsr;                   /* the shift register: high when bit 0 is clear */
} nes_noise_ch_t;

#define NES_CH 4u                    /* the channels' timers: Pulse 1, Pulse 2, Triangle, Noise */
typedef struct {
    nes_pulse_ch_t p[2];
    nes_tri_ch_t tri;
    nes_noise_ch_t noise;
    int32_t level;                   /* the mixed output the steps have reached, Q15 */
    int32_t acc;                     /* the running sum of the ideal steps, Q(15 + NES_KBITS) */
    int32_t step[NES_CHUNK + NES_TAPS];  /* the ideal steps by sample, Q(15 + NES_KBITS) */
    int32_t res[NES_CHUNK + NES_TAPS];   /* the steps' residuals (band-limited - ideal) by sample; the tails run
                                          * into the next chunk */
} nes_t;

/* the 2A03's pulse mixer, 95.88 / (8128 / (p1 + p2) + 100), Q15 */
static const uint16_t NES_PULSE_MIX[31] = {
    0, 382, 754, 1118, 1474, 1821, 2160, 2491, 2815, 3132, 3442, 3745, 4042, 4332, 4616, 4895,
    5167, 5434, 5696, 5953, 6204, 6450, 6692, 6929, 7162, 7390, 7614, 7834, 8050, 8262, 8470};
/* the 2A03's TND mixer, 159.79 / (1 / (t / 8227 + n / 12241) + 100), Q15, by t * 16 + n */
static const uint16_t NES_TND_MIX[256] = {
        0,   424,   842,  1252,  1657,  2055,  2446,  2832,  3212,  3586,  3954,  4317,  4675,  5027,  5374,  5716,
      629,  1043,  1451,  1852,  2247,  2635,  3018,  3395,  3766,  4132,  4492,  4847,  5197,  5541,  5881,  6215,
     1243,  1647,  2045,  2437,  2823,  3203,  3577,  3945,  4308,  4666,  5018,  5365,  5707,  6044,  6377,  6704,
     1842,  2237,  2626,  3009,  3386,  3757,  4123,  4483,  4838,  5188,  5533,  5872,  6207,  6537,  6862,  7183,
     2428,  2814,  3194,  3568,  3937,  4300,  4657,  5010,  5357,  5699,  6036,  6369,  6696,  7019,  7338,  7652,
     3000,  3377,  3749,  4114,  4475,  4830,  5180,  5525,  5864,  6199,  6529,  6855,  7175,  7492,  7804,  8111,
     3559,  3928,  4291,  4649,  5001,  5349,  5691,  6028,  6361,  6689,  7012,  7330,  7644,  7954,  8260,  8561,
     4106,  4466,  4821,  5171,  5516,  5856,  6191,  6521,  6847,  7168,  7484,  7796,  8104,  8407,  8707,  9002,
     4640,  4993,  5340,  5683,  6020,  6353,  6681,  7004,  7323,  7637,  7947,  8252,  8554,  8851,  9145,  9434,
     5163,  5508,  5848,  6183,  6513,  6839,  7160,  7476,  7789,  8096,  8400,  8699,  8995,  9286,  9574,  9857,
     5674,  6012,  6345,  6673,  6996,  7315,  7629,  7939,  8245,  8547,  8844,  9137,  9427,  9713,  9995, 10273,
     6175,  6505,  6831,  7152,  7469,  7781,  8089,  8393,  8692,  8988,  9279,  9567,  9851, 10131, 10407, 10680,
     6665,  6988,  7307,  7622,  7932,  8238,  8539,  8837,  9130,  9420,  9706,  9988, 10266, 10541, 10812, 11079,
     7145,  7461,  7774,  8082,  8385,  8685,  8981,  9272,  9560,  9844, 10124, 10400, 10673, 10943, 11209, 11471,
     7614,  7924,  8230,  8532,  8830,  9123,  9413,  9699,  9981, 10259, 10534, 10805, 11073, 11337, 11598, 11856,
     8074,  8378,  8678,  8973,  9265,  9553,  9837, 10117, 10394, 10667, 10936, 11202, 11465, 11724, 11980, 12233};
/* the noise periods, NTSC, in CPU clocks */
static const uint16_t NES_NOISE_PERIOD[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016,
                                              2034, 4068};
/* the duty waveforms as the sequencer steps through them */
static const uint8_t NES_DUTY[4] = {0x02, 0x06, 0x1E, 0xF9};   /* bit s = step s: 01000000 01100000 01111000 10011111 */

static uint32_t nes__clocks(uint32_t clocks)     /* CPU clocks -> samples, Q20 */
{
    return clocks * NES_K >> (32 - 8 - NES_FRAC);
}

static void nes_init(nes_t *c)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)c;
    for (i = 0; i < sizeof *c; i++)
        b[i] = 0;
    for (i = 0; i < 2u; i++)
        c->p[i].tm.step = nes__clocks(2u);
    c->tri.tm.step = nes__clocks(1u);
    c->tri.tm.seq = 15;              /* at a 0 step: silent until it first plays (the 2A03 powers up at 15) */
    c->noise.tm.step = nes__clocks(NES_NOISE_PERIOD[0]);
    c->noise.lfsr = 1;
}

/* the channel's timer: Pulse 1, Pulse 2, Triangle, Noise by number */
static nes_timer_t *nes__timer(nes_t *c, uint32_t i)
{
    return i < 2u ? &c->p[i].tm : i == 2u ? &c->tri.tm : &c->noise.tm;
}

/* channel i's output for its sequencer step now */
static uint8_t nes__out(const nes_t *c, uint32_t i)
{
    if (i < 2u) {
        const nes_pulse_ch_t *p = &c->p[i];
        if (!p->r.enable || p->r.period < 8u || !(NES_DUTY[p->r.duty & 3u] >> p->tm.seq & 1u))
            return 0;
        return p->r.volume & 15u;
    }
    if (i == 2u)
        return (uint8_t)(c->tri.tm.seq < 16u ? 15u - c->tri.tm.seq : c->tri.tm.seq - 16u);
    return c->noise.r.enable && !(c->noise.lfsr & 1u) ? c->noise.r.volume & 15u : 0u;
}

/* whether channel i's timer runs: while the channel is enabled, at volume 0 too (the 2A03's timers run on: the
 * pulses keep their phase); a pulse's muted periods below 8 and the triangle's below 2 stop it, as does disabling */
static uint8_t nes__runs(const nes_t *c, uint32_t i)
{
    if (i < 2u)
        return c->p[i].r.enable && c->p[i].r.period >= 8u;
    if (i == 2u)
        return c->tri.r.enable && c->tri.r.period >= 2u;
    return c->noise.r.enable;
}

static void nes__advance(nes_t *c, uint32_t i)   /* channel i's sequencer one step on */
{
    if (i < 3u) {
        nes_timer_t *t = nes__timer(c, i);
        t->seq = (uint8_t)((t->seq + 1u) & (i < 2u ? 7u : 31u));
    } else {                                     /* feedback: bit 0 ^ bit 1 (long) or bit 6 (short), into bit 14 */
        uint32_t x = c->noise.lfsr, fb = (x ^ x >> (c->noise.r.mode ? 6u : 1u)) & 1u;
        c->noise.lfsr = (uint16_t)(x >> 1 | fb << 14);
    }
}

/* the output steps to the channels' mix at time t (Q20): a band-limited step, so nothing above Nyquist folds back.
 * The ideal step goes to step[] (summed exactly: no drift), the band-limited one's difference from it to res[]. */
static void nes__step(nes_t *c, uint32_t t)
{
    int32_t m = NES_PULSE_MIX[c->p[0].tm.out + c->p[1].tm.out] + NES_TND_MIX[c->tri.tm.out * 16u + c->noise.tm.out];
    int32_t d = m - c->level;
    if (d) {
        /* the residual between the two kernel phases around t, split by the rest of t: the two parts sum to d */
        const int16_t *k0 = NES_BLEP[t >> (NES_FRAC - NES_PHASE_BITS) & ((1u << NES_PHASE_BITS) - 1u)];
        const int16_t *k1 = k0 + NES_TAPS;
        int32_t d1 = d * (int32_t)(t & ((1u << (NES_FRAC - NES_PHASE_BITS)) - 1u)) >> (NES_FRAC - NES_PHASE_BITS);
        int32_t d0 = d - d1;
        int32_t *b = &c->res[t >> NES_FRAC];
        uint32_t i;
        for (i = 0; i < NES_TAPS; i++)
            b[i] += d0 * k0[i] + d1 * k1[i];
        c->step[(t >> NES_FRAC) + NES_TAPS / 2u] += d * (1 << NES_KBITS);
        c->level = m;
    }
}

static void nes_set_pulse(nes_t *c, uint32_t ch, const nes_pulse_t *r)
{
    nes_pulse_ch_t *p = &c->p[ch & 1u];
    if (r->period != p->r.period)
        p->tm.step = nes__clocks(2u * ((r->period & 0x7FFu) + 1u));
    p->r = *r;
    p->r.period &= 0x7FFu;
}

static void nes_set_triangle(nes_t *c, const nes_triangle_t *r)
{
    nes_tri_ch_t *t = &c->tri;
    if (r->period != t->r.period)
        t->tm.step = nes__clocks((r->period & 0x7FFu) + 1u);
    t->r = *r;
    t->r.period &= 0x7FFu;
}

static void nes_set_noise(nes_t *c, const nes_noise_t *r)
{
    nes_noise_ch_t *x = &c->noise;
    x->r = *r;
    x->r.period &= 15u;
    x->tm.step = nes__clocks(NES_NOISE_PERIOD[x->r.period]);
}

static void nes__chunk(nes_t *c, int32_t *out, uint32_t n)
{
    uint32_t end = n << NES_FRAC, i;
    for (i = 0; i < NES_CH; i++) {             /* register writes take effect at the start */
        nes_timer_t *t = nes__timer(c, i);
        t->run = nes__runs(c, i);
        t->out = nes__out(c, i);
        nes__step(c, 0);
    }
    for (;;) {                                 /* the sequencer steps, in time order */
        uint32_t k = NES_CH, t = end;
        for (i = 0; i < NES_CH; i++) {
            const nes_timer_t *x = nes__timer(c, i);
            if (x->run && x->next < t) {
                t = x->next;
                k = i;
            }
        }
        if (k == NES_CH)
            break;
        {
            nes_timer_t *x = nes__timer(c, k);
            nes__advance(c, k);
            x->next += x->step;
            x->out = nes__out(c, k);
        }
        nes__step(c, t);
    }
    for (i = 0; i < NES_CH; i++)               /* a stopped timer keeps what was left of its step */
        if (nes__timer(c, i)->run)
            nes__timer(c, i)->next -= end;
    for (i = 0; i < n; i++) {
        c->acc += c->step[i];
        out[i] = ((c->acc >> 1) + (c->res[i] >> 1) + (1 << (NES_KBITS - 2))) >> (NES_KBITS - 1);   /* (halved: the
                                                                                     * sum can pass 2^31) */
    }
    for (i = 0; i < NES_TAPS; i++) {             /* the tails to the start, for the next chunk */
        c->step[i] = c->step[n + i];
        c->res[i] = c->res[n + i];
        c->step[n + i] = c->res[n + i] = 0;
    }
    for (; i < n; i++)
        c->step[i] = c->res[i] = 0;
}

static void nes_render(nes_t *c, int32_t *out, uint32_t n)
{
    while (n) {
        uint32_t k = n < NES_CHUNK ? n : NES_CHUNK;
        nes__chunk(c, out, k);
        out += k;
        n -= k;
    }
}
