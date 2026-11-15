/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app. main.c calls it:
 *   app_boot_screen()   at power-on, before the audio and the input start: the boot screen; the player starts
 *   app_start()         once the boot screen has been shown: the app's first screen
 *   app_frame()         each main-loop frame (~60 a second): input in, LEDs and display out
 *   app_idle()          between frames, as often as the main loop gets round to it
 *   app_audio(out, n)   in the audio ISR: n stereo frames, Q15, interleaved L R
 * So far: the name on an empty screen, the LEDs dark; the keys play track 1 (player.c), one note at a time: the
 * last key pressed takes the channel, its release silences it. Every other input (USB MIDI in too) is taken and
 * dropped. */
#define VT_NAME "V-Track"
#define VT_KEY_NOTE0 48u         /* keybed key 0 = C3 (MIDI 48) */

static struct {
    uint8_t drawn;               /* the screen shows the current view */
    uint64_t keys;               /* the keybed keys held at the last frame */
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
    player_init();                       /* before the audio ISR runs it */
}

static void app_start(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NLED; i++)
        fm1_led[i] = 0;
    for (i = 0; i < SMK_NPAD; i++)
        padled_set(i, 0, 0, 0);
    app.drawn = 0;
    app.keys = fm1_in.keys;              /* keys held already: no note until pressed again */
}

static void app_keys(void)           /* the keybed: releases, then the highest key newly pressed takes the track */
{
    uint64_t now = fm1_in.keys, down = now & ~app.keys, up = app.keys & ~now;
    uint32_t k;
    app.keys = now;
    for (k = 0; k < FM1_NKEY; k++)
        if (up >> k & 1u)
            player_note_off(0, VT_KEY_NOTE0 + k);
    for (k = FM1_NKEY; k-- > 0;)
        if (down >> k & 1u) {
            player_note_on(0, VT_KEY_NOTE0 + k);
            break;
        }
}

static void app_input(void)          /* take every input event: the keys play, the rest is dropped */
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
    app_keys();
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
    player_audio(out, n);
}
