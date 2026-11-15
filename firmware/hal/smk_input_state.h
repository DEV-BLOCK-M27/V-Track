/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * SMK-37 Pro port: Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* SMK-37 Pro input, the part without registers: the input state (fm1_in), the key and pad events, the velocity
 * curves and the main-loop API (fm1_in, fm1_enc_take, fm1_input_edges, fm1_input_note_edges, smk_extkey_take,
 * fm1_led, fm1_led_key). smk_input.h scans the hardware into this state; the host simulator (tests/vt_sim.h) sets it
 * from its input events, so everything above it runs as on the device. SMK_INPUT_LOCK / SMK_INPUT_UNLOCK: the
 * main-loop API's IRQ lock (default: cli / sti). */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

#define FM1_NCOL 16u
#define FM1_NBTN 18u
#define FM1_NKEY 37u              /* the keybed */
#define FM1_NENC 8u
#define SMK_NPAD 16u
#define SMK_NLED 19u
#define SMK_KEY_F3 5u             /* keybed key 5 = F3: keys 5..31 are the FM-1's 27 note keys (bits 0..26) */
#define SMK_DEB_RELEASE 4u        /* frames open in a row: a button release (~6 ms) */
#define SMK_DEB_PRESS 2u          /* frames closed in a row: a button press (~3 ms) */
#define SMK_KEY_REL 3u            /* frames with contact 1 open: a key release (~5 ms) */
#define SMK_ENC_DIV 4             /* decoder counts a step: a detent is a whole quadrature cycle (the stock's 2
                                   * gave two UI steps a click, as the FM-1 has one) */
enum { FM1_BTN_OCT_DOWN = 4, FM1_BTN_OCT_UP = 5 };
/* button ids by their printed names (smk_input.h SMK_BTNC; the LED of button i is LED i, SEQ PLAY's green) */
enum { SMK_BTN_KNOB_BANK = 0, SMK_BTN_FADER_BANK = 1, SMK_BTN_LEFT = 2, SMK_BTN_RIGHT = 3, SMK_BTN_OCT_DOWN = 4,
       SMK_BTN_OCT_UP = 5, SMK_BTN_ARP = 6, SMK_BTN_NOTE_REPEAT = 7, SMK_BTN_SCALE = 8, SMK_BTN_CHORD = 9,
       SMK_BTN_GLOBE = 10, SMK_BTN_BT = 11, SMK_BTN_PATCH = 12, SMK_BTN_PARA = 13, SMK_BTN_FX = 14, SMK_BTN_SEQ = 15,
       SMK_BTN_SEQ_PLAY = 16, SMK_BTN_SEQ_REC = 17 };
#define FM1_OCT_BOTH (1u << FM1_BTN_OCT_DOWN | 1u << FM1_BTN_OCT_UP)

/* the pads are the 16 white keys of the FM-1's 27 (F3 G3 A3 B3 C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5): the step
 * grid, the drum sounds and every layer's tiles land on them, low notes at the bottom: the bottom row
 * (hardware pads 8..15, mux B; padtest on a unit, 2026-10-10) white keys 0..7, the top row (pads 0..7, mux A) 8..15 */
static const uint8_t SMK_WHITE_NOTE[SMK_NPAD] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static volatile uint8_t smk_pad_grid;   /* set by the UI: pads as a step grid, white key w on pad w (1-8 top row) */
#define SMK_PAD_OF_WHITE(w) (smk_pad_grid ? (w) : (w) < 8u ? (w) + 8u : (w) - 8u)   /* white key 0..15 -> hardware pad */
/* the piano layout (the UI sets smk_pad_piano, e.g. for SCALE): top row C4 D4 E4 F4 G4 A4 B4 C5, bottom row the
 * black keys between them (C#4 D#4 - F#4 G#4 A#4 - C#5); note-key index (0 = F3) by hardware pad, -1 = none */
static const int8_t SMK_PIANO_NOTE[SMK_NPAD] = {7, 9, 11, 12, 14, 16, 18, 19, 8, 10, -1, 13, 15, 17, -1, 20};
static volatile uint8_t smk_pad_piano;
static uint8_t smk_pad_note[SMK_NPAD];   /* the note key a pad pressed (+1; 0 = none): its release ends that one */
#define SMK_WHITE_OF_PAD(p) (smk_pad_grid ? (p) : (p) < 8u ? (p) + 8u : (p) - 8u)

#define SMK_TRIM_FLASH 0xC1000u   /* USRTRIM: the factory calibration record ("trimdat"), never written here */
/* 5-point velocity curves (trimdata, flash 0xC1000: +0x08 16 pads x 5 u16, +0xA8 37 keys x 5 u16) */
static uint16_t smk_trim_pad[SMK_NPAD][5], smk_trim_key[FM1_NKEY][5];


/* ------------------------------------------------------------------ state --- */
static volatile struct {
    uint32_t notes;              /* bit n = note key n (0 = F3 .. 26 = G5): keybed keys 5..31 | the pads */
    uint32_t knotes, pnotes;     /* (the two sources of notes: keybed keys 5..31, pads as white keys) */
    uint32_t buttons;            /* debounced: bit i = button i (0..17) */
    uint32_t pressed, released;  /* button edges since the last fm1_input_edges() */
    uint32_t notes_pressed;      /* note-key press edges since the last fm1_input_note_edges() */
    uint64_t keys;               /* all 37 keybed keys: bit k = key k (0 = lowest C) */
    uint8_t key_vel[FM1_NKEY];   /* MIDI velocity 1..127 of each key's last press (a pad: its white key's) */
    uint8_t raw[FM1_NCOL];       /* last frame, rows packed, 1 = closed */
    uint8_t cnt[FM1_NBTN];       /* button debounce */
    uint8_t ks_last[FM1_NKEY], ks_stab[FM1_NKEY], ks_rel[FM1_NKEY];
    uint32_t ks_t1[FM1_NKEY];    /* fm1_ticks() when contact 1 closed */
    uint8_t enc_last[FM1_NENC], enc_hist[FM1_NENC];
    int16_t enc_cnt[FM1_NENC];   /* transitions, + = B leads */
    int16_t enc_steps[FM1_NENC];
    uint16_t pad_raw[SMK_NPAD], pad_base[SMK_NPAD], pad_peak[SMK_NPAD];
    uint8_t pad_st[SMK_NPAD];    /* 0 idle, 1..SMK_PAD_PEAKN collecting the peak, 0x80 on */
    uint8_t pad_low[SMK_NPAD];
    uint8_t pad_press[SMK_NPAD]; /* pressure 0..127 while on */
    uint16_t fad_acc[4];         /* faders: 0..1023 IIR */
    uint8_t fader[4];            /* 0..127 */
    uint16_t wheel[2], pedal, batt;   /* raw 0..1023 (mux C addresses 4, 5, 6, 7) */
    uint16_t fad_raw[4];         /* (debug: the last raw fader readings) */
    uint16_t fad_hist[4][2];     /* the two readings before: a median of 3 drops a lone spike */
    uint32_t adc_ok, adc_miss;   /* (debug: conversions read / not finished within a tick) */
    uint32_t frames;
} fm1_in;

/* key events beyond the FM-1's 27 (keybed keys 0..4 and 32..36): key << 8 | velocity (0 = release) */
#define SMK_EXTQ 32u
static volatile uint16_t smk_extq[SMK_EXTQ];
static volatile uint8_t smk_extq_w, smk_extq_r;

static uint8_t fm1_led[SMK_NLED];     /* 1 = lit (button LED i = button i) */

/* -------------------------------------------------------------- velocity --- */
static uint32_t smk_curve(const uint16_t *t, uint32_t x)   /* stock curve: 5 points -> 1..1023 */
{
    uint32_t i = 0, v;
    while (i < 5u && x > t[i])
        i++;
    if (i == 0)
        return 1;
    if (i == 5u)
        return 1023;
    v = (i - 1u) * 256u + ((x - t[i - 1u]) << 8) / (uint32_t)(t[i] - t[i - 1u]);
    return v ? v : 1u;
}
FM1_INLINE uint8_t smk_vel7(uint32_t v10) { return (uint8_t)(v10 >> 3 ? v10 >> 3 : 1u); }

static void smk_trim_defaults(void)
{
    static const uint16_t P[5] = {50, 550, 720, 850, 950}, K[5] = {5, 640, 800, 920, 980};
    uint32_t i, j;
    for (i = 0; i < SMK_NPAD; i++)
        for (j = 0; j < 5u; j++)
            smk_trim_pad[i][j] = P[j];
    for (i = 0; i < FM1_NKEY; i++)
        for (j = 0; j < 5u; j++)
            smk_trim_key[i][j] = K[j];
}

/* the factory calibration record (0x22C bytes, "trimdat\0" at +0x224), e.g. read from flash 0xC1000;
 * ignored unless the marker and strictly rising curves check out. Returns 1 if taken. */
static int smk_trim_load(const uint8_t *rec)
{
    static const char M[8] = "trimdat";
    uint32_t i, j, ok = 1;
    const uint8_t *p;
    for (i = 0; i < 8u; i++)
        ok &= rec[0x224u + i] == (uint8_t)M[i];
    for (i = 0; ok && i < SMK_NPAD + FM1_NKEY; i++) {
        p = rec + 8u + i * 10u;
        for (j = 1; j < 5u; j++)
            ok &= (p[2u * j] | p[2u * j + 1u] << 8) > (p[2u * j - 2u] | p[2u * j - 1u] << 8);
    }
    if (!ok)
        return 0;
    for (i = 0; i < SMK_NPAD + FM1_NKEY; i++)
        for (j = 0; j < 5u; j++) {
            uint16_t v = (uint16_t)(rec[8u + i * 10u + 2u * j] | rec[9u + i * 10u + 2u * j] << 8);
            if (i < SMK_NPAD)
                smk_trim_pad[i][j] = v;
            else
                smk_trim_key[i - SMK_NPAD][j] = v;
        }
    return 1;
}

static void smk__extq(uint32_t k, uint32_t vel)
{
    uint8_t w = smk_extq_w;
    if ((uint8_t)(w + 1u) % SMK_EXTQ == smk_extq_r)
        return;
    smk_extq[w] = (uint16_t)(k << 8 | vel);
    smk_extq_w = (uint8_t)((w + 1u) % SMK_EXTQ);
}

static void smk__key_event(uint32_t k, uint32_t vel)     /* vel 0 = release */
{
    if (vel)
        fm1_in.key_vel[k] = (uint8_t)vel;
    if (k >= SMK_KEY_F3 && k < SMK_KEY_F3 + 27u) {
        uint32_t bit = 1u << (k - SMK_KEY_F3);
        if (vel) {
            fm1_in.knotes |= bit;
            fm1_in.notes_pressed |= bit;
        } else {
            fm1_in.knotes &= ~bit;
        }
        fm1_in.notes = fm1_in.knotes | fm1_in.pnotes;
    } else {
        smk__extq(k, vel);
    }
    if (vel)
        fm1_in.keys |= (uint64_t)1u << k;
    else
        fm1_in.keys &= ~((uint64_t)1u << k);
}

static void smk__padq(uint32_t pad, uint32_t vel)     /* a pad hit (vel) or release (0): its white key */
{
    int32_t n0 = smk_pad_piano ? SMK_PIANO_NOTE[pad] : (int32_t)SMK_WHITE_NOTE[SMK_WHITE_OF_PAD(pad)];
    uint32_t n, bit;
    if (vel) {
        if (n0 < 0)
            return;                                   /* (a gap of the piano layout) */
        smk_pad_note[pad] = (uint8_t)(n0 + 1);
    } else {
        if (!smk_pad_note[pad])
            return;
        n0 = smk_pad_note[pad] - 1;                    /* the key it pressed, whatever the layout is now */
        smk_pad_note[pad] = 0;
    }
    n = (uint32_t)n0;
    bit = 1u << n;
    if (vel) {
        fm1_in.key_vel[SMK_KEY_F3 + n] = (uint8_t)vel;
        fm1_in.pnotes |= bit;
        fm1_in.notes_pressed |= bit;
    } else {
        fm1_in.pnotes &= ~bit;
    }
    fm1_in.notes = fm1_in.knotes | fm1_in.pnotes;
}

/* ------------------------------------------------------- main-loop API --- */
#ifndef SMK_INPUT_LOCK
static inline uint32_t fm1__lock(void)
{
    __asm__ volatile("cli" ::: "memory");
    return 0;
}
static inline void fm1__unlock(uint32_t v)
{
    (void)v;
    __asm__ volatile("csync\n\tsti" ::: "memory");
}
#else
static inline uint32_t fm1__lock(void) { return SMK_INPUT_LOCK(); }
static inline void fm1__unlock(uint32_t v) { SMK_INPUT_UNLOCK(v); }
#endif

static int32_t fm1_enc_take(uint32_t e)
{
    uint32_t k = fm1__lock();
    int32_t s = fm1_in.enc_steps[e];
    fm1_in.enc_steps[e] = 0;
    fm1__unlock(k);
    return s;
}

static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    fm1__unlock(k);
    return p;
}

static uint32_t fm1_input_note_edges(void)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    fm1__unlock(k);
    return p;
}

/* the next outer-key event (keybed key << 8 | velocity, 0 = release), -1 if none */
static int32_t smk_extkey_take(void)
{
    int32_t v = -1;
    if (smk_extq_r != smk_extq_w) {
        v = smk_extq[smk_extq_r];
        smk_extq_r = (uint8_t)((smk_extq_r + 1u) % SMK_EXTQ);
    }
    return v;
}

/* LED of button id (0..17); note keys have no LED on the SMK (the pads show the step grid) */
static void fm1_led_key(uint32_t id, int on)
{
    if (id < SMK_NLED)
        fm1_led[id] = (uint8_t)(on != 0);
}

/* fm1_adc.h stand-ins for main.c: the battery comes from mux C; the FM-1's MASTER pot is knob K1 here, as a
 * virtual pot (0..1023, 24 a detent; starts at 724: master_q12 2048, the level the firmware starts with) */
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
#define SMK_ENC_MASTER 0u          /* K1, where the FM-1 has its MASTER pot (top left) */
static int32_t smk_master = 724;
static void fm1_adc_init(void) {}
static int32_t fm1_adc_read(uint32_t ch)
{
    if (ch == FM1_ADC_BATT)
        return fm1_in.batt ? (int32_t)fm1_in.batt : -1;
    if (ch == FM1_ADC_MASTER) {
        smk_master += fm1_enc_take(SMK_ENC_MASTER) * 24;
        smk_master = smk_master < 0 ? 0 : smk_master > 1023 ? 1023 : smk_master;
        return smk_master;
    }
    return -1;
}
