/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app. main.c calls it:
 *   app_boot_screen()   at power-on, before the audio and the input start: the boot screen; a new project, the player
 *                       starts
 *   app_start()         once the boot screen has been shown (and after an update session): the app's screen
 *   app_frame()         each main-loop frame (~60 a second): input in, LEDs and display out
 *   app_idle()          between frames, as often as the main loop gets round to it
 *   app_audio(out, n)   in the audio ISR: n stereo frames, Q15, interleaved L R
 * So far, one view, the pattern view (docs/design/pattern-view.png): a header (the track, its chip and channel, its
 * pattern Pxx; the tempo, REC, the pattern's length) over 13 rows of the selected track's pattern, all four columns
 * (note, instrument, volume, effect), and the other three tracks' note columns right of a separator. The cursor row
 * has a blue bar, the playing row a red one (both: purple). The screen follows the cursor, scrolling as little as
 * keeps it shown; a page turned shows the page from its first row.
 * K2 moves the cursor (it stops at the first and last row), shift (KNOB BANK) + K2 sets the pattern's length (16 to
 * 64 rows). K3 selects the track (1 to 4, it stops at the ends), K4 sets the tempo (1 BPM a detent). K5 to K8 edit
 * the cursor row's note (a semitone a detent, C-0 to B-7), instrument (00 to 0F), volume (0 to F) and effect
 * parameter (00 to FF): an empty field takes the value written last first (C-4, 00, F at the start); shift + K5 to
 * K7 empties the field, shift + K8 picks the effect's letter (none, 0 1 2 3 4 A B D F G S V; none empties it).
 * < and > turn the page (16 rows; the cursor moves with it, the last page may be shorter), shift + < and > select
 * the track's pattern (P00 to P1F). The keys play the selected track (player.c), one note at a time a track: the
 * last key pressed takes the channel, its release silences it; a held note stays on its track.
 * SEQ PLAY starts and stops the patterns (looped, all four tracks), SEQ REC switches recording (both LEDs show it).
 * With REC on and the sequencer stopped a key also writes its note into the cursor row, and shift with a white key
 * a cut, with a black key a release (shift keeps the keys from playing); the cursor then moves down a row (after
 * the last row, to the first). The pads show the 16 rows of the cursor's page: empty, a note (cut and release too),
 * the cursor, the playing row, dark past the pattern's end; a tap moves the cursor to its row. Every other input
 * (USB MIDI in too) is taken and dropped. */
#define VT_NAME "V-Track"
#define VT_KEY_NOTE0 48u         /* keybed key 0 = C3 (MIDI 48) */
#define VT_ENC_TRACK 2u          /* K3: track select */
#define VT_ENC_TEMPO 3u          /* K4: tempo */
#define VT_ENC_CURSOR 1u         /* K2: cursor row */
#define VT_ENC_NOTE 4u           /* K5 to K8: the cursor row's note, instrument, volume, effect */
#define VT_NOTE_LO 12u           /* K5's notes: C-0 .. B-7 (MIDI), as FamiTracker's */
#define VT_NOTE_HI 107u
#define VT_PAGE 16u              /* rows a page: the pads */
#define VT_SHOWN 13u             /* the rows the screen shows */
#define VT_HEAD_H 30u            /* the header: two lines */
#define VT_ROW_H 16u             /* a row's line */
#define VT_PITCH 8               /* the columns' character cell (AF_S, monospaced) */
#define VT_X_NUM 4               /* the columns: row number, the selected track's note, instrument, volume, */
#define VT_X_NOTE 26             /* effect; the separator; the other tracks' notes */
#define VT_X_INS 56
#define VT_X_VOL 78
#define VT_X_EFFECT 92
#define VT_X_SEP 121
#define VT_X_OTHER 127
#define VT_OTHER_W 32
#define VT_PALETTE 8u            /* NIGHT (ui_palettes.h): black, not drawn in greys as GREY is (the view's colours) */
#define VT_C_BG RGB(0, 0, 0)
#define VT_C_HEAD RGB(20, 20, 40)    /* the colours (the mockup, docs/design/pattern-view.png) */
#define VT_C_HEAD_DIM RGB(110, 110, 130)
#define VT_C_TEXT RGB(235, 235, 235)
#define VT_C_NUM RGB(90, 90, 90)
#define VT_C_NUM_BEAT RGB(170, 170, 60)
#define VT_C_INS RGB(90, 210, 90)
#define VT_C_VOL RGB(90, 190, 230)
#define VT_C_EFFECT RGB(230, 200, 70)
#define VT_C_EMPTY RGB(55, 55, 55)
#define VT_C_OTHER RGB(160, 160, 160)
#define VT_C_SEP RGB(50, 50, 70)
#define VT_C_CURSOR RGB(30, 40, 120)
#define VT_C_PLAYING RGB(45, 20, 20)
#define VT_C_CURSOR_PLAYING RGB(90, 35, 110)   /* the cursor row playing: the two bars mixed, brighter */
#define VT_LINE_CURSOR 1u        /* vt_line_t.mark */
#define VT_LINE_PLAYING 2u
#define VT_LINE_PAST 4u          /* past the pattern's end: blank */

typedef struct {                 /* what a screen line shows: drawn again when it changes */
    uint8_t row, mark, note[PROJECT_TRACKS], ins, vol;
    uint16_t effect;
} vt_line_t;

static struct {
    uint32_t head;               /* what the header shows (app_head_key); 0 = not drawn */
    uint8_t track;               /* the selected track 0..3 */
    uint8_t cursor;              /* the cursor row */
    uint8_t top;                 /* the first row on the screen */
    uint8_t rec;                 /* SEQ REC on */
    uint8_t last[PROJECT_F_EFFECT];   /* by PROJECT_F_*: the note, instrument and volume written last (an empty
                                       * field takes it first) */
    uint64_t keys;               /* the keybed keys held at the last frame */
    uint8_t key_track[FM1_NKEY]; /* the track each held key plays on */
    uint16_t pads_held;          /* the pads held at the last frame, a bit each */
    vt_line_t shown[VT_SHOWN];   /* what each screen line shows */
} app;

static const uint8_t VT_PAD_EMPTY[3] = {2, 2, 6}, VT_PAD_NOTE[3] = {0, 20, 60}, VT_PAD_CURSOR[3] = {60, 60, 60},
                     VT_PAD_PLAYING[3] = {0, 90, 10}, VT_PAD_PAST[3] = {0, 0, 0};   /* r g b */

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

static void app_redraw(void)         /* the header and every line again */
{
    app.head = 0;
    memset(app.shown, 0xFF, sizeof app.shown);   /* (no line shows row 0xFF) */
}

static void app_start(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NLED; i++)
        fm1_led[i] = 0;
    palette_set(VT_PALETTE);
    app_redraw();
    app.track = 0;
    app.cursor = app.top = 0;
    app.rec = 0;
    app.last[PROJECT_F_NOTE] = PROJECT_NOTE(60u);   /* C-4 */
    app.last[PROJECT_F_INS] = PROJECT_INS(0u);
    app.last[PROJECT_F_VOL] = PROJECT_VOL(PROJECT_VOLUMES - 1u);
    app.pads_held = 0;
    app.keys = fm1_in.keys;              /* keys held already: no note until pressed again */
    lcd_fill(0, 0, 240, 240, VT_C_BG);
}

static uint32_t app_page(void) { return app.cursor / VT_PAGE * VT_PAGE; }   /* the cursor page's first row */

static void app_follow(void)         /* the cursor moved: the screen scrolls as little as keeps it shown */
{
    if (app.cursor < app.top)
        app.top = app.cursor;
    if (app.cursor >= app.top + VT_SHOWN)
        app.top = (uint8_t)(app.cursor + 1u - VT_SHOWN);
}

static int32_t app_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void app_cursor(int32_t r)    /* the cursor to row r, within the pattern; the screen follows */
{
    app.cursor = (uint8_t)app_clamp(r, 0, (int32_t)project_rows(app.track) - 1);
    app_follow();
}

/* < (-1) / > (+1): the cursor a page up / down, the screen to the page; with shift the track's pattern before / after
 * (the cursor kept, within its length) */
static void app_page_or_pattern(int32_t dir, uint32_t shift)
{
    int32_t page = (int32_t)app_page() + dir * (int32_t)VT_PAGE;
    if (shift) {
        project_set_pattern(app.track, (int32_t)project_pattern(app.track) + dir);
        app_cursor(app.cursor);
        return;
    }
    if (page < 0 || page >= (int32_t)project_rows(app.track))
        return;                          /* the first / last page: it stays */
    app.top = (uint8_t)page;
    app_cursor((int32_t)app.cursor + dir * (int32_t)VT_PAGE);   /* (rows 0D to 0F of a page: the screen follows) */
}

static void app_enter(uint32_t v)    /* REC, stopped: v into the cursor row, the cursor down a row */
{
    project_set_field(app.track, app.cursor, PROJECT_F_NOTE, v);
    if (PROJECT_IS_NOTE(v))
        app.last[PROJECT_F_NOTE] = (uint8_t)v;
    app_cursor(app.cursor + 1u < project_rows(app.track) ? (int32_t)app.cursor + 1 : 0);
}

/* K5 to K8 turned: field f of the cursor row. An empty field (a cut or release too) takes the value written last,
 * then each detent steps it (the note a semitone); they stop at the ends. Shift empties it. The effect: K8 its
 * parameter (00 .. FF; not without a letter), shift + K8 its letter (none, then PROJECT_EFFECT_LETTERS; none
 * empties it) */
static void app_edit(uint32_t f, int32_t steps, uint32_t shift)
{
    static const char LETTERS[] = " " PROJECT_EFFECT_LETTERS;
    uint32_t v = project_field(app.track, app.cursor, f);
    int32_t lo = 0, hi = 0;
    if (f == PROJECT_F_EFFECT) {
        uint32_t i = 0;
        while (LETTERS[i] && (uint32_t)(uint8_t)LETTERS[i] != v >> 8)
            i++;
        if (shift) {
            i = (uint32_t)app_clamp((int32_t)(LETTERS[i] ? i : 0u) + steps, 0, (int32_t)sizeof LETTERS - 2);
            v = i ? PROJECT_EFFECT((uint8_t)LETTERS[i], v & 0xFFu) : PROJECT_EMPTY;
        } else if (v) {
            v = PROJECT_EFFECT(v >> 8, (uint32_t)app_clamp((int32_t)(v & 0xFFu) + steps, 0, 255));
        }
        project_set_field(app.track, app.cursor, f, v);
        return;
    }
    if (shift) {
        project_set_field(app.track, app.cursor, f, PROJECT_EMPTY);
        return;
    }
    if (f == PROJECT_F_NOTE) {
        lo = (int32_t)PROJECT_NOTE(VT_NOTE_LO);
        hi = (int32_t)PROJECT_NOTE(VT_NOTE_HI);
    } else {
        lo = 1;                          /* PROJECT_INS(0), PROJECT_VOL(0) */
        hi = f == PROJECT_F_INS ? (int32_t)PROJECT_INS(PROJECT_INSTRUMENTS - 1u)
                                 : (int32_t)PROJECT_VOL(PROJECT_VOLUMES - 1u);
    }
    if (v == PROJECT_EMPTY || (f == PROJECT_F_NOTE && !PROJECT_IS_NOTE(v))) {
        v = app.last[f];
        steps -= steps > 0 ? 1 : -1;     /* the first detent: the last value */
    }
    v = (uint32_t)app_clamp((int32_t)v + steps, lo, hi);
    project_set_field(app.track, app.cursor, f, v);
    app.last[f] = (uint8_t)v;
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

static void app_pads(void)           /* a pad newly hit: the cursor to its row of the page (past the end: nothing) */
{
    uint32_t p, held = 0;
    for (p = 0; p < SMK_NPAD; p++)
        held |= (uint32_t)((fm1_in.pad_st[p] & 0x80u) != 0) << p;
    for (p = 0; p < SMK_NPAD; p++)
        if ((held & ~app.pads_held) >> p & 1u && app_page() + p < project_rows(app.track))
            app_cursor((int32_t)(app_page() + p));
    app.pads_held = (uint16_t)held;
}

static void app_track(int32_t steps)  /* K3 turned: the next track up or down, stopping at 1 and 4 */
{
    int32_t t = (int32_t)app.track + steps;
    t = t < 0 ? 0 : t >= (int32_t)PLAYER_TRACKS ? (int32_t)PLAYER_TRACKS - 1 : t;
    if (t != app.track) {
        app.track = (uint8_t)t;
        app_cursor(app.cursor);
        app_redraw();
    }
}

static void app_input(void)          /* take every input event */
{
    uint32_t e, pressed, shift = fm1_in.buttons >> SMK_BTN_KNOB_BANK & 1u;
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
    if (pressed >> SMK_BTN_LEFT & 1u)
        app_page_or_pattern(-1, shift);
    if (pressed >> SMK_BTN_RIGHT & 1u)
        app_page_or_pattern(1, shift);
    fm1_input_note_edges();
    while (smk_extkey_take() >= 0)
        ;
    for (e = 0; e < FM1_NENC; e++) {
        int32_t s = fm1_enc_take(e);
        if (!s)
            continue;
        if (e == VT_ENC_CURSOR && shift) {   /* shift + K2: the pattern's length */
            project_set_rows(app.track, (int32_t)project_rows(app.track) + s);
            app_cursor(app.cursor);
        } else if (e == VT_ENC_CURSOR) {
            app_cursor((int32_t)app.cursor + s);
        }
        if (e == VT_ENC_TRACK)
            app_track(s);
        if (e == VT_ENC_TEMPO)
            project_set_bpm((int32_t)project_bpm() + s);
        if (e >= VT_ENC_NOTE && e < VT_ENC_NOTE + 4u)
            app_edit(e - VT_ENC_NOTE, s, shift);
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

static void app_hex(uint32_t v, uint32_t n, char *s)   /* v as n hex digits */
{
    static const char HEX[] = "0123456789ABCDEF";
    s[n] = 0;
    while (n--) {
        s[n] = HEX[v & 15u];
        v >>= 4;
    }
}

static void app_field(char *s, uint32_t f, uint32_t v)   /* a field as the tracker writes it; empty: dots */
{
    if (f == PROJECT_F_NOTE) {
        app_note_name(v, s);
    } else if (!v) {                     /* an empty instrument "..", volume ".", effect "..." */
        uint32_t k = f == PROJECT_F_INS ? 1u : f == PROJECT_F_VOL ? 2u : 0u;
        memcpy(s, &"..."[k], 4u - k);
    } else if (f == PROJECT_F_EFFECT) {
        s[0] = (char)(v >> 8);
        app_hex(v & 0xFFu, 2, s + 1);
    } else {
        app_hex(v - 1u, f == PROJECT_F_INS ? 2u : 1u, s);
    }
}

static void app_draw_field(int32_t x, uint32_t f, uint32_t v, uint16_t ink, uint16_t bg)
{
    char s[4];
    app_field(s, f, v);
    cv_text_mono(x, 0, &AF_S, s, v ? ink : VT_C_EMPTY, bg, VT_PITCH);
}

static void app_draw_line(uint32_t i, const vt_line_t *l)   /* screen line i */
{
    static const uint16_t BG[4] = {VT_C_BG, VT_C_CURSOR, VT_C_PLAYING, VT_C_CURSOR_PLAYING};
    uint16_t bg = BG[l->mark & (VT_LINE_CURSOR | VT_LINE_PLAYING)];
    uint32_t t, k = 0;
    cv_begin(240, VT_ROW_H, bg);
    cv_rect(VT_X_SEP, 0, 1, VT_ROW_H, VT_C_SEP);
    if (!(l->mark & VT_LINE_PAST)) {
        char num[3];
        app_hex(l->row, 2, num);
        cv_text_mono(VT_X_NUM, 0, &AF_S, num, l->row % 4u ? VT_C_NUM : VT_C_NUM_BEAT, bg, VT_PITCH);
        app_draw_field(VT_X_NOTE, PROJECT_F_NOTE, l->note[app.track], VT_C_TEXT, bg);
        app_draw_field(VT_X_INS, PROJECT_F_INS, l->ins, VT_C_INS, bg);
        app_draw_field(VT_X_VOL, PROJECT_F_VOL, l->vol, VT_C_VOL, bg);
        app_draw_field(VT_X_EFFECT, PROJECT_F_EFFECT, l->effect, VT_C_EFFECT, bg);
        for (t = 0; t < PROJECT_TRACKS; t++)
            if (t != app.track)
                app_draw_field(VT_X_OTHER + (int32_t)(k++) * VT_OTHER_W, PROJECT_F_NOTE, l->note[t], VT_C_OTHER, bg);
    }
    cv_blit(0, VT_HEAD_H + i * VT_ROW_H);
}

static uint32_t app_head_key(void)   /* what the header shows, as one word (never 0) */
{
    return 1u | (uint32_t)app.track << 1 | (uint32_t)app.rec << 3 | project_bpm() << 4 |
           project_rows(app.track) << 13 | project_pattern(app.track) << 20;
}

static void app_draw_head(void)
{
    char s[12];
    uint32_t b = project_bpm(), n = 0;
    cv_begin(240, VT_HEAD_H, VT_C_HEAD);
    s[0] = 'T';
    s[1] = (char)('1' + app.track);
    s[2] = 0;
    cv_text(8, 1, &AF_S, s, VT_C_TEXT);
    cv_text(26, 1, &AF_S, player_track_name(app.track), VT_C_TEXT);
    s[0] = 'P';
    app_hex(project_pattern(app.track), 2, s + 1);
    cv_text_r(232, 1, &AF_S, s, VT_C_TEXT, VT_C_HEAD);
    s[0] = (char)('0' + project_rows(app.track) / 10u);
    s[1] = (char)('0' + project_rows(app.track) % 10u);
    memcpy(s + 2, " ROWS", 6);
    cv_text_r(232, 15, &AF_S, s, VT_C_HEAD_DIM, VT_C_HEAD);
    if (b >= 100u)
        s[n++] = (char)('0' + b / 100u);
    s[n++] = (char)('0' + b / 10u % 10u);
    s[n++] = (char)('0' + b % 10u);
    memcpy(s + n, " BPM", 5);
    n = (uint32_t)cv_text(8, 15, &AF_S, s, VT_C_HEAD_DIM);
    if (app.rec)
        cv_text((int32_t)n + 12, 15, &AF_S, "REC", VT_C_HEAD_DIM);
    cv_blit(0, 0);
}

static void app_draw(void)           /* the header if it changed, then each line that changed */
{
    uint32_t i, t, playing = player_playing(), prow = player_row(), rows = project_rows(app.track);
    if (app.head != app_head_key()) {
        app_draw_head();
        app.head = app_head_key();
    }
    for (i = 0; i < VT_SHOWN; i++) {
        vt_line_t l;
        uint32_t row = app.top + i;
        memset(&l, 0, sizeof l);
        l.row = (uint8_t)row;
        if (row >= rows) {
            l.mark = VT_LINE_PAST;
        } else {
            for (t = 0; t < PROJECT_TRACKS; t++)
                l.note[t] = (uint8_t)project_field(t, row, PROJECT_F_NOTE);
            l.ins = (uint8_t)project_field(app.track, row, PROJECT_F_INS);
            l.vol = (uint8_t)project_field(app.track, row, PROJECT_F_VOL);
            l.effect = (uint16_t)project_field(app.track, row, PROJECT_F_EFFECT);
            l.mark = (uint8_t)((row == app.cursor ? VT_LINE_CURSOR : 0u) |
                               (playing && row == prow ? VT_LINE_PLAYING : 0u));
        }
        if (memcmp(&l, &app.shown[i], sizeof l)) {
            app_draw_line(i, &l);
            app.shown[i] = l;
        }
    }
}

static void app_leds(void)           /* the pads: the rows of the page; SEQ PLAY and SEQ REC */
{
    uint32_t p, page = app_page(), playing = player_playing(), prow = player_row();
    for (p = 0; p < SMK_NPAD; p++) {
        uint32_t row = page + p;
        const uint8_t *c = row >= project_rows(app.track) ? VT_PAD_PAST
                           : playing && row == prow   ? VT_PAD_PLAYING
                           : row == app.cursor      ? VT_PAD_CURSOR
                           : project_field(app.track, row, PROJECT_F_NOTE) ? VT_PAD_NOTE
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
