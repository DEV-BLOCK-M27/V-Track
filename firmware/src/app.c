/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The V-Track app. main.c calls it:
 *   app_boot_screen()   at power-on, before the audio and the input start: the boot screen; a new project, the player
 *                       starts
 *   app_start()         once the boot screen has been shown (and after an update session): the app's screen
 *   app_frame()         each main-loop frame (~60 a second): input in, LEDs and display out
 *   app_idle()          between frames, as often as the main loop gets round to it
 *   app_audio(out, n)   in the audio ISR: n stereo frames, Q15, interleaved L R
 * Three views. The pattern view (docs/design/pattern-view.png): a header (the track, its chip and channel, the
 * modes LOOP REC VEL, its pattern Pxx; the tempo, the selected instrument, the pattern's length) over 13 rows of
 * the selected track's pattern, all four columns (note, instrument, volume, effect), and the other three tracks'
 * note columns right of a separator. The cursor row has a blue bar, the playing row a red one (both: purple). The
 * screen follows the cursor, scrolling as little as keeps it shown; a page turned shows the page from its first row.
 * K2 moves the cursor (it stops at the first and last row), shift (KNOB BANK) + K2 sets the pattern's length (16 to
 * 64 rows). K3 selects the track (1 to 4, it stops at the ends), K4 sets the tempo (1 BPM a detent). K5 to K8 edit
 * the cursor row's note (a semitone a detent, C-0 to B-7), instrument (00 to 0F), volume (0 to F) and effect
 * parameter (00 to FF): an empty field takes the value written last first (C-4, 00, F at the start); shift + K5 to
 * K7 empties the field, shift + K8 picks the effect's letter (none, 0 1 2 3 4 A B D F G S V; none empties it).
 * < and > turn the page (16 rows; the cursor moves with it, the last page may be shorter), shift + < and > select
 * the track's pattern (P00 to P1F). The keys play the selected track (player.c), one note at a time a track: the
 * last key pressed takes the channel, its release silences it; a held note stays on its track.
 * The pattern view shows the current entry's patterns (the order list's cursor); the playing row only while that
 * entry plays. SEQ PLAY starts and stops the order list from the current entry (player.c: it loops at its end),
 * shift + SEQ PLAY switches the pattern-loop mode (the current entry over and over; LOOP in the header), SEQ REC
 * switches recording (both LEDs show it), shift + SEQ REC velocity recording (on at the start; VEL in the header).
 * With REC on and the sequencer stopped a key also writes its note into the cursor row, and shift with a white key
 * a cut, with a black key a release (shift keeps the keys from playing); the cursor then moves down a row (after
 * the last row, to the first). With REC on and the sequencer playing they land in the row playing (of the entry
 * playing) instead; the cursor stays. With velocity recording on a note also writes the key's velocity into the
 * volume column (velocity / 8, at least 1: 1 to F). PARA clears the cursor row, CHORD inserts an empty row there
 * (the rows below a row down), shift + CHORD deletes it (the rows below a row up); SCALE copies the track's pattern,
 * shift + SCALE pastes it into the track's pattern (shift + < > select another number first). Shift + PARA undoes
 * the last edit of a pattern (one level): a clear, insert, delete, paste, a key recorded, the turns of K5 to K8 on
 * one field of one row, the turns of shift + K2 (the length). The pads show the 16 rows of the cursor's page:
 * empty, a note (cut and release too), the cursor, the playing row, dark past the pattern's end; a tap moves the
 * cursor to its row.
 * NOTE REPEAT switches the pads to instrument mode and back (its LED lit in instrument mode): pads 1 to 16 are
 * instruments 00 to 0F, those of the selected track's chip lit (the selected one brighter), the others (free slots,
 * other chips) dark, taking no tap. A tap selects the instrument: the header shows it (INSxx and its name), the keys
 * play it, step entry writes it into the instrument column with the note, and K6 on an empty instrument starts
 * from it. None is selected at first (the keys play a plain tone, step entry leaves the column empty); a track of
 * another chip drops it.
 * PATCH opens the instrument editor on the selected instrument (none: the chip's first, which is selected), SEQ goes
 * back to the pattern view; PATCH's LED is lit in the editor. A header (INSxx, its name with the name's cursor under
 * a character, the chip; the track the keys play) over the four sequences' panels: VOL, ARP, PITCH, DUTY (MODE on
 * the noise track), each its length, loop and release point over a bar graph of its 64 steps (the length's on a
 * lighter ground; a green line before the loop point's step, an orange one before the release point's); the selected
 * panel lighter, with the cursor's step brighter and its number and value at the left. K2 moves the cursor along the
 * steps, K3 selects the sequence, K5 sets the step's value (within project.c's PROJECT_SEQ_MIN .. MAX, the mode
 * 0..1 on the noise track; a step past the end lengthens the sequence to it, from what it holds), K6 the length (0 to 64), K7 the loop point and K8 the release point (none,
 * then the steps within the length); K4 sets the name's character at the name's cursor (space, A to Z, 0 to 9, -),
 * shift + K4 moves that cursor (7 characters). All of them stop at their ends. The pads are the instruments as in
 * instrument mode; a tap edits that one, shift + a free slot's pad copies the edited instrument there and edits the
 * copy. The keys play the edited instrument on the selected track (step entry is off), so a held note plays each
 * edited step it reaches (one it has played to the end of no more: the player's rules, player.c).
 * ARP opens the order list, SEQ goes back to the pattern view; ARP's LED is lit there. A header (ORDER, the tempo,
 * SONG or LOOP; the tracks' columns T1 to T4, the selected one brighter) over 13 entries: the entry's number and
 * the pattern it names for each track. The cursor's entry (the current one) has a blue bar, the playing one a red
 * one (both: purple); the screen follows the cursor. K2 moves the cursor, shift + K2 moves the cursor's entry with
 * it; K5 to K8 set its pattern on tracks 1 to 4 (P00 to P1F); K3 and K4 select the track and set the tempo as in
 * the pattern view. > adds an entry after the cursor's (new patterns: each track's first one no entry names), shift
 * + > a copy of it, the cursor to it; shift + < deletes the cursor's entry (not the last one; < alone: nothing). The
 * pads show the 16 entries of the cursor's page (an entry, the cursor, the playing one, dark past the end); a tap
 * moves the cursor there. The keys play the selected track (step entry is off).
 * GLOBE opens the project view (store.c), SEQ goes back to the pattern view; GLOBE's LED is lit there. A header
 * (PROJECT, the project's name with the name's cursor under a character; the project memory's fullness, MEMORY n%,
 * over a bar) over the four slots (the number and the name of the project saved there, or EMPTY; the selected one
 * with a blue bar), a message line and two lines of help. K2 selects the slot (pads 1 to 4 too: the selected one
 * bright, a saved project dim), K4 sets the name's character at its cursor, shift + K4 moves the cursor (as the
 * editor's name). > saves the project to the slot (over a saved project it asks first: > again); < loads the slot's
 * project (it asks first: < again), shift + < starts a new project (it asks first: shift + < again). Loading and a
 * new project stop the sequencer; the views start from the new project's first row, no instrument selected. A
 * project the memory has no room for is refused with NOT SAVED: MEMORY FULL (the slot keeps what it held); an
 * autosave the memory has no room for shows AUTOSAVE: MEMORY FULL. Any other button or encoder takes the question
 * back. K3 selects the track, the keys play it (step entry is off).
 * Every other input (USB MIDI in too) is taken and dropped. */
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
#define VT_EDIT_LENGTH 4u        /* app.edit.what: the pattern's length (shift + K2), past the PROJECT_F_* fields */
#define VT_EDIT_NONE 0xFFu       /* .. no field edit going on */
#define VT_NO_INS PLAYER_NO_INS  /* no instrument selected */
#define VT_VIEW_PATTERN 0u       /* app.view */
#define VT_VIEW_EDITOR 1u
#define VT_VIEW_ORDER 2u
#define VT_VIEW_PROJECT 3u
#define VT_ASK_NONE 0u           /* app.ask: the project view's action waiting for its second press */
#define VT_ASK_SAVE 1u
#define VT_ASK_LOAD 2u
#define VT_ASK_NEW 3u
#define VT_P_SLOT_Y 38           /* the project view: the slots' lines, the message's, the help lines' */
#define VT_P_MSG_Y 110
#define VT_P_HELP_Y 196
#define VT_C_FULL RGB(230, 80, 60)   /* the memory's bar, nearly full */
#define VT_O_X 64                /* the order list: each track's pattern column, VT_O_W apart */
#define VT_O_W 44
#define VT_ENC_STEP 1u           /* the editor: K2 step, K3 sequence, K4 name, K5 value, K6 length, K7 loop, */
#define VT_ENC_SEQ 2u            /* K8 release */
#define VT_ENC_NAME 3u
#define VT_ENC_VALUE 4u
#define VT_ENC_LEN 5u
#define VT_ENC_LOOP 6u
#define VT_ENC_RELEASE 7u
#define VT_SEQ_H 52u             /* a sequence's panel: a label line over its bar graph */
#define VT_SEQ_LABEL_H 14
#define VT_GRAPH_X 46            /* the graph: a step every VT_STEP_W px, VT_GRAPH_H rows */
#define VT_GRAPH_H 36
#define VT_STEP_W 3
#define VT_NAME_X 50             /* the editor's name, monospaced, a cell each character */
#define VT_C_SEQ_SEL RGB(14, 14, 30)  /* the selected sequence's panel */
#define VT_C_GRAPH RGB(36, 36, 56)    /* the graph within the length, past it */
#define VT_C_GRAPH_PAST RGB(10, 10, 14)
#define VT_C_BAR RGB(90, 190, 230)
#define VT_C_BAR_STEP RGB(240, 240, 240)   /* the step the cursor is on */
#define VT_C_STEP RGB(50, 60, 110)    /* .. its column */
#define VT_C_LOOP RGB(90, 210, 90)
#define VT_C_RELEASE RGB(230, 120, 60)

typedef struct {                 /* what a sequence's panel shows: drawn again when it changes */
    project_seq_t q;
    uint8_t ins, sel, step, mode;
} vt_panel_t;

typedef struct {                 /* what an order list line shows: drawn again when it changes */
    uint8_t entry, mark, track, pat[PROJECT_TRACKS];
} vt_oline_t;

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
    uint8_t vel;                 /* recording writes the key's velocity into the volume column */
    struct {                     /* the field edit going on: its turns are one undo step */
        uint8_t track, pattern, row, what;   /* what: a PROJECT_F_* field, VT_EDIT_LENGTH; VT_EDIT_NONE: none */
    } edit;
    uint8_t pad_ins;             /* the pads in instrument mode */
    uint8_t ins;                 /* the selected instrument 0..15, VT_NO_INS */
    uint8_t last[PROJECT_F_EFFECT];   /* by PROJECT_F_*: the note, instrument and volume written last (an empty
                                       * field takes it first) */
    uint64_t keys;               /* the keybed keys held at the last frame */
    uint8_t key_track[FM1_NKEY]; /* the track each held key plays on */
    uint16_t pads_held;          /* the pads held at the last frame, a bit each */
    vt_line_t shown[VT_SHOWN];   /* what each screen line shows */
    uint8_t view;                /* VT_VIEW_* */
    uint8_t seq, step, name_pos; /* the editor: its sequence, step, the name's cursor */
    vt_panel_t panel[PROJECT_SEQS];   /* what each sequence's panel shows */
    uint8_t otop;                /* the order list: the first entry on the screen */
    vt_oline_t oshown[VT_SHOWN]; /* what each of its lines shows */
    uint8_t slot, pname_pos, ask;   /* the project view: the slot, the name's cursor, the action asked (VT_ASK_*) */
    char msg[32];                /* .. its message */
} app;

static const uint8_t VT_PAD_EMPTY[3] = {2, 2, 6}, VT_PAD_NOTE[3] = {0, 20, 60}, VT_PAD_CURSOR[3] = {60, 60, 60},
                     VT_PAD_PLAYING[3] = {0, 90, 10}, VT_PAD_PAST[3] = {0, 0, 0},   /* r g b */
                     VT_PAD_INS[3] = {10, 40, 10}, VT_PAD_INS_SEL[3] = {60, 110, 20};

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
    store_boot();                        /* the autosaved project, or a new one */
    player_init();                       /* before the audio ISR runs it */
}

static void app_redraw(void)         /* the header and every line again */
{
    app.head = 0;
    memset(app.shown, 0xFF, sizeof app.shown);   /* (no line shows row 0xFF) */
    memset(app.panel, 0xFF, sizeof app.panel);   /* (no panel shows instrument 0xFF) */
    memset(app.oshown, 0xFF, sizeof app.oshown); /* (no line shows entry 0xFF) */
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
    app.vel = 1;
    app.edit.what = VT_EDIT_NONE;
    app.pad_ins = 0;
    app.ins = VT_NO_INS;
    app.last[PROJECT_F_NOTE] = PROJECT_NOTE(60u);   /* C-4 */
    app.last[PROJECT_F_INS] = PROJECT_INS(0u);
    app.last[PROJECT_F_VOL] = PROJECT_VOL(PROJECT_VOLUMES - 1u);
    app.pads_held = 0;
    app.keys = fm1_in.keys;              /* keys held already: no note until pressed again */
    app.view = VT_VIEW_PATTERN;
    app.seq = app.step = app.name_pos = 0;
    app.otop = 0;
    app.slot = app.pname_pos = 0;
    app.ask = VT_ASK_NONE;
    app.msg[0] = 0;
    lcd_fill(0, 0, 240, 240, VT_C_BG);
}

static uint32_t app_chip(uint32_t track)   /* the chip track 0..3 plays (ADR 0002; so far the NES only) */
{
    (void)track;
    return PROJECT_CHIP_NES;
}

static int app_ins_offered(uint32_t i)   /* instrument i is one of the selected track's chip */
{
    return project_ins_chip(i) == app_chip(app.track);
}

static uint32_t app_noise(void)      /* the noise track: the duty sequence is the noise's mode (0..1) */
{
    return app.track == PLAYER_NOISE;
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

static int app_entry_playing(void)   /* the current entry plays (the pattern view's rows) */
{
    return player_playing() && player_entry() == project_entry();
}

static void app_bpm_text(char *s)    /* the project's tempo: "120 BPM" */
{
    uint32_t b = project_bpm(), n = 0;
    if (b >= 100u)
        s[n++] = (char)('0' + b / 100u);
    s[n++] = (char)('0' + b / 10u % 10u);
    s[n++] = (char)('0' + b % 10u);
    memcpy(s + n, " BPM", 5);
}

static void app_cursor(int32_t r)    /* the cursor to row r, within the pattern; the screen follows */
{
    uint8_t c = (uint8_t)app_clamp(r, 0, (int32_t)project_rows(app.track) - 1);
    if (c != app.cursor && app.edit.what != VT_EDIT_LENGTH)
        app.edit.what = VT_EDIT_NONE;    /* (a field edit ends with the cursor leaving its row) */
    app.cursor = c;
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

static void app_undo_step_new(void) { app.edit.what = VT_EDIT_NONE; }   /* the next field edit: its own undo step */

/* REC: v (a note, cut or release; a key's velocity vel) into the selected track's row: stopped the cursor row (the
 * cursor then down a row), playing the row playing (of the entry playing). A note also writes the selected
 * instrument, and its velocity as the volume (1 to F) unless velocity recording is off. One undo step */
static void app_record(uint32_t v, uint32_t vel)
{
    uint32_t playing = player_playing(), e, row, p;
    do {                                 /* the entry and row playing, as one (the audio ISR moves them on) */
        row = player_row();
        e = player_entry();
    } while (playing && row != player_row());
    p = playing ? project_order(e, app.track) : project_pattern(app.track);
    row = playing ? row : app.cursor;
    project_checkpoint_of(app.track, p);
    app_undo_step_new();
    project_set_field_of(app.track, p, row, PROJECT_F_NOTE, v);
    if (PROJECT_IS_NOTE(v)) {
        app.last[PROJECT_F_NOTE] = (uint8_t)v;
        if (app.ins != VT_NO_INS)
            project_set_field_of(app.track, p, row, PROJECT_F_INS, PROJECT_INS(app.ins));
        if (app.vel)
            project_set_field_of(app.track, p, row, PROJECT_F_VOL, PROJECT_VOL(vel >> 3 ? vel >> 3 : 1u));
    }
    if (!playing)
        app_cursor(app.cursor + 1u < project_rows(app.track) ? (int32_t)app.cursor + 1 : 0);
}

/* a field edit about to change what (a PROJECT_F_* field of the cursor row, VT_EDIT_LENGTH): a new undo step,
 * unless it goes on with the one before (the same field of the same row, the same length, turned again) */
static void app_undo_step_join(uint32_t what)
{
    uint32_t pattern = project_pattern(app.track), row = what == VT_EDIT_LENGTH ? 0u : app.cursor;
    if (app.edit.what == what && app.edit.track == app.track && app.edit.pattern == pattern && app.edit.row == row)
        return;
    project_checkpoint(app.track);
    app.edit.track = app.track, app.edit.pattern = (uint8_t)pattern, app.edit.row = (uint8_t)row;
    app.edit.what = (uint8_t)what;
}

/* the pattern view's editing buttons: PARA clears the cursor row, shift + PARA undoes the last edit; CHORD inserts
 * a row at the cursor, shift + CHORD deletes it; SCALE copies the pattern, shift + SCALE pastes it */
static void app_edit_button(uint32_t b, uint32_t shift)
{
    app_undo_step_new();
    if (b == SMK_BTN_PARA && shift)
        project_undo();
    else if (b == SMK_BTN_PARA)
        project_clear_row(app.track, app.cursor);
    else if (b == SMK_BTN_CHORD && shift)
        project_delete_row(app.track, app.cursor);
    else if (b == SMK_BTN_CHORD)
        project_insert_row(app.track, app.cursor);
    else if (b == SMK_BTN_SCALE && shift)
        project_paste(app.track);
    else if (b == SMK_BTN_SCALE)
        project_copy(app.track);
    app_cursor(app.cursor);              /* (an undo or paste may change the length) */
}

static void app_write_field(uint32_t f, uint32_t v)   /* field f of the cursor row set to v: an edit if it changes */
{
    if (v == project_field(app.track, app.cursor, f))
        return;                          /* (a turn at an end: the undo step before it kept) */
    app_undo_step_join(f);
    project_set_field(app.track, app.cursor, f, v);
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
        app_write_field(f, v);
        return;
    }
    if (shift) {
        app_write_field(f, PROJECT_EMPTY);
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
    app_write_field(f, v);
    app.last[f] = (uint8_t)v;
}

static int app_black(uint32_t k)     /* keybed key k is a black key */
{
    return (0x54Au >> ((VT_KEY_NOTE0 + k) % 12u)) & 1u;
}

static void app_keys(void)           /* the keybed: releases, then the highest key newly pressed */
{
    uint64_t now = fm1_in.keys, down = now & ~app.keys, up = app.keys & ~now;
    uint32_t k, shift = fm1_in.buttons >> SMK_BTN_KNOB_BANK & 1u, enter = app.rec && app.view == VT_VIEW_PATTERN;
    app.keys = now;
    for (k = 0; k < FM1_NKEY; k++)
        if (up >> k & 1u)
            player_note_off(app.key_track[k], VT_KEY_NOTE0 + k);
    for (k = FM1_NKEY; k-- > 0;)
        if (down >> k & 1u) {
            if (shift) {                 /* shift: cut or release, no sound */
                if (enter)
                    app_record(app_black(k) ? PROJECT_RELEASE : PROJECT_CUT, 0);
                break;
            }
            app.key_track[k] = app.track;
            player_note_on(app.track, VT_KEY_NOTE0 + k, app.ins);
            if (enter)
                app_record(PROJECT_NOTE(VT_KEY_NOTE0 + k), fm1_in.key_vel[k]);
            break;
        }
}

/* --- the instrument editor (PATCH) --- */

static void app_view(uint32_t v)     /* PATCH: the editor for the selected instrument (none: the chip's first); SEQ:
                                      * the pattern view; ARP: the order list */
{
    uint32_t i;
    if (v == VT_VIEW_EDITOR && app.ins == VT_NO_INS)
        for (i = 0; i < PROJECT_INSTRUMENTS && app.ins == VT_NO_INS; i++)
            if (app_ins_offered(i))
                app.ins = (uint8_t)i;
    if (v == VT_VIEW_EDITOR && app.ins == VT_NO_INS)
        return;                          /* (no instrument of the chip at all) */
    if (v == VT_VIEW_EDITOR)
        app.last[PROJECT_F_INS] = (uint8_t)PROJECT_INS(app.ins);
    if (v == VT_VIEW_PATTERN)
        app_cursor(app.cursor);          /* (the entry's patterns may be shorter) */
    if (v == VT_VIEW_PROJECT && v != app.view)
        app.ask = VT_ASK_NONE, app.msg[0] = 0;
    if (v != app.view) {
        app.view = (uint8_t)v;
        app_redraw();
        lcd_fill(0, 0, 240, 240, VT_C_BG);
    }
}

/* name n with its character at pos turned steps through CHARS (stopping at the ends), into name (padded with spaces) */
static void app_name_turn(char *name, const char *n, uint32_t pos, int32_t steps)
{
    static const char CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
    uint32_t k, c = 0, end = 0;
    for (k = 0; k + 1u < PROJECT_NAME; k++)
        name[k] = end || !n[k] ? (end = 1, ' ') : n[k];
    name[PROJECT_NAME - 1u] = 0;
    while (CHARS[c] && CHARS[c] != name[pos])
        c++;
    c = (uint32_t)app_clamp((int32_t)(CHARS[c] ? c : 0u) + steps, 0, (int32_t)sizeof CHARS - 2);
    name[pos] = CHARS[c];
}

static void app_editor_edit(uint32_t e, int32_t steps, uint32_t shift)   /* an encoder turned in the editor */
{
    const project_seq_t *q;
    if (e == VT_ENC_NAME && shift)
        app.name_pos = (uint8_t)app_clamp((int32_t)app.name_pos + steps, 0, PROJECT_NAME - 2);
    if (e == VT_ENC_NAME && !shift) {    /* the character at the name's cursor */
        char name[PROJECT_NAME];
        app_name_turn(name, project_ins_name(app.ins), app.name_pos, steps);
        project_set_ins_name(app.ins, name);
    }
    if (e == VT_ENC_STEP)
        app.step = (uint8_t)app_clamp((int32_t)app.step + steps, 0, PROJECT_SEQ_STEPS - 1);
    if (e == VT_ENC_SEQ)
        app.seq = (uint8_t)app_clamp((int32_t)app.seq + steps, 0, PROJECT_SEQS - 1);
    q = project_seq(app.ins, app.seq);
    if (e == VT_ENC_VALUE) {             /* (past the end: from what the step holds; the sequence grows to it) */
        int32_t v = q->v[app.step] + steps;
        if (app.seq == PROJECT_SEQ_DUTY && app_noise())   /* the noise's mode: 0..1 */
            v = app_clamp(v, 0, 1);
        project_set_step(app.ins, app.seq, app.step, v);
    }
    if (e == VT_ENC_LEN)
        project_set_len(app.ins, app.seq, (int32_t)q->len + steps);
    if (e == VT_ENC_LOOP || e == VT_ENC_RELEASE) {   /* none (-1), then the steps 00 .. the last */
        int32_t k = e == VT_ENC_LOOP ? q->loop : q->release;
        k = app_clamp((k == PROJECT_SEQ_NONE ? -1 : k) + steps, -1, (int32_t)q->len - 1);
        if (e == VT_ENC_LOOP)
            project_set_loop(app.ins, app.seq, k < 0 ? PROJECT_SEQ_NONE : (uint32_t)k);
        else
            project_set_release(app.ins, app.seq, k < 0 ? PROJECT_SEQ_NONE : (uint32_t)k);
    }
}

/* a pad hit in the editor: its instrument edited; with shift, a free slot's: the instrument copied there, the copy
 * edited (shift + another instrument's: nothing) */
static void app_editor_pad(uint32_t p, uint32_t shift)
{
    if (shift ? project_ins_copy(app.ins, p) : app_ins_offered(p)) {
        app.ins = (uint8_t)p;
        app.last[PROJECT_F_INS] = (uint8_t)PROJECT_INS(p);
    }
}

/* --- the order list (ARP) --- */

static uint32_t app_opage(void) { return project_entry() / VT_PAGE * VT_PAGE; }   /* the entry's page */

static void app_entry(int32_t e)     /* the order list's cursor (the current entry) to e; the screen follows */
{
    project_set_entry(e);
    e = (int32_t)project_entry();
    if (e < app.otop)
        app.otop = (uint8_t)e;
    if (e >= app.otop + (int32_t)VT_SHOWN)
        app.otop = (uint8_t)(e + 1 - (int32_t)VT_SHOWN);
}

/* > : a new entry after the cursor's (each track's first pattern no entry names), shift + > : a copy of it; the
 * cursor to it. shift + < : the cursor's entry deleted (not the last one left) */
static void app_order_button(int32_t dir, uint32_t shift)
{
    uint32_t e = project_entry();
    if (dir > 0 && (shift ? project_order_dup(e) : project_order_add(e)))
        app_entry((int32_t)e + 1);
    if (dir < 0 && shift && project_order_delete(e))
        app_entry((int32_t)project_entry());
}

static void app_order_edit(uint32_t enc, int32_t steps, uint32_t shift)   /* an encoder turned in the order list */
{
    uint32_t e = project_entry();
    if (enc == VT_ENC_CURSOR)            /* K2: the cursor; shift: the entry moves with it */
        app_entry(shift ? (int32_t)project_order_move(e, steps) : (int32_t)e + steps);
    if (enc >= VT_ENC_NOTE && enc < VT_ENC_NOTE + PROJECT_TRACKS)   /* K5 to K8: track 1 to 4's pattern */
        project_set_order(e, enc - VT_ENC_NOTE, (int32_t)project_order(e, enc - VT_ENC_NOTE) + steps);
}

/* --- the project view (GLOBE) --- */

static void app_say(const char *a, int32_t slot, const char *b)   /* the message: a, the slot's number, b */
{
    uint32_t n = 0;
    while (*a && n + 1u < sizeof app.msg)
        app.msg[n++] = *a++;
    if (slot >= 0 && n + 1u < sizeof app.msg)
        app.msg[n++] = (char)('1' + slot);
    while (b && *b && n + 1u < sizeof app.msg)
        app.msg[n++] = *b++;
    app.msg[n] = 0;
}

static void app_project_reset(void)  /* another project: the views from its start */
{
    app.cursor = app.top = 0;
    app.otop = 0;
    app.edit.what = VT_EDIT_NONE;
    app.ins = VT_NO_INS;
    app_redraw();
}

/* > saves to the slot (over a saved project: asked first, > again), < loads it (asked first, < again), shift + <
 * starts a new project (asked first, shift + < again) */
static void app_project_button(int32_t dir, uint32_t shift)
{
    uint32_t want = dir > 0 ? VT_ASK_SAVE : shift ? VT_ASK_NEW : VT_ASK_LOAD;
    int rc;
    if (dir > 0 && shift) {
        app.ask = VT_ASK_NONE;
        return;
    }
    if (want == VT_ASK_LOAD && !store_used(app.slot)) {
        app.ask = VT_ASK_NONE;
        app_say("SLOT ", app.slot, " IS EMPTY");
        return;
    }
    if (app.ask != want && (want != VT_ASK_SAVE || store_used(app.slot))) {
        app.ask = (uint8_t)want;
        if (want == VT_ASK_SAVE)
            app_say("SAVE OVER SLOT ", app.slot, "? > AGAIN");
        else if (want == VT_ASK_LOAD)
            app_say("LOAD SLOT ", app.slot, "? < AGAIN");
        else
            app_say("NEW PROJECT? SHIFT < AGAIN", -1, 0);
        return;
    }
    app.ask = VT_ASK_NONE;
    if (want == VT_ASK_SAVE) {
        rc = store_save(app.slot);
        if (rc == STORE_OK)
            app_say("SAVED TO SLOT ", app.slot, 0);
        else
            app_say(rc == STORE_FULL ? "NOT SAVED: MEMORY FULL" : "NOT SAVED: FLASH ERROR", -1, 0);
        return;
    }
    player_stop();
    if (want == VT_ASK_NEW) {
        store_new();
        app_project_reset();
        app_say("NEW PROJECT", -1, 0);
        return;
    }
    rc = store_load(app.slot);
    if (rc == STORE_OK) {
        app_project_reset();
        app_say("LOADED SLOT ", app.slot, 0);
    } else {
        app_say("SLOT ", app.slot, " UNREADABLE");
    }
}

static void app_project_edit(uint32_t e, int32_t steps, uint32_t shift)   /* an encoder turned in the project view */
{
    app.ask = VT_ASK_NONE;
    if (e == VT_ENC_CURSOR) {            /* K2: the slot */
        app.slot = (uint8_t)app_clamp((int32_t)app.slot + steps, 0, STORE_SLOTS - 1);
        app.msg[0] = 0;
    }
    if (e == VT_ENC_NAME && shift)       /* shift + K4: the name's cursor; K4: its character */
        app.pname_pos = (uint8_t)app_clamp((int32_t)app.pname_pos + steps, 0, PROJECT_NAME - 2);
    if (e == VT_ENC_NAME && !shift) {
        char name[PROJECT_NAME];
        app_name_turn(name, project_name(), app.pname_pos, steps);
        project_set_name(name);
    }
}

/* a pad newly hit: the cursor to its row of the page (past the end: nothing); in instrument mode its instrument
 * selected (one of another chip or a free slot: nothing); in the editor that instrument edited; in the order list
 * the cursor to its entry of the page (past the end: nothing) */
static void app_pads(void)
{
    uint32_t p, held = 0, shift = fm1_in.buttons >> SMK_BTN_KNOB_BANK & 1u;
    for (p = 0; p < SMK_NPAD; p++)
        held |= (uint32_t)((fm1_in.pad_st[p] & 0x80u) != 0) << p;
    for (p = 0; p < SMK_NPAD; p++) {
        if (!((held & ~app.pads_held) >> p & 1u))
            continue;
        if (app.view == VT_VIEW_EDITOR) {
            app_editor_pad(p, shift);
        } else if (app.view == VT_VIEW_PROJECT) {
            if (p < STORE_SLOTS)
                app.slot = (uint8_t)p, app.ask = VT_ASK_NONE, app.msg[0] = 0;
        } else if (app.view == VT_VIEW_ORDER) {
            if (app_opage() + p < project_orders())
                app_entry((int32_t)(app_opage() + p));
        } else if (app.pad_ins && app_ins_offered(p)) {
            app.ins = (uint8_t)p;
            app.last[PROJECT_F_INS] = (uint8_t)PROJECT_INS(p);
        } else if (!app.pad_ins && app_page() + p < project_rows(app.track)) {
            app_cursor((int32_t)(app_page() + p));
        }
    }
    app.pads_held = (uint16_t)held;
}

static void app_track(int32_t steps)  /* K3 turned: the next track up or down, stopping at 1 and 4 */
{
    int32_t t = (int32_t)app.track + steps;
    t = t < 0 ? 0 : t >= (int32_t)PLAYER_TRACKS ? (int32_t)PLAYER_TRACKS - 1 : t;
    if (t != app.track) {
        app.track = (uint8_t)t;
        if (app.ins != VT_NO_INS && !app_ins_offered(app.ins))
            app.ins = VT_NO_INS;
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
    if (pressed >> SMK_BTN_SEQ_PLAY & 1u && shift) {   /* shift + SEQ PLAY: the pattern-loop mode */
        player_set_loop(!player_loop());
    } else if (pressed >> SMK_BTN_SEQ_PLAY & 1u) {
        if (player_playing())
            player_stop();
        else
            player_play();
    }
    if (pressed >> SMK_BTN_SEQ_REC & 1u && shift)   /* shift + SEQ REC: velocity recording */
        app.vel ^= 1u;
    else if (pressed >> SMK_BTN_SEQ_REC & 1u)
        app.rec ^= 1u;
    if (pressed >> SMK_BTN_NOTE_REPEAT & 1u)
        app.pad_ins ^= 1u;
    if (pressed >> SMK_BTN_PATCH & 1u)
        app_view(VT_VIEW_EDITOR);
    if (pressed >> SMK_BTN_SEQ & 1u)
        app_view(VT_VIEW_PATTERN);
    if (pressed >> SMK_BTN_ARP & 1u)
        app_view(VT_VIEW_ORDER);
    if (pressed >> SMK_BTN_GLOBE & 1u)
        app_view(VT_VIEW_PROJECT);
    if (app.view == VT_VIEW_PROJECT && pressed &
                                          ~(1u << SMK_BTN_LEFT | 1u << SMK_BTN_RIGHT | 1u << SMK_BTN_GLOBE |
                                            1u << SMK_BTN_KNOB_BANK))
        app.ask = VT_ASK_NONE;           /* (any other button but shift: the question is off) */
    if (app.view == VT_VIEW_PROJECT && pressed >> SMK_BTN_LEFT & 1u)
        app_project_button(-1, shift);
    if (app.view == VT_VIEW_PROJECT && pressed >> SMK_BTN_RIGHT & 1u)
        app_project_button(1, shift);
    if (app.view == VT_VIEW_ORDER && pressed >> SMK_BTN_LEFT & 1u)
        app_order_button(-1, shift);
    if (app.view == VT_VIEW_ORDER && pressed >> SMK_BTN_RIGHT & 1u)
        app_order_button(1, shift);
    if (app.view == VT_VIEW_PATTERN && pressed >> SMK_BTN_PARA & 1u)
        app_edit_button(SMK_BTN_PARA, shift);
    if (app.view == VT_VIEW_PATTERN && pressed >> SMK_BTN_CHORD & 1u)
        app_edit_button(SMK_BTN_CHORD, shift);
    if (app.view == VT_VIEW_PATTERN && pressed >> SMK_BTN_SCALE & 1u)
        app_edit_button(SMK_BTN_SCALE, shift);
    if (app.view == VT_VIEW_PATTERN && pressed >> SMK_BTN_LEFT & 1u)
        app_page_or_pattern(-1, shift);
    if (app.view == VT_VIEW_PATTERN && pressed >> SMK_BTN_RIGHT & 1u)
        app_page_or_pattern(1, shift);
    fm1_input_note_edges();
    while (smk_extkey_take() >= 0)
        ;
    for (e = 0; e < FM1_NENC; e++) {
        int32_t s = fm1_enc_take(e);
        if (!s)
            continue;
        if (app.view == VT_VIEW_EDITOR) {
            app_editor_edit(e, s, shift);
            continue;
        }
        if (app.view == VT_VIEW_PROJECT) {
            app_project_edit(e, s, shift);
            if (e == VT_ENC_TRACK)
                app_track(s);
            continue;
        }
        if (app.view == VT_VIEW_ORDER) {
            app_order_edit(e, s, shift);
            if (e == VT_ENC_TRACK)
                app_track(s);
            if (e == VT_ENC_TEMPO)
                project_set_bpm((int32_t)project_bpm() + s);
            continue;
        }
        if (e == VT_ENC_CURSOR && shift) {   /* shift + K2: the pattern's length */
            int32_t n = app_clamp((int32_t)project_rows(app.track) + s, (int32_t)PROJECT_ROWS_MIN, (int32_t)PROJECT_ROWS);
            if ((uint32_t)n != project_rows(app.track)) {
                app_undo_step_join(VT_EDIT_LENGTH);
                project_set_rows(app.track, n);
            }
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
           project_rows(app.track) << 13 | project_pattern(app.track) << 20 | (uint32_t)player_loop() << 25 |
           (uint32_t)((app.ins + 1u) & 0x1Fu) << 26 | (uint32_t)app.vel << 31;
}

static void app_draw_head(void)
{
    char s[12];
    int32_t x;
    cv_begin(240, VT_HEAD_H, VT_C_HEAD);
    s[0] = 'T';
    s[1] = (char)('1' + app.track);
    s[2] = 0;
    cv_text(8, 1, &AF_S, s, VT_C_TEXT);
    cv_text(26, 1, &AF_S, player_track_name(app.track), VT_C_TEXT);
    s[0] = 'P';
    app_hex(project_pattern(app.track), 2, s + 1);
    x = cv_text_r(232, 1, &AF_S, s, VT_C_TEXT, VT_C_HEAD);
    if (app.vel)                         /* the modes left of it: velocity recording, REC, the pattern loop */
        x = cv_text_r(x - 8, 1, &AF_S, "VEL", VT_C_HEAD_DIM, VT_C_HEAD);
    if (app.rec)
        x = cv_text_r(x - 8, 1, &AF_S, "REC", VT_C_HEAD_DIM, VT_C_HEAD);
    if (player_loop())
        cv_text_r(x - 8, 1, &AF_S, "LOOP", VT_C_HEAD_DIM, VT_C_HEAD);
    s[0] = (char)('0' + project_rows(app.track) / 10u);
    s[1] = (char)('0' + project_rows(app.track) % 10u);
    memcpy(s + 2, " ROWS", 6);
    x = cv_text_r(232, 15, &AF_S, s, VT_C_HEAD_DIM, VT_C_HEAD);
    if (app.ins != VT_NO_INS) {          /* the selected instrument: INSxx NAME */
        x = cv_text_r(x - 10, 15, &AF_S, project_ins_name(app.ins), VT_C_INS, VT_C_HEAD);
        memcpy(s, "INS", 3);
        app_hex(app.ins, 2, s + 3);
        cv_text_r(x - 5, 15, &AF_S, s, VT_C_INS, VT_C_HEAD);
    }
    app_bpm_text(s);
    cv_text(8, 15, &AF_S, s, VT_C_HEAD_DIM);
    cv_blit(0, 0);
}

static void app_draw(void)           /* the header if it changed, then each line that changed */
{
    uint32_t i, t, playing = app_entry_playing(), prow = player_row(), rows = project_rows(app.track);
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

static void app_signed(int32_t v, uint32_t plus, char *s)   /* v as 12, -3, 0; with plus +12 */
{
    uint32_t n = 0, m = (uint32_t)(v < 0 ? -v : v);
    if (v < 0 || (v && plus))
        s[n++] = v < 0 ? '-' : '+';
    if (m >= 100u)
        s[n++] = (char)('0' + m / 100u);
    if (m >= 10u)
        s[n++] = (char)('0' + m / 10u % 10u);
    s[n++] = (char)('0' + m % 10u);
    s[n] = 0;
}

static int32_t app_point(int32_t x, const char *what, uint32_t k, uint16_t ink)   /* "LOOP 03", "LOOP --" */
{
    char s[12];
    uint32_t n = 0;
    while (*what)
        s[n++] = *what++;
    s[n++] = ' ';
    if (k == PROJECT_SEQ_NONE)
        s[n] = s[n + 1u] = '-', s[n + 2u] = 0;
    else
        s[n] = (char)('0' + k / 10u), s[n + 1u] = (char)('0' + k % 10u), s[n + 2u] = 0;
    return cv_text(x, 1, &AF_S, s, ink) + 10;
}

/* sequence s's panel: its name, length, loop and release points (the selected one: the cursor's step and value);
 * its steps as bars (from the bottom, or from the middle for the signed arpeggio and pitch), the length's steps on a
 * lighter ground, the loop and release points as lines before their steps, the cursor's step brighter */
static void app_draw_panel(uint32_t s, const vt_panel_t *pl)
{
    static const char *const NAME[PROJECT_SEQS] = {"VOL", "ARP", "PITCH", "DUTY"};
    const project_seq_t *q = &pl->q;
    uint16_t bg = pl->sel ? VT_C_SEQ_SEL : VT_C_BG;
    int32_t lo = PROJECT_SEQ_MIN[s], hi = PROJECT_SEQ_MAX[s], y0 = VT_SEQ_LABEL_H, zero, x, k;
    char t[8];
    cv_begin(240, VT_SEQ_H, bg);
    cv_text(4, 1, &AF_S, s == PROJECT_SEQ_DUTY && pl->mode ? "MODE" : NAME[s], pl->sel ? VT_C_TEXT : VT_C_HEAD_DIM);
    memcpy(t, "LEN ", 4);
    t[4] = (char)('0' + q->len / 10u);
    t[5] = (char)('0' + q->len % 10u);
    t[6] = 0;
    x = cv_text(VT_GRAPH_X, 1, &AF_S, t, VT_C_HEAD_DIM) + 10;
    x = app_point(x, "LOOP", q->loop, VT_C_LOOP);
    app_point(x, "REL", q->release, VT_C_RELEASE);
    if (pl->sel) {                       /* the cursor's step and its value */
        app_hex(pl->step, 2, t);
        cv_text(4, y0 + 3, &AF_S, t, VT_C_HEAD_DIM);
        if (pl->step < q->len)
            app_signed(q->v[pl->step], lo < 0, t);
        else
            memcpy(t, "..", 3);
        cv_text(4, y0 + 19, &AF_S, t, VT_C_TEXT);
    }
    zero = lo < 0 ? y0 + VT_GRAPH_H / 2 : y0 + VT_GRAPH_H - 1;
    for (k = 0; k < (int32_t)PROJECT_SEQ_STEPS; k++) {
        int32_t bx = VT_GRAPH_X + k * VT_STEP_W, v, h;
        uint16_t ground = k < q->len ? VT_C_GRAPH : VT_C_GRAPH_PAST;
        if (pl->sel && k == pl->step)
            ground = VT_C_STEP;
        cv_rect(bx, y0, VT_STEP_W - 1, VT_GRAPH_H, ground);
        if (k >= q->len)
            continue;
        v = q->v[k];
        if (lo < 0)                      /* signed: up or down from the middle */
            h = v * (VT_GRAPH_H / 2 - 1) / hi;
        else
            h = (v - lo) * (VT_GRAPH_H - 1) / (hi - lo);
        if (h >= 0)
            cv_rect(bx, zero - h, VT_STEP_W - 1, h + 1, pl->sel && k == pl->step ? VT_C_BAR_STEP : VT_C_BAR);
        else
            cv_rect(bx, zero, VT_STEP_W - 1, 1 - h, pl->sel && k == pl->step ? VT_C_BAR_STEP : VT_C_BAR);
    }
    if (q->loop != PROJECT_SEQ_NONE)
        cv_rect(VT_GRAPH_X + q->loop * VT_STEP_W - 1, y0, 1, VT_GRAPH_H, VT_C_LOOP);
    if (q->release != PROJECT_SEQ_NONE)
        cv_rect(VT_GRAPH_X + q->release * VT_STEP_W - 1, y0, 1, VT_GRAPH_H, VT_C_RELEASE);
    cv_blit(0, VT_HEAD_H + s * VT_SEQ_H);
}

static uint32_t app_editor_head_key(void)   /* what the editor's header shows, as one word (never 0) */
{
    const char *n = project_ins_name(app.ins);
    uint32_t h = 1u | (uint32_t)app.ins << 1 | (uint32_t)app.track << 5 | (uint32_t)app.name_pos << 7, i;
    for (i = 0; n[i]; i++)
        h = h * 31u + (uint8_t)n[i];
    return h | 1u;
}

static void app_editor_head(void)     /* INSxx and its name (the name's cursor under it), the chip; the track */
{
    char s[8];
    int32_t x;
    cv_begin(240, VT_HEAD_H, VT_C_HEAD);
    memcpy(s, "INS", 3);
    app_hex(app.ins, 2, s + 3);
    cv_text(8, 1, &AF_S, s, VT_C_INS);
    cv_text_mono(VT_NAME_X, 1, &AF_S, project_ins_name(app.ins), VT_C_INS, VT_C_HEAD, VT_PITCH);
    cv_rect(VT_NAME_X + app.name_pos * VT_PITCH, 14, VT_PITCH - 1, 1, VT_C_INS);
    cv_text_r(232, 1, &AF_S, "NES", VT_C_HEAD_DIM, VT_C_HEAD);
    s[0] = 'T';
    s[1] = (char)('1' + app.track);
    s[2] = 0;
    x = cv_text(8, 15, &AF_S, s, VT_C_HEAD_DIM);
    cv_text(x + 8, 15, &AF_S, player_track_name(app.track), VT_C_HEAD_DIM);
    cv_blit(0, 0);
}

static void app_editor_draw(void)     /* the editor: the header if it changed, then each panel that changed */
{
    uint32_t s;
    if (app.head != app_editor_head_key()) {
        app_editor_head();
        app.head = app_editor_head_key();
    }
    for (s = 0; s < PROJECT_SEQS; s++) {
        vt_panel_t pl;
        memset(&pl, 0, sizeof pl);
        pl.q = *project_seq(app.ins, s);
        pl.ins = app.ins;
        pl.sel = s == app.seq;
        pl.step = pl.sel ? app.step : 0u;
        pl.mode = (uint8_t)app_noise();
        if (memcmp(&pl, &app.panel[s], sizeof pl)) {
            app_draw_panel(s, &pl);
            app.panel[s] = pl;
        }
    }
}

static uint32_t app_order_head_key(void)   /* what the order list's header shows, as one word (never 0) */
{
    return 1u | (uint32_t)app.track << 1 | (uint32_t)player_loop() << 3 | project_bpm() << 4;
}

static void app_order_head(void)     /* ORDER, the tempo, SONG or LOOP; the tracks' columns T1 .. T4 */
{
    char s[12];
    uint32_t t;
    int32_t x;
    cv_begin(240, VT_HEAD_H, VT_C_HEAD);
    x = cv_text(8, 1, &AF_S, "ORDER", VT_C_TEXT);
    app_bpm_text(s);
    cv_text(x + 12, 1, &AF_S, s, VT_C_HEAD_DIM);
    cv_text_r(232, 1, &AF_S, player_loop() ? "LOOP" : "SONG", VT_C_TEXT, VT_C_HEAD);
    for (t = 0; t < PROJECT_TRACKS; t++) {
        s[0] = 'T';
        s[1] = (char)('1' + t);
        s[2] = 0;
        cv_text(VT_O_X + (int32_t)t * VT_O_W, 15, &AF_S, s, t == app.track ? VT_C_TEXT : VT_C_HEAD_DIM);
    }
    cv_blit(0, 0);
}

static void app_order_line(uint32_t i, const vt_oline_t *l)   /* order list line i: the entry, its patterns */
{
    static const uint16_t BG[4] = {VT_C_BG, VT_C_CURSOR, VT_C_PLAYING, VT_C_CURSOR_PLAYING};
    uint16_t bg = BG[l->mark & (VT_LINE_CURSOR | VT_LINE_PLAYING)];
    uint32_t t;
    char s[3];
    cv_begin(240, VT_ROW_H, bg);
    if (!(l->mark & VT_LINE_PAST)) {
        app_hex(l->entry, 2, s);
        cv_text_mono(VT_X_NUM, 0, &AF_S, s, VT_C_NUM_BEAT, bg, VT_PITCH);
        for (t = 0; t < PROJECT_TRACKS; t++) {
            app_hex(l->pat[t], 2, s);
            cv_text_mono(VT_O_X + (int32_t)t * VT_O_W, 0, &AF_S, s, t == l->track ? VT_C_TEXT : VT_C_OTHER, bg,
                         VT_PITCH);
        }
    }
    cv_blit(0, VT_HEAD_H + i * VT_ROW_H);
}

static void app_order_draw(void)     /* the order list: the header if it changed, then each line that changed */
{
    uint32_t i, t, playing = player_playing(), pe = player_entry();
    if (app.otop + VT_SHOWN > project_orders())   /* (entries deleted: the screen keeps as many shown as can be) */
        app.otop = (uint8_t)(project_orders() > VT_SHOWN ? project_orders() - VT_SHOWN : 0u);
    if (project_entry() < app.otop)
        app.otop = (uint8_t)project_entry();
    if (app.head != app_order_head_key()) {
        app_order_head();
        app.head = app_order_head_key();
    }
    for (i = 0; i < VT_SHOWN; i++) {
        vt_oline_t l;
        uint32_t e = app.otop + i;
        memset(&l, 0, sizeof l);
        l.entry = (uint8_t)e;
        l.track = app.track;
        if (e >= project_orders()) {
            l.mark = VT_LINE_PAST;
        } else {
            for (t = 0; t < PROJECT_TRACKS; t++)
                l.pat[t] = (uint8_t)project_order(e, t);
            l.mark = (uint8_t)((e == project_entry() ? VT_LINE_CURSOR : 0u) |
                               (playing && e == pe ? VT_LINE_PLAYING : 0u));
        }
        if (memcmp(&l, &app.oshown[i], sizeof l)) {
            app_order_line(i, &l);
            app.oshown[i] = l;
        }
    }
}

static uint32_t app_project_key(void)   /* what the project view shows, as one word (never 0) */
{
    uint32_t h = 1u + app.slot + 4u * app.pname_pos + 64u * app.ask + 256u * store_percent(), i, k;
    const char *n = project_name();
    for (i = 0; n[i]; i++)
        h = h * 31u + (uint8_t)n[i];
    for (i = 0; app.msg[i]; i++)
        h = h * 31u + (uint8_t)app.msg[i];
    for (k = 0; k < STORE_SLOTS; k++)
        for (i = 0, n = store_name(k), h = h * 31u + 7u; n[i]; i++)
            h = h * 31u + (uint8_t)n[i];
    h = h * 31u + (uint32_t)store_autosave_full();
    return h | 1u;
}

/* the project view: the header (PROJECT, the name with its cursor; the memory's fullness, its bar), the four slots
 * (the selected one with the cursor's bar), the message, the help */
static void app_project_draw(void)
{
    char s[24];
    uint32_t i, pct = store_percent();
    int32_t x;
    if (app.head == app_project_key())
        return;
    app.head = app_project_key();
    cv_begin(240, VT_HEAD_H, VT_C_HEAD);
    x = cv_text(8, 1, &AF_S, "PROJECT", VT_C_TEXT) + 10;
    cv_text_mono(x, 1, &AF_S, project_name(), VT_C_INS, VT_C_HEAD, VT_PITCH);
    cv_rect(x + app.pname_pos * VT_PITCH, 14, VT_PITCH - 1, 1, VT_C_INS);
    memcpy(s, "MEMORY ", 7);
    i = 7;
    if (pct >= 100u)
        s[i++] = '1';
    if (pct >= 10u)
        s[i++] = (char)('0' + pct / 10u % 10u);
    s[i++] = (char)('0' + pct % 10u);
    s[i++] = '%';
    s[i] = 0;
    cv_text_r(232, 1, &AF_S, s, VT_C_HEAD_DIM, VT_C_HEAD);
    cv_rect(8, 20, 224, 5, VT_C_GRAPH);
    cv_rect(8, 20, (int32_t)(224u * pct / 100u), 5, pct >= 90u ? VT_C_FULL : VT_C_BAR);
    cv_blit(0, 0);
    for (i = 0; i < STORE_SLOTS; i++) {
        const char *n = store_used(i) ? store_name(i) : "EMPTY";
        uint32_t k = 0;
        s[k++] = (char)('1' + i);
        s[k++] = ' ';
        while (*n && k + 1u < sizeof s)
            s[k++] = *n++;
        s[k] = 0;
        cv_begin(240, VT_ROW_H, i == app.slot ? VT_C_CURSOR : VT_C_BG);
        cv_text(8, 0, &AF_S, s, store_used(i) ? VT_C_TEXT : VT_C_EMPTY);
        cv_blit(0, VT_P_SLOT_Y + i * VT_ROW_H);
    }
    cv_begin(240, VT_ROW_H, VT_C_BG);
    if (app.msg[0])
        cv_text(8, 0, &AF_S, app.msg, VT_C_EFFECT);
    else if (store_autosave_full())
        cv_text(8, 0, &AF_S, "AUTOSAVE: MEMORY FULL", VT_C_FULL);
    cv_blit(0, VT_P_MSG_Y);
    cv_begin(240, 2u * VT_ROW_H, VT_C_BG);
    cv_text(8, 0, &AF_S, "> SAVE   < LOAD   SHIFT < NEW", VT_C_HEAD_DIM);
    cv_text(8, (int32_t)VT_ROW_H, &AF_S, "K2 SLOT   K4 NAME", VT_C_HEAD_DIM);
    cv_blit(0, VT_P_HELP_Y);
}

/* the pads: the rows of the page, the instruments, or the order list's entries; SEQ PLAY, SEQ REC, NOTE REPEAT */
static void app_leds(void)
{
    uint32_t p, page = app_page(), playing = app_entry_playing(), prow = player_row(), opage = app_opage();
    for (p = 0; p < SMK_NPAD; p++) {
        uint32_t row = page + p, e = opage + p;
        const uint8_t *c = app.view == VT_VIEW_PROJECT
                               ? (p >= STORE_SLOTS   ? VT_PAD_PAST
                                  : p == app.slot    ? VT_PAD_CURSOR
                                  : store_used(p)    ? VT_PAD_NOTE
                                                     : VT_PAD_EMPTY)
                           : app.view == VT_VIEW_ORDER
                               ? (e >= project_orders()                          ? VT_PAD_PAST
                                  : player_playing() && e == player_entry() ? VT_PAD_PLAYING
                                  : e == project_entry()                         ? VT_PAD_CURSOR
                                                                                 : VT_PAD_NOTE)
                           : app.pad_ins || app.view == VT_VIEW_EDITOR
                               ? (p == app.ins ? VT_PAD_INS_SEL : app_ins_offered(p) ? VT_PAD_INS : VT_PAD_PAST)
                           : row >= project_rows(app.track) ? VT_PAD_PAST
                           : playing && row == prow   ? VT_PAD_PLAYING
                           : row == app.cursor      ? VT_PAD_CURSOR
                           : project_field(app.track, row, PROJECT_F_NOTE) ? VT_PAD_NOTE
                                                    : VT_PAD_EMPTY;
        padled_set(p, c[0], c[1], c[2]);
    }
    fm1_led_key(SMK_BTN_SEQ_PLAY, playing);
    fm1_led_key(SMK_BTN_SEQ_REC, app.rec);
    fm1_led_key(SMK_BTN_NOTE_REPEAT, app.pad_ins);
    fm1_led_key(SMK_BTN_PATCH, app.view == VT_VIEW_EDITOR);
    fm1_led_key(SMK_BTN_ARP, app.view == VT_VIEW_ORDER);
    fm1_led_key(SMK_BTN_GLOBE, app.view == VT_VIEW_PROJECT);
}

static void app_frame(void)
{
    app_input();
    if (app.view == VT_VIEW_EDITOR)
        app_editor_draw();
    else if (app.view == VT_VIEW_ORDER)
        app_order_draw();
    else if (app.view == VT_VIEW_PROJECT)
        app_project_draw();
    else
        app_draw();
    app_leds();
    store_service(fm1_ms, player_playing() || fm1_in.keys != 0);   /* (an erase would cut the notes off) */
}

static void app_idle(void) {}

static void app_audio(int32_t *out, uint32_t n)
{
    player_audio(out, n);
}
