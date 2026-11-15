/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Seam A: the V-Track device in the host simulator. The firmware's own sources (firmware/src/vtrack.c, everything
 * from the display code up) run against a simulated SMK-37:
 *   - the clock: fm1_ms and the 24 MHz TIMER4 advance with sim_run_ms; the audio ISR (audio.c, the real one)
 *     renders each I2S half when the DMA would ask for it, the main loop (main.c vt_frame) runs every 15 ms with
 *     vt_idle in between, as the device's fm1_main does;
 *   - the input: keys, pads, buttons, encoders, faders, wheels land in the SMK input state (hal/smk_input_state.h,
 *     the real one) as the hardware scan leaves them; USB MIDI in goes into usb.c's ring as a received packet does;
 *   - the output: the 240 x 240 frame (and the text visible on it, read back from the drawing code), the 16 pad
 *     colours as last sent, the button LEDs, USB MIDI packets sent, the audio (24-bit stereo, as the codec gets it);
 *   - the flash: a 1 MiB NOR behind the flash driver's entry points, with the driver's own allow-list (FL_GUARD);
 *     refused erases / writes are counted, the store is read back as bytes.
 * A test includes this file, then drives the device only through the sim_* functions below and observes only
 * what they return: what a player would see, hear and keep.
 *   sim_power_on()            power on (the flash as it is: erased at first) up to the app's first screen
 *   sim_run_ms(ms)            time passes
 *   sim_key(k, vel)           keybed key 0..36 (0 = C3) down with velocity 1..127, or up (vel 0)
 *   sim_pad(p, vel)           pad 0..15 (0..7 the top row, left to right) hit with velocity 1..127, or released (0)
 *   sim_button(b, down)       button SMK_BTN_* (hal/smk_input_state.h) down / up
 *   sim_encoder(e, steps)     encoder K1..K8 = 0..7 turned by steps detents (+ clockwise)
 *   sim_fader(f, v)           fader 0..3 at 0..127
 *   sim_wheel(w, raw)         pitch wheel (0) / mod wheel (1) at its raw reading 0..1023
 *   sim_midi_in(st, d1, d2)   a USB MIDI channel message from the host; 0 if the device's ring was full
 *   sim_midi_out(pkts, max)   USB-MIDI event packets the device sent since the last call
 *   sim_audio(&frames)        stereo frames (L R interleaved, 24-bit) rendered since the last call
 *   sim_frame()               the screen: 240 x 240 RGB565 (row by row)
 *   sim_screen_text(s)        1 if the text s is on the screen now (drawn and not drawn over since)
 *   sim_pad_led(p)            pad p's colour as last sent to the pads, 0xRRGGBB
 *   sim_button_led(b)         1 if button b's LED is lit
 *   sim_flash(), SIM_FLASH_SIZE   the flash chip's bytes; sim_flash_writes() / sim_flash_refused(): erases and
 *                             page writes done / refused by the driver
 *   sim_save_ppm(path)        the screen as a PPM file (for looking at it) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VT_SIM 1
#define BOARD_SMK37 1
#define VT_SIM_USB "vt_sim_usb.h"

/* the firmware's libc (libc.c), its memset / memcpy / memcmp renamed beside the host's */
#define memset vt_memset
#define memcpy vt_memcpy
#define memcmp vt_memcmp
#include "../firmware/src/libc.c"

/* ------------------------------------------------------------- the clock --- */
#define FM1_TICKS_PER_US 24u                 /* TIMER4: 24 MHz */
static uint32_t sim_ticks;
static uint32_t fm1_ticks(void) { return sim_ticks; }
static void sim__ms(void);
static void fm1_delay_ms(uint32_t ms)        /* the device waits: time passes (the main loop does not run) */
{
    while (ms--)
        sim__ms();
}

/* ------------------------------------------------------------- the input --- */
#define SMK_INPUT_LOCK() 0u
#define SMK_INPUT_UNLOCK(v) ((void)(v))
#include "../firmware/hal/smk_input_state.h"
static void fm1_input_init(void)
{
    smk_trim_defaults();
}

/* ------------------------------------------------- audio (ALNK0), timer --- */
#define FM1_AUDIO_HALF 0x80u
static uint32_t sim_half, sim_audio_on;
static uint8_t fm1_audio_pending(void) { return FM1_AUDIO_HALF; }
static void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static uint32_t fm1_audio_free_half(void) { return sim_half; }
static void fm1_audio_ack_half(void) {}
static void fm1_audio_init(int32_t *buf, uint32_t words, void (*isr)(void), uint32_t prio)
{
    (void)buf; (void)words; (void)isr; (void)prio;
    sim_audio_on = 1;
}
static void fm1_audio_stop(void) { sim_audio_on = 0; }
void isr_alnk0(void) {}
void isr_timer5(void) {}
static void fm1_timer5_start(void (*isr)(void), uint32_t prio) { (void)isr; (void)prio; }

/* --------------------------------------------------- system (fm1_sys.h) --- */
static struct { uint32_t magic, failed, pending; } bootguard;
static uint32_t sim_uboot, sim_update, sim_reboots;
static void fm1_enter_uboot(void) { sim_uboot++; }
static void fm1_enter_update(const uint8_t *parm) { (void)parm; sim_update++; }
static void fm1_reboot(void) { sim_reboots++; }
static void fm1_wdt_feed(void) {}
static void fm1_guard_lock_top(void) {}
static void fm1_irq_enable_all(void) {}

/* ----------------------------------------------------------- the screen --- */
/* the text on the screen, read back from the drawing code (gfx.c's hooks): what is drawn on the canvas waits for its
 * blit; a blit or a fill takes away every text it draws over */
#define SIM_TXT_MAX 256
typedef struct { int32_t x0, y0, x1, y1; char s[48]; } sim_txt_t;
static sim_txt_t sim_txt[SIM_TXT_MAX], sim_cv_txt[SIM_TXT_MAX];
static uint32_t sim_ntxt, sim_cv_ntxt;
static int32_t sim_blit_x = -1, sim_blit_y, sim_blit_r0;
static void sim__cv_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s)
{
    sim_txt_t *t;
    if (sim_cv_ntxt == SIM_TXT_MAX)
        return;
    t = &sim_cv_txt[sim_cv_ntxt++];
    t->x0 = x0; t->y0 = y0; t->x1 = x1; t->y1 = y1;
    snprintf(t->s, sizeof t->s, "%s", s);
}
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags) sim__cv_text(x0, y0, x1, y1, s)
#define GFX_HOOK_BLIT(x, y, r0) (sim_blit_x = (int32_t)(x), sim_blit_y = (int32_t)(y), sim_blit_r0 = (int32_t)(r0))
#define GFX_HOOK_BEGIN() (sim_cv_ntxt = 0)
#define GFX_HOOK_PIXELS(n) ((void)0)

static uint16_t sim_fb[240 * 240];           /* RGB565 */
static void sim__cover(int32_t x, int32_t y, int32_t w, int32_t h)   /* drawn over: its texts are gone */
{
    uint32_t i, k = 0;
    for (i = 0; i < sim_ntxt; i++) {
        sim_txt_t *t = &sim_txt[i];
        if (t->x1 <= x || t->x0 >= x + w || t->y1 <= y || t->y0 >= y + h)
            sim_txt[k++] = *t;
    }
    sim_ntxt = k;
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            sim_fb[(y + j) * 240u + x + i] = c;
    sim__cover((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)   /* p: the panel's byte order */
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++) {
            uint16_t v = p[j * w + i];
            sim_fb[(y + j) * 240u + x + i] = (uint16_t)(v >> 8 | v << 8);
        }
    sim__cover((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
    if (sim_blit_x >= 0) {                   /* a canvas blit (cv_blit): its texts land on the screen */
        for (i = 0; i < sim_cv_ntxt && sim_ntxt < SIM_TXT_MAX; i++) {
            sim_txt_t t = sim_cv_txt[i];
            if (t.y0 < sim_blit_r0)
                continue;
            t.x0 += sim_blit_x; t.x1 += sim_blit_x;
            t.y0 += sim_blit_y; t.y1 += sim_blit_y;
            sim_txt[sim_ntxt++] = t;
        }
        sim_blit_x = -1;
    }
}
static void lcd_sync(void) {}
static void lcd_init(void) {}

/* the 16 RGB pads (lcd.c on the device): the colours set, then sent */
static uint32_t sim_pad_set[16], sim_pad_sent[16];
static void padled_set(uint32_t pad, uint8_t r, uint8_t g, uint8_t b)
{
    if (pad < 16u)
        sim_pad_set[pad] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}
static void padled_flush(void) { memcpy(sim_pad_sent, sim_pad_set, sizeof sim_pad_sent); }

/* ------------------------------------------------------------ the flash --- */
#include "../firmware/hal/fm1_flash_map.h"
#define SIM_FLASH_SIZE 0x100000u
static uint8_t sim_nor[SIM_FLASH_SIZE];
static uint32_t sim_refused, sim_writes, sim_nor_init;
#define FL_FAR(fn) (fn)
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0xEB6014u; }   /* the SMK-37 Pro's part */
static void fl_plain_window_init(void) {}
static const uint8_t *fm1_xip_ptr(uint32_t off) { return sim_nor + (off % SIM_FLASH_SIZE); }
static int fl_read_ram(uint32_t off, uint8_t *dst, uint32_t n)
{
    if (off >= SIM_FLASH_SIZE || n > SIM_FLASH_SIZE - off)
        return -1;
    memcpy(dst, sim_nor + off, n);
    return 0;
}
static int fl_erase4k_ram(uint32_t off, uint32_t *took_us)  /* the driver's checks (hal/fm1_flash.h) */
{
    if (took_us)
        *took_us = 0;
    if (!FL_GUARD(off, 0x1000u) || (off & 0xFFFu)) {
        sim_refused++;
        return -1;
    }
    memset(sim_nor + off, 0xFF, 0x1000u);
    sim_writes++;
    return 0;
}
static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    if (!FL_GUARD(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u) {
        sim_refused++;
        return -1;
    }
    for (i = 0; i < n; i++)
        sim_nor[off + i] &= src[i];          /* NOR: a write only clears bits */
    sim_writes++;
    return 0;
}
static int fl_erase4k(uint32_t off, uint32_t *took_us) { return fl_erase4k_ram(off, took_us); }
static int fl_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    while (n) {
        uint32_t chunk = 256u - (off & 0xFFu);
        int rc;
        if (chunk > n)
            chunk = n;
        if ((rc = fl_prog_ram(off, src, chunk)) != 0)
            return rc;
        off += chunk; src += chunk; n -= chunk;
    }
    return 0;
}

/* ---------------------------------------------------------- the firmware --- */
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"   /* the HAL register macros (never touched here) */
#define __attribute__(x)
#include "../firmware/src/vtrack.c"
#undef __attribute__
#undef memset
#undef memcpy
#undef memcmp

/* ------------------------------------------------------------ the device --- */
static uint32_t sim_booted, sim_last_frame, sim_samples_due, sim_tick_rem;
static int32_t *sim_abuf;                    /* the audio since the last sim_audio */
static uint32_t sim_aframes, sim_acap;

static void sim__ms(void)                    /* one millisecond of the device */
{
    fm1_ms++;
    sim_ticks += 1000u * FM1_TICKS_PER_US;
    sim_tick_rem += 10u;                     /* the input scan: a frame every 1.6 ms */
    while (sim_tick_rem >= 16u) {
        sim_tick_rem -= 16u;
        fm1_in.frames++;
    }
    sim_samples_due += FS;
    while (sim_samples_due >= 1000u * HALF_FRAMES) {   /* an I2S half: the audio ISR renders it */
        const int32_t *o = &abuf[sim_half * HALF_WORDS];
        sim_samples_due -= 1000u * HALF_FRAMES;
        if (!sim_audio_on)
            continue;
        fm1_alnk0_irq();
        if (sim_aframes + HALF_FRAMES > sim_acap) {
            sim_acap = sim_acap ? 2u * sim_acap : 65536u;
            sim_abuf = realloc(sim_abuf, sim_acap * 2u * sizeof *sim_abuf);
        }
        memcpy(sim_abuf + 2u * sim_aframes, o, HALF_FRAMES * 2u * sizeof *o);
        sim_aframes += HALF_FRAMES;
        sim_half ^= 1u;
    }
    if (!sim_booted)
        return;
    if (fm1_ms - sim_last_frame >= 15u) {    /* main.c fm1_main: a frame, then vt_idle until 15 ms are over */
        sim_last_frame = fm1_ms;
        vt_frame();
    } else {
        vt_idle();
    }
}

static void sim_power_on(void)
{
    if (!sim_nor_init) {
        memset(sim_nor, 0xFF, sizeof sim_nor);
        sim_nor_init = 1;
    }
    vt_boot();
    sim_booted = 1;
    sim_last_frame = fm1_ms - 15u;
}
static void sim_run_ms(uint32_t ms)
{
    while (ms--)
        sim__ms();
}

static void sim_key(uint32_t k, uint32_t vel)
{
    if (k < FM1_NKEY)
        smk__key_event(k, vel > 127u ? 127u : vel);
}
static void sim_pad(uint32_t p, uint32_t vel)
{
    if (p >= SMK_NPAD)
        return;
    vel = vel > 127u ? 127u : vel;
    fm1_in.pad_st[p] = vel ? 0x80u : 0u;
    fm1_in.pad_press[p] = (uint8_t)vel;
    smk__padq(p, vel);
}
static void sim_button(uint32_t b, int down)
{
    uint32_t bit = 1u << b;
    if (b >= FM1_NBTN || !!(fm1_in.buttons & bit) == !!down)
        return;
    fm1_in.buttons ^= bit;
    if (down)
        fm1_in.pressed |= bit;
    else
        fm1_in.released |= bit;
}
static void sim_encoder(uint32_t e, int32_t steps)
{
    if (e < FM1_NENC)
        fm1_in.enc_steps[e] = (int16_t)(fm1_in.enc_steps[e] + steps);
}
static void sim_fader(uint32_t f, uint32_t v) { if (f < 4u) fm1_in.fader[f] = (uint8_t)(v > 127u ? 127u : v); }
static void sim_wheel(uint32_t w, uint32_t raw) { if (w < 2u) fm1_in.wheel[w] = (uint16_t)(raw > 1023u ? 1023u : raw); }

static int sim_midi_in(uint8_t st, uint8_t d1, uint8_t d2)   /* as usb.c ep1_rx takes a received packet */
{
    if (st < 0x80u || st >= 0xF0u)
        return 1;
    return midi_enqueue((uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)(d1 & 127u) << 16 |
                        (uint32_t)(d2 & 127u) << 24, 1);
}
static uint32_t sim_midi_out(uint32_t *pkts, uint32_t max)  /* as usb.c ep1_tx sends them */
{
    uint32_t n = 0;
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r % MQ];
        mo_r++;
        if (n < max)
            pkts[n] = p;
        n++;
    }
    return n < max ? n : max;
}

static const int32_t *sim_audio(uint32_t *frames)
{
    static int32_t *out;
    free(out);
    out = sim_abuf;
    *frames = sim_aframes;
    sim_abuf = 0;
    sim_aframes = sim_acap = 0;
    return out;
}

static const uint16_t *sim_frame(void) { return sim_fb; }
static int sim_screen_text(const char *s)
{
    uint32_t i;
    for (i = 0; i < sim_ntxt; i++)
        if (!strcmp(sim_txt[i].s, s) && sim_txt[i].x0 >= 0 && sim_txt[i].x1 <= 240 && sim_txt[i].y0 >= 0 &&
            sim_txt[i].y1 <= 240)
            return 1;
    return 0;
}
static uint32_t sim_pad_led(uint32_t p) { return p < 16u ? sim_pad_sent[p] : 0u; }
static int sim_button_led(uint32_t b) { return b < SMK_NLED && fm1_led[b]; }
static const uint8_t *sim_flash(void) { return sim_nor; }
static uint32_t sim_flash_refused(void) { return sim_refused; }
static uint32_t sim_flash_writes(void) { return sim_writes; }

static int sim_save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f)
        return -1;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = sim_fb[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u),
                          (uint8_t)((c & 31u) * 255u / 31u)};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}
