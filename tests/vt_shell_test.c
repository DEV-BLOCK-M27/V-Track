/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app shell on the simulated SMK-37 (seam A, tests/vt_sim.h): it boots through a screen showing the
 * name into the pattern view (track 1's rows, tests/vt_pattern_test.c), silent, the button LEDs dark, the pads
 * showing the rows; a key played (it sounds: tests/vt_note_test.c) leaves the screen and the LEDs as they are; the
 * controls V-Track does not use yet and USB MIDI in change nothing (MIDI in is taken, never refused); the flash is
 * not written.
 *   tests/vt_shell_test [OUT_DIR]   (OUT_DIR: the screen as shell.ppm) */
#include "vt_sim.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("%-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static int silent(const int32_t *a, uint32_t frames)
{
    uint32_t i;
    for (i = 0; i < 2u * frames; i++)
        if (a[i])
            return 0;
    return 1;
}

static int buttons_dark(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NLED; i++)
        if (sim_button_led(i))
            return 0;
    return 1;
}

static uint32_t pads_then[SMK_NPAD];
static void pads_keep(void)              /* the pads' colours now, for pads_kept */
{
    uint32_t i;
    for (i = 0; i < SMK_NPAD; i++)
        pads_then[i] = sim_pad_led(i);
}
static int pads_kept(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NPAD; i++)
        if (sim_pad_led(i) != pads_then[i])
            return 0;
    return 1;
}

static int screen_drawn(void)            /* more than one colour on the screen */
{
    const uint16_t *f = sim_frame();
    uint32_t i;
    for (i = 1; i < 240u * 240u; i++)
        if (f[i] != f[0])
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t frames, pkts[16];
    const int32_t *a;
    char path[512];

    sim_power_on();
    sim_run_ms(100);
    check(sim_screen_text("NES \xB7 PULSE 1") && sim_screen_text("00"), "power-on: the pattern view, track 1's rows");
    check(screen_drawn(), "power-on: something is drawn");
    a = sim_audio(&frames);
    check(frames >= 44100u * 500u / 1000u, "power-on: audio runs from the boot screen on (>= 0.5 s)");
    check(silent(a, frames), "power-on: the audio is silent");
    check(buttons_dark(), "power-on: the button LEDs dark");
    check(sim_pad_led(0) && sim_pad_led(1) && sim_pad_led(0) != sim_pad_led(1), "power-on: the pads show the rows");
    pads_keep();

    sim_key(17, 100);                    /* F4, played and held */
    sim_run_ms(250);
    sim_key(17, 0);
    sim_run_ms(250);
    sim_audio(&frames);                  /* (what it played: tests/vt_note_test.c) */
    check(sim_screen_text("NES \xB7 PULSE 1") && sim_screen_text("00"), "a key played: the screen as it was");
    check(buttons_dark() && pads_kept(), "a key played: the LEDs as they were");
    check(sim_midi_out(pkts, 16) == 0, "a key played: no MIDI out");

    {   /* inputs not used yet: buttons, encoders, faders, wheels; USB MIDI notes, more than the device's ring holds */
        uint32_t i, refused = 0;
        sim_button(SMK_BTN_SEQ, 1);
        sim_encoder(4, 3);
        sim_fader(0, 100);
        sim_wheel(0, 900);
        sim_wheel(1, 200);
        sim_run_ms(50);
        sim_button(SMK_BTN_SEQ, 0);
        sim_encoder(4, -3);
        for (i = 0; i < 1000u; i++) {
            refused += !sim_midi_in(0x90, (uint8_t)(48u + i % 24u), 100);
            refused += !sim_midi_in(0x80, (uint8_t)(48u + i % 24u), 0);
            sim_run_ms(2);
        }
        sim_run_ms(100);
        check(refused == 0, "USB MIDI in: 2000 messages over 2 s, every one taken");
        a = sim_audio(&frames);
        check(silent(a, frames), "buttons, knobs, faders, wheels, MIDI: still silent");
        check(sim_screen_text("NES \xB7 PULSE 1") && buttons_dark() && pads_kept() && sim_midi_out(pkts, 16) == 0,
              "buttons, knobs, faders, wheels, MIDI: the screen and LEDs as they were, no MIDI out");
    }
    check(sim_flash_writes() == 0 && sim_flash_refused() == 0, "the flash is not written, nothing refused");

    if (argc > 1) {
        snprintf(path, sizeof path, "%s/shell.ppm", argv[1]);
        sim_save_ppm(path);
    }
    printf(fails ? "V-TRACK SHELL: %d FAILED\n" : "V-TRACK SHELL: ok\n", fails);
    return fails != 0;
}
