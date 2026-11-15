/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The NES chip core: the 2A03's channels (NTSC) at register level. Seam B (tests/nes_test.c):
 *   nes_init(c)                    power-on: every channel silent
 *   nes_set_pulse(c, ch, &regs)    once per frame: pulse ch (0 = Pulse 1, 1 = Pulse 2) gets this register state
 *   nes_render(c, out, n)          n mono samples at NES_RATE, Q15 (1.0 = the 2A03 mixer's full scale)
 * It knows nothing of tracks, rows or instruments: the player (player.c) turns those into register states.
 * So far: the two pulse channels and the 2A03's pulse mixer; the triangle, the noise and the TND mixer follow. */
#ifndef NES_RATE
#define NES_RATE FS                  /* the output rate (vtrack.c) */
#endif
#define NES_CPU 1789773u             /* the 2A03's CPU clock, NTSC */
#define NES_FRAC 20                  /* sample times: Q20 */
#define NES_CHUNK 64u                /* samples rendered at a time */
/* samples per CPU clock, Q32 >> 8 (so 2 * 2048 periods of it fit 32 bits) */
#define NES_K ((uint32_t)((((uint64_t)NES_RATE << 32) / NES_CPU) >> 8))
#include "nes_blep.h"                /* NES_BLEP: the band-limited step (tools/gen_nes_blep.py) */

typedef struct {
    uint16_t period;                 /* the 11-bit timer period t: f = CPU / (16 (t + 1)); t < 8 is muted */
    uint8_t duty;                    /* 0..3: 12.5, 25, 50, 75 % */
    uint8_t volume;                  /* 0..15 (constant volume) */
    uint8_t enable;                  /* the channel's bit in $4015 */
} nes_pulse_t;

typedef struct {
    nes_pulse_t r;
    uint32_t step;                   /* samples per sequencer step (2 (t + 1) CPU clocks), Q20 */
    uint32_t next;                   /* the next sequencer step, Q20 from the start of the chunk */
    uint8_t seq;                     /* the sequencer's step 0..7 */
    uint8_t out;                     /* the channel's output now, 0..15 */
} nes_pulse_ch_t;

typedef struct {
    nes_pulse_ch_t p[2];
    int32_t level;                   /* the mixed output the steps have reached, Q15 */
    int32_t acc;                     /* the running sum of the steps: the output sample, Q(15 + NES_KBITS) */
    int32_t buf[NES_CHUNK + NES_TAPS];   /* the band-limited steps by sample; the tail runs into the next chunk */
} nes_t;

/* the 2A03's pulse mixer, 95.88 / (8128 / (p1 + p2) + 100), Q15 */
static const uint16_t NES_PULSE_MIX[31] = {
    0, 382, 754, 1118, 1474, 1821, 2160, 2491, 2815, 3132, 3442, 3745, 4042, 4332, 4616, 4895,
    5167, 5434, 5696, 5953, 6204, 6450, 6692, 6929, 7162, 7390, 7614, 7834, 8050, 8262, 8470};
/* the duty waveforms as the sequencer steps through them */
static const uint8_t NES_DUTY[4] = {0x02, 0x06, 0x1E, 0xF9};   /* bit s = step s: 01000000 01100000 01111000 10011111 */

static void nes_init(nes_t *c)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)c;
    for (i = 0; i < sizeof *c; i++)
        b[i] = 0;
    for (i = 0; i < 2u; i++)
        c->p[i].step = 2u * NES_K >> (32 - 8 - NES_FRAC);
}

static uint8_t nes__pulse_out(const nes_pulse_ch_t *p)
{
    if (!p->r.enable || p->r.period < 8u || !(NES_DUTY[p->r.duty & 3u] >> p->seq & 1u))
        return 0;
    return p->r.volume & 15u;
}

/* the output steps to the channels' mix at time t (Q20): a band-limited step, so nothing above Nyquist folds back */
static void nes__step(nes_t *c, uint32_t t)
{
    int32_t m = NES_PULSE_MIX[c->p[0].out + c->p[1].out], d = m - c->level;
    if (d) {
        /* the step between the two kernel phases around t, split by the rest of t: the two parts sum to d */
        const int16_t *k0 = NES_BLEP[t >> (NES_FRAC - NES_PHASE_BITS) & ((1u << NES_PHASE_BITS) - 1u)];
        const int16_t *k1 = k0 + NES_TAPS;
        int32_t d1 = d * (int32_t)(t >> (NES_FRAC - NES_PHASE_BITS - 8) & 255u) >> 8, d0 = d - d1;
        int32_t *b = &c->buf[t >> NES_FRAC];
        uint32_t i;
        for (i = 0; i < NES_TAPS; i++)
            b[i] += d0 * k0[i] + d1 * k1[i];
        c->level = m;
    }
}

static void nes_set_pulse(nes_t *c, uint32_t ch, const nes_pulse_t *r)
{
    nes_pulse_ch_t *p = &c->p[ch & 1u];
    if (r->period != p->r.period)
        p->step = (uint32_t)(2u * ((r->period & 0x7FFu) + 1u)) * NES_K >> (32 - 8 - NES_FRAC);
    p->r = *r;
    p->r.period &= 0x7FFu;
}

static void nes__chunk(nes_t *c, int32_t *out, uint32_t n)
{
    uint32_t end = n << NES_FRAC, i;
    for (i = 0; i < 2u; i++) {                 /* register writes take effect at the start */
        c->p[i].out = nes__pulse_out(&c->p[i]);
        nes__step(c, 0);
    }
    for (;;) {                                 /* the sequencer steps, in time order */
        nes_pulse_ch_t *p = c->p[1].next < c->p[0].next ? &c->p[1] : &c->p[0];
        uint32_t t = p->next;
        if (t >= end)
            break;
        p->seq = (uint8_t)((p->seq + 1u) & 7u);
        p->next += p->step;
        p->out = nes__pulse_out(p);
        nes__step(c, t);
    }
    for (i = 0; i < 2u; i++)
        c->p[i].next -= end;
    for (i = 0; i < n; i++) {
        c->acc += c->buf[i];
        out[i] = (c->acc + (1 << (NES_KBITS - 1))) >> NES_KBITS;
    }
    for (i = 0; i < NES_TAPS; i++) {             /* the tail to the start, for the next chunk */
        c->buf[i] = c->buf[n + i];
        c->buf[n + i] = 0;
    }
    for (; i < n; i++)
        c->buf[i] = 0;
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
