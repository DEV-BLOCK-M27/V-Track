/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app. main.c calls it:
 *   app_boot_screen()   at power-on, before the audio and the input start: the boot screen
 *   app_start()         once the boot screen has been shown: the app's first screen
 *   app_frame()         each main-loop frame (~60 a second): input in, LEDs and display out
 *   app_idle()          between frames, as often as the main loop gets round to it
 *   app_audio(out, n)   in the audio ISR: n stereo frames, Q15, interleaved L R
 * The shell: the name on an empty screen, the LEDs dark, silence; every input (USB MIDI in too) is taken and
 * dropped. */
#define VT_NAME "V-Track"

static struct {
    uint8_t drawn;               /* the screen shows the current view */
} app;

static void app_name_screen(uint32_t y)   /* an empty screen with the name at row y */
{
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, y, 240, &AF_M, VT_NAME, T_TEXT, 1);
}

static void app_boot_screen(void)
{
    palette_set(UI_GREY_INDEX);
    app_name_screen(100);
    draw_text_box(0, 128, 240, &AF_S, FELUCCA_VERSION, T_MID, 1);
}

static void app_start(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NLED; i++)
        fm1_led[i] = 0;
    for (i = 0; i < SMK_NPAD; i++)
        padled_set(i, 0, 0, 0);
    app.drawn = 0;
}

static void app_input(void)          /* take every input event (nothing acts on them yet) */
{
    uint32_t e;
    while (mi_r != mi_w) {               /* USB MIDI in (usb.c's ring; the TIMER5 ISR fills it) */
        RING_PUBLISH();
        mi_r++;
    }
    midi_in_overflow = 0;
    fm1_input_edges(0);
    fm1_input_note_edges();
    while (smk_extkey_take() >= 0)
        ;
    for (e = 0; e < FM1_NENC; e++)
        fm1_enc_take(e);
}

static void app_frame(void)
{
    app_input();
    if (!app.drawn) {
        app_name_screen(110);
        app.drawn = 1;
    }
}

static void app_idle(void) {}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < 2u * n; i++)
        out[i] = 0;
}
