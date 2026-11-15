/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app. main.c calls it:
 *   app_boot_screen()   at power-on, before the audio and the input start: the boot screen; a new project, the player
 *                       starts
 *   app_start()         once the boot screen has been shown (and after an update session): the app's screen
 *   app_frame()         each main-loop frame (~60 a second): input in, LEDs and display out
 *   app_idle()          between frames, as often as the main loop gets round to it
 *   app_audio(out, n)   in the audio ISR: n stereo frames, Q15, interleaved L R
 * So far, one view: the selected track's chip and channel and the tempo on top, below it the 16 rows of the cursor's
 * page of its pattern (the cursor row highlighted, the playing row tinted). K3 selects the track (1 to 4, it stops
 * at the ends), K4 sets the tempo (1 BPM a detent). The keys play the selected track (player.c), one note at a time
 * a track: the last key pressed takes the channel, its release silences it; a held note stays on its track.
 * SEQ PLAY starts and stops the pattern (looped, all four tracks), SEQ REC switches recording (both LEDs show it).
 * With REC on and the sequencer stopped a key also writes its note into the cursor row, and KNOB BANK (shift) with
 * a white key a cut, with a black key a release (shift keeps the keys from playing); the cursor then moves down a
 * row (after the last row, to the first). The pads show the 16 rows of the page: empty, a note (cut and release
 * too), the cursor, the playing row; a tap moves the cursor to its row. Every other input (USB MIDI in too) is
 * taken and dropped. */
#define VT_NAME "V-Track"
#define VT_KEY_NOTE0 48u         /* keybed key 0 = C3 (MIDI 48) */
#define VT_ENC_TRACK 2u          /* K3: track select */
#define VT_ENC_TEMPO 3u          /* K4: tempo */
#define VT_PAGE 16u              /* rows a page: the pads */
#define VT_LIST_Y 26u            /* the row list: its top, an entry's height */
#define VT_LIST_H 13u
#define VT_SHOWN_CURSOR 0x100u   /* app.shown: what a list entry shows (the note column, these flags) */
#define VT_SHOWN_PLAYING 0x200u
#define VT_SHOWN_NONE 0xFFFFFFFFu   /* not drawn */

static struct {
    uint8_t drawn;               /* the header shows the current tempo */
    uint8_t track;               /* the selected track 0..3 */
    uint8_t cursor;              /* the cursor row */
    uint8_t rec;                 /* SEQ REC on */
    uint64_t keys;               /* the keybed keys held at the last frame */
    uint8_t key_track[FM1_NKEY]; /* the track each held key plays on */
    uint16_t pads_held;          /* the pads held at the last frame, a bit each */
    uint32_t shown[VT_PAGE];     /* what each list entry shows */
} app;

static const uint8_t VT_PAD_EMPTY[3] = {2, 2, 6}, VT_PAD_NOTE[3] = {0, 20, 60}, VT_PAD_CURSOR[3] = {60, 60, 60},
                     VT_PAD_PLAYING[3] = {0, 90, 10};   /* r g b */

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
    project_init();
    player_init();                       /* before the audio ISR runs it */
}

static void app_redraw(void)         /* the header and every list entry again */
{
    uint32_t i;
    app.drawn = 0;
    for (i = 0; i < VT_PAGE; i++)
        app.shown[i] = VT_SHOWN_NONE;
}

static void app_start(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NLED; i++)
        fm1_led[i] = 0;
    app_redraw();
    app.track = 0;
    app.cursor = 0;
    app.rec = 0;
    app.pads_held = 0;
    app.keys = fm1_in.keys;              /* keys held already: no note until pressed again */
    lcd_fill(0, 0, 240, 240, T_BG);
}

static uint32_t app_page(void) { return app.cursor / VT_PAGE * VT_PAGE; }   /* the cursor page's first row */

static void app_enter(uint32_t v)    /* REC, stopped: v into the cursor row, the cursor down a row */
{
    project_set_note(app.track, app.cursor, v);
    app.cursor = (uint8_t)(app.cursor + 1u < project_rows(app.track) ? app.cursor + 1u : 0u);
}

static int app_black(uint32_t k)     /* keybed key k is a black key */
{
    return (0x54Au >> ((VT_KEY_NOTE0 + k) % 12u)) & 1u;
}

static void app_keys(void)           /* the keybed: releases, then the highest key newly pressed */
{
    uint64_t now = fm1_in.keys, down = now & ~app.keys, up = app.keys & ~now;
    uint32_t k, shift = fm1_in.buttons >> SMK_BTN_KNOB_BANK & 1u, enter = app.rec && !player_playing();
    app.keys = now;
    for (k = 0; k < FM1_NKEY; k++)
        if (up >> k & 1u)
            player_note_off(app.key_track[k], VT_KEY_NOTE0 + k);
    for (k = FM1_NKEY; k-- > 0;)
        if (down >> k & 1u) {
            if (shift) {                 /* shift: cut or release, no sound */
                if (enter)
                    app_enter(app_black(k) ? PROJECT_RELEASE : PROJECT_CUT);
                break;
            }
            app.key_track[k] = app.track;
            player_note_on(app.track, VT_KEY_NOTE0 + k);
            if (enter)
                app_enter(PROJECT_NOTE(VT_KEY_NOTE0 + k));
            break;
        }
}

static void app_pads(void)           /* a pad newly hit: the cursor to its row of the page */
{
    uint32_t p, held = 0;
    for (p = 0; p < SMK_NPAD; p++)
        held |= (uint32_t)((fm1_in.pad_st[p] & 0x80u) != 0) << p;
    for (p = 0; p < SMK_NPAD; p++)
        if ((held & ~app.pads_held) >> p & 1u)
            app.cursor = (uint8_t)(app_page() + p);
    app.pads_held = (uint16_t)held;
}

static void app_track(int32_t steps)  /* K3 turned: the next track up or down, stopping at 1 and 4 */
{
    int32_t t = (int32_t)app.track + steps;
    t = t < 0 ? 0 : t >= (int32_t)PLAYER_TRACKS ? (int32_t)PLAYER_TRACKS - 1 : t;
    if (t != app.track) {
        app.track = (uint8_t)t;
        app_redraw();
    }
}

static void app_input(void)          /* take every input event */
{
    uint32_t e, pressed;
    while (mi_r != mi_w) {               /* USB MIDI in (usb.c's ring; the TIMER5 ISR fills it) */
        RING_PUBLISH();
        mi_r++;
    }
    midi_in_overflow = 0;
    pressed = fm1_input_edges(0);
    if (pressed >> SMK_BTN_SEQ_PLAY & 1u) {
        if (player_playing())
            player_stop();
        else
            player_play();
    }
    if (pressed >> SMK_BTN_SEQ_REC & 1u)
        app.rec ^= 1u;
    fm1_input_note_edges();
    while (smk_extkey_take() >= 0)
        ;
    for (e = 0; e < FM1_NENC; e++) {
        int32_t s = fm1_enc_take(e);
        if (!s)
            continue;
        if (e == VT_ENC_TRACK)
            app_track(s);
        if (e == VT_ENC_TEMPO) {
            project_set_bpm((int32_t)project_bpm() + s);
            app.drawn = 0;
        }
    }
    app_keys();
    app_pads();
}

static void app_note_name(uint32_t v, char *s)   /* a note column as the tracker writes it: C-4, C#4, ---, ===, ... */
{
    static const char NAME[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
    uint32_t m = v - 1u;
    if (PROJECT_IS_NOTE(v)) {
        s[0] = NAME[2u * (m % 12u)];
        s[1] = NAME[2u * (m % 12u) + 1u];
        s[2] = m < 12u ? '-' : (char)('0' + (m / 12u - 1u) % 10u);
    } else {
        s[0] = s[1] = s[2] = v == PROJECT_CUT ? '-' : v == PROJECT_RELEASE ? '=' : '.';
    }
    s[3] = 0;
}

static void app_draw_entry(uint32_t i, uint32_t row, uint32_t show)   /* list entry i: row, as show says */
{
    static const char HEX[] = "0123456789ABCDEF";
    char num[3] = {HEX[row >> 4 & 15u], HEX[row & 15u], 0}, note[4];
    uint16_t bg = show & VT_SHOWN_CURSOR ? T_SEL : show & VT_SHOWN_PLAYING ? T_TINT : T_BG;
    uint16_t fg = show & VT_SHOWN_CURSOR ? T_INK : T_TEXT;
    app_note_name(show & 0xFFu, note);
    cv_begin(240, VT_LIST_H, bg);
    cv_text(16, 0, &AF_S, num, show & VT_SHOWN_CURSOR ? T_INK : T_MID);
    cv_text(48, 0, &AF_S, note, fg);
    cv_blit(0, VT_LIST_Y + i * VT_LIST_H);
}

static void app_draw(void)           /* the header if it changed, then each list entry that changed */
{
    uint32_t i, page = app_page(), playing = player_playing(), prow = player_row();
    if (!app.drawn) {
        char bpm[8];
        uint32_t b = project_bpm(), n = 0;
        if (b >= 100u)
            bpm[n++] = (char)('0' + b / 100u);
        bpm[n++] = (char)('0' + b / 10u % 10u);
        bpm[n++] = (char)('0' + b % 10u);
        memcpy(bpm + n, " BPM", 5);
        cv_begin(240, AF_M.h, T_BG);
        cv_text(8, 0, &AF_M, player_track_name(app.track), T_TEXT);
        cv_text_r(232, 0, &AF_M, bpm, T_MID, T_BG);
        cv_blit(0, 4);
        app.drawn = 1;
    }
    for (i = 0; i < VT_PAGE; i++) {
        uint32_t row = page + i, show = project_note(app.track, row);
        if (row == app.cursor)
            show |= VT_SHOWN_CURSOR;
        if (playing && row == prow)
            show |= VT_SHOWN_PLAYING;
        if (show != app.shown[i]) {
            app_draw_entry(i, row, show);
            app.shown[i] = show;
        }
    }
}

static void app_leds(void)           /* the pads: the rows of the page; SEQ PLAY and SEQ REC */
{
    uint32_t p, page = app_page(), playing = player_playing(), prow = player_row();
    for (p = 0; p < SMK_NPAD; p++) {
        uint32_t row = page + p;
        const uint8_t *c = playing && row == prow   ? VT_PAD_PLAYING
                           : row == app.cursor      ? VT_PAD_CURSOR
                           : project_note(app.track, row) ? VT_PAD_NOTE
                                                    : VT_PAD_EMPTY;
        padled_set(p, c[0], c[1], c[2]);
    }
    fm1_led_key(SMK_BTN_SEQ_PLAY, playing);
    fm1_led_key(SMK_BTN_SEQ_REC, app.rec);
}

static void app_frame(void)
{
    app_input();
    app_draw();
    app_leds();
}

static void app_idle(void) {}

static void app_audio(int32_t *out, uint32_t n)
{
    player_audio(out, n);
}
