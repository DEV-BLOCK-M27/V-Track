/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app shell on the simulated SMK-37 (seam A, tests/vt_sim.h): it starts into a screen showing the name,
 * silent; a key played, the other controls and USB MIDI in change nothing (MIDI in is taken, never refused); the LEDs stay dark; the flash is not written.
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

static int leds_dark(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NPAD; i++)
        if (sim_pad_led(i))
            return 0;
    for (i = 0; i < SMK_NLED; i++)
        if (sim_button_led(i))
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
    check(sim_screen_text("V-Track"), "power-on: the screen shows V-Track");
    check(screen_drawn(), "power-on: something is drawn");
    a = sim_audio(&frames);
    check(frames >= 44100u * 500u / 1000u, "power-on: audio runs from the boot screen on (>= 0.5 s)");
    check(silent(a, frames), "power-on: the audio is silent");
    check(leds_dark(), "power-on: pads and button LEDs dark");

    sim_key(17, 100);                    /* F4, played and held */
    sim_run_ms(250);
    sim_key(17, 0);
    sim_run_ms(250);
    a = sim_audio(&frames);
    check(frames >= 44100u * 450u / 1000u && silent(a, frames), "a key played: still silent");
    check(sim_screen_text("V-Track"), "a key played: the screen still shows V-Track");
    check(leds_dark(), "a key played: the LEDs stay dark");
    check(sim_midi_out(pkts, 16) == 0, "a key played: no MIDI out");

    {   /* every other input: pads, buttons, encoders, faders, wheels; USB MIDI notes, more than the device's ring holds */
        uint32_t i, refused = 0;
        sim_pad(3, 90);
        sim_button(SMK_BTN_SEQ, 1);
        sim_encoder(4, 3);
        sim_fader(0, 100);
        sim_wheel(0, 900);
        sim_wheel(1, 200);
        sim_run_ms(50);
        sim_pad(3, 0);
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
        check(silent(a, frames), "pads, buttons, knobs, faders, wheels, MIDI: still silent");
        check(sim_screen_text("V-Track") && leds_dark() && sim_midi_out(pkts, 16) == 0,
              "pads, buttons, knobs, faders, wheels, MIDI: the screen, dark LEDs, no MIDI out");
    }
    check(sim_flash_writes() == 0 && sim_flash_refused() == 0, "the flash is not written, nothing refused");

    if (argc > 1) {
        snprintf(path, sizeof path, "%s/shell.ppm", argv[1]);
        sim_save_ppm(path);
    }
    printf(fails ? "V-TRACK SHELL: %d FAILED\n" : "V-TRACK SHELL: ok\n", fails);
    return fails != 0;
}
