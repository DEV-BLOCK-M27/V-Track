/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The tracker display and field entry on the simulated SMK-37 (seam A, tests/vt_sim.h). The pattern view shows 13
 * rows of the selected track with all four columns (note, instrument, volume, effect) and the other three tracks
 * as note columns; the cursor row and the playing row are marked. K2 moves the cursor, K5 to K8 edit the cursor
 * row's note, instrument, volume and effect; KNOB BANK (shift) + K2 sets the pattern's length (16 to 64 rows);
 * < and > page through it a page of 16 rows (display and pads), shift + < and > select the track's pattern (32).
 *   tests/vt_view_test [OUT_DIR]   (OUT_DIR: the screen with all columns filled as view.ppm) */
#include "vt_sim.h"

#define K2 1u                                /* encoders */
#define K3 2u
#define K5 4u
#define K6 5u
#define K7 6u
#define K8 7u
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_E4 16u
#define KEY_G4 19u

static int fails;
static void check(int ok, const char *what)
{
    printf("%-96s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void press(uint32_t key)               /* a key pressed and released */
{
    sim_key(key, 100);
    sim_run_ms(40);
    sim_key(key, 0);
    sim_run_ms(40);
}

static void button(uint32_t b)                /* a button pressed and released */
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

static void turn(uint32_t e, int32_t steps)   /* an encoder turned */
{
    sim_encoder(e, steps);
    sim_run_ms(40);
}

static void shift_on(int on)                  /* KNOB BANK held / let go */
{
    sim_button(SMK_BTN_KNOB_BANK, on);
    sim_run_ms(30);
}

/* the screen line of row number s, into what, checked against want */
static void line_is(const char *s, const char *want, const char *what)
{
    char got[256], msg[512];
    sim_screen_line(s, got, sizeof got);
    snprintf(msg, sizeof msg, "%s: \"%s\"", what, got);
    check(!strcmp(got, want), msg);
}

static uint16_t line_bg(const char *num)      /* the colour of row num's line (left of its number), 0 if not shown */
{
    int32_t b[4];
    return sim_screen_box(num, b) ? sim_frame()[b[1] * 240 + 1] : 0u;
}

static void tidy(const char *what)
{
    char why[160], msg[320];
    int ok = sim_screen_tidy(why, sizeof why);
    snprintf(msg, sizeof msg, "%s: no text cut or overlapping (%s)", what, why);
    check(ok, msg);
}

int main(int argc, char **argv)
{
    uint32_t n;
    char what[160];

    sim_power_on();
    sim_run_ms(100);
    sim_button(SMK_BTN_KNOB_BANK, 1);        /* shift + SEQ REC: velocity recording off (vt_record_test's) */
    sim_run_ms(30);
    sim_button(SMK_BTN_SEQ_REC, 1);
    sim_run_ms(30);
    sim_button(SMK_BTN_SEQ_REC, 0);
    sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);

    /* the view: the selected track in full, the other three as note columns */
    line_is("00", "00 ... .. . ... ... ... ...", "power-on: row 00, empty: note, instrument, volume, effect; 3 notes");
    check(sim_screen_text("T1") && sim_screen_text("NES \xB7 PULSE 1") && sim_screen_text("P00"),
          "power-on: the header names track 1, its channel and its pattern P00");
    check(sim_screen_text("0C") && !sim_screen_text("0D"), "power-on: 13 rows, 00 to 0C");
    button(SMK_BTN_SEQ_REC);
    press(KEY_C4);                            /* track 1 row 00: C-4 */
    turn(K3, 1);
    turn(K2, -1);
    press(KEY_E4);                            /* track 2 row 00: E-4 */
    turn(K3, 1);
    turn(K2, -1);
    press(KEY_G4);                            /* track 3 row 00: G-4 */
    turn(K3, -1);
    line_is("00", "00 E-4 .. . ... C-4 G-4 ...", "track 2: its row 00 in full, then tracks 1, 3, 4's notes");
    check(sim_screen_text("T2") && sim_screen_text("NES \xB7 PULSE 2"), "track 2: the header names it");
    tidy("track 2's view");

    /* K5 to K8: the cursor row's note, instrument, volume, effect */
    button(SMK_BTN_SEQ_REC);                  /* (REC off: the knobs edit all the same) */
    turn(K3, -1);
    turn(K2, -1);
    line_is("00", "00 C-4 .. . ... E-4 G-4 ...", "track 1, the cursor on row 00");
    turn(K5, 2);
    line_is("00", "00 D-4 .. . ... E-4 G-4 ...", "K5 +2: the note up two semitones, D-4");
    turn(K6, 1);
    line_is("00", "00 D-4 00 . ... E-4 G-4 ...", "K6 on an empty instrument: 00");
    turn(K6, 3);
    turn(K7, 1);
    line_is("00", "00 D-4 03 F ... E-4 G-4 ...", "K6 +3: instrument 03; K7 on an empty volume: F");
    turn(K7, -3);
    shift_on(1);
    turn(K8, 4);
    shift_on(0);
    line_is("00", "00 D-4 03 C 300 E-4 G-4 ...", "K7 -3: volume C; shift + K8 +4: effect letter 3, parameter 00");
    turn(K8, 0x12);
    shift_on(1);
    turn(K8, 2);
    shift_on(0);
    line_is("00", "00 D-4 03 C A12 E-4 G-4 ...", "K8 +0x12: parameter 12; shift + K8 +2: letter A (0 1 2 3 4 A)");
    tidy("all four columns filled");
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/view.ppm", argv[1]);
        sim_save_ppm(path);
    }
    turn(K2, 1);
    turn(K5, -1);
    line_is("01", "01 D-4 .. . ... ... ... ...", "row 01, K5 on an empty note: the last note written, D-4");
    turn(K3, 1);
    turn(K3, -1);
    turn(K2, -1);
    line_is("00", "00 D-4 03 C A12 E-4 G-4 ...", "track 2 and back, row 00 drawn from the pattern: as edited");
    turn(K5, 200);
    turn(K6, 40);
    turn(K7, 40);
    turn(K8, 300);
    line_is("00", "00 B-7 0F F AFF E-4 G-4 ...", "K5 to K8 turned far up: B-7, instrument 0F, volume F, AFF");
    turn(K5, -200);
    turn(K6, -40);
    turn(K7, -40);
    turn(K8, -300);
    line_is("00", "00 C-0 00 0 A00 E-4 G-4 ...", "K5 to K8 turned far down: C-0, 00, 0, A00");
    turn(K2, 1);
    check(sim_pad_led(0) != sim_pad_led(2), "row 00 holds C-0 (the cursor on row 01): pad 1 not pad 3's empty colour");
    turn(K2, -1);
    shift_on(1);
    turn(K5, 1);
    turn(K6, -1);
    turn(K7, 1);
    turn(K8, -20);
    shift_on(0);
    line_is("00", "00 ... .. . ... E-4 G-4 ...", "shift + K5 K6 K7, shift + K8 down to no letter: the row emptied");
    turn(K2, 1);
    check(sim_pad_led(0) == sim_pad_led(2), "row 00 empty: pad 1 the empty colour, as pad 3");

    /* the length: shift + K2, 16 to 64 rows; < and > page through it, 16 rows a page, the screen and the pads */
    check(sim_screen_text("16 ROWS"), "a new pattern: 16 rows (the header)");
    shift_on(1);
    turn(K2, 100);
    check(sim_screen_text("64 ROWS"), "shift + K2 +100: 64 rows, no more");
    turn(K2, -100);
    check(sim_screen_text("16 ROWS"), "shift + K2 -100: 16 rows, no fewer");
    turn(K2, 32);
    shift_on(0);
    check(sim_screen_text("48 ROWS"), "shift + K2 +32: 48 rows");
    check(sim_screen_text("0C") && sim_screen_text("01") && !sim_screen_text("10"), "page 1: rows 00 to 0C");
    button(SMK_BTN_RIGHT);
    check(sim_screen_text("10") && sim_screen_text("1C") && !sim_screen_text("0F") && !sim_screen_text("1D"),
          "> : page 2, the screen shows rows 10 to 1C");
    turn(K5, 25);                             /* row 11: C-0 (the last note written) + 24: C-2 */
    turn(K2, 1);
    line_is("11", "11 C-2 .. . ... ... ... ...", "> : the cursor a page down, on row 11; K5 writes C-2 there");
    check(sim_pad_led(1) != sim_pad_led(0) && sim_pad_led(2) != sim_pad_led(0) && sim_pad_led(3) == sim_pad_led(0),
          "page 2 on the pads: pad 2 row 11's note, pad 3 the cursor (row 12), pads 1 and 4 empty");
    button(SMK_BTN_RIGHT);
    check(sim_screen_text("20") && sim_screen_text("2C") && !sim_screen_text("1F"), "> : page 3, rows 20 to 2C");
    check(sim_pad_led(1) == sim_pad_led(0) && sim_pad_led(2) != sim_pad_led(0),
          "page 3 on the pads: the cursor on pad 3");
    button(SMK_BTN_RIGHT);
    check(sim_screen_text("20") && !sim_screen_text("30"), "> on the last page: it stays");
    button(SMK_BTN_LEFT);
    button(SMK_BTN_LEFT);
    check(sim_screen_text("00") && sim_screen_text("0C") && !sim_screen_text("10"), "< twice: page 1, rows 00 to 0C");
    shift_on(1);
    turn(K2, -8);
    shift_on(0);
    turn(K2, 10);
    button(SMK_BTN_RIGHT);
    button(SMK_BTN_RIGHT);
    check(sim_screen_text("40 ROWS") && sim_screen_text("27") && !sim_screen_text("28"),
          "40 rows, page 3: rows 20 to 27, nothing past them");
    line_is("27", "27 ... .. . ... ... ... ...", "the cursor from row 0C two pages down: on the last row, 27");
    {
        uint32_t p, dark = 0;
        for (p = 0; p < 16u; p++)
            dark |= (uint32_t)(sim_pad_led(p) == 0) << p;
        check(dark == 0xFF00u, "40 rows, page 3 on the pads: pads 1 to 8 rows 20 to 27, pads 9 to 16 dark");
    }
    button(SMK_BTN_LEFT);
    button(SMK_BTN_LEFT);
    tidy("page 1 again");
    check(!sim_screen_text("0F"), "page 1: row 0F not on the screen");
    sim_pad(15, 100);
    sim_run_ms(40);
    sim_pad(15, 0);
    sim_run_ms(40);
    check(sim_screen_text("0F") && line_bg("0F") != line_bg("0E"),
          "pad 16 tapped: the screen scrolls to row 0F, the cursor bar on it");
    button(SMK_BTN_RIGHT);
    check(sim_screen_text("1F") && line_bg("1F") != line_bg("1E"), "> from row 0F: row 1F shown, the cursor bar on it");
    button(SMK_BTN_RIGHT);                    /* (40 rows: page 3's pads 9 to 16 are past the end) */
    turn(K2, -5);
    sim_pad(15, 100);
    sim_run_ms(40);
    sim_pad(15, 0);
    sim_run_ms(40);
    check(line_bg("22") != line_bg("23") && line_bg("27") == line_bg("23"),
          "page 3 of 40 rows: pad 16 (past the end) tapped: nothing, the cursor stays on row 22");
    button(SMK_BTN_LEFT);
    button(SMK_BTN_LEFT);
    turn(K2, -15);                            /* (the screen back to row 00, the cursor on 07) */
    turn(K2, 7);

    /* the patterns: 32 a track, shift + < and > select one; each keeps its rows (and its length) */
    turn(K5, 1);
    line_is("07", "07 C-2 .. . ... ... ... ...", "P00, row 07: C-2");
    shift_on(1);
    for (n = 0; n < 7u; n++)
        button(SMK_BTN_RIGHT);
    shift_on(0);
    check(sim_screen_text("P07") && sim_screen_text("16 ROWS"),
          "shift + > 7 times: track 1 on P07, a new pattern, 16 rows");
    line_is("07", "07 ... .. . ... ... ... ...", "P07, row 07: empty");
    turn(K5, 3);
    turn(K6, 1);
    line_is("07", "07 D-2 00 . ... ... ... ...", "P07, row 07: D-2 00");
    shift_on(1);
    for (n = 0; n < 7u; n++)
        button(SMK_BTN_LEFT);
    shift_on(0);
    check(sim_screen_text("P00") && sim_screen_text("40 ROWS"), "shift + < 7 times: back on P00, 40 rows");
    line_is("07", "07 C-2 .. . ... ... ... ...", "P00, row 07: C-2 kept");
    shift_on(1);
    for (n = 0; n < 7u; n++)
        button(SMK_BTN_RIGHT);
    shift_on(0);
    line_is("07", "07 D-2 00 . ... ... ... ...", "P07 again, row 07: D-2 00 kept");
    shift_on(1);
    for (n = 0; n < 30u; n++)
        button(SMK_BTN_RIGHT);
    shift_on(0);
    check(sim_screen_text("P1F"), "shift + > 30 times more: P1F, the 32nd, no further");
    shift_on(1);
    for (n = 0; n < 24u; n++)
        button(SMK_BTN_LEFT);
    shift_on(0);
    check(sim_screen_text("P07"), "shift + < 24 times: P07");
    turn(K3, 1);
    check(sim_screen_text("P00"), "track 2: still on its P00");
    line_is("00", "00 E-4 .. . ... ... G-4 ...", "track 2, row 00: its E-4; track 1's column shows its P07 (empty)");
    turn(K3, -1);

    /* the playing row: marked on the screen while the sequencer plays, a row a sixteenth (125 ms at 120 BPM), the
     * cursor row (07) too */
    {
        uint16_t plain = line_bg("01"), cursor = line_bg("07");
        uint32_t ms, r, row = 99, rows = 0, ok = 1, t0 = 0, lo = 1000, hi = 0, on_cursor = 0;
        check(plain != cursor, "stopped: the cursor row's bar, other rows plain");
        button(SMK_BTN_SEQ_PLAY);
        for (ms = 0; ms < 1700u; ms++) {
            uint32_t at = 99, k = 0;
            char num[3];
            sim_run_ms(1);
            for (r = 0; r < 13u; r++) {
                uint16_t c;
                snprintf(num, sizeof num, "%02X", r);
                c = line_bg(num);
                if (c != plain && c != cursor) {
                    at = r;
                    k++;
                }
            }
            if (k > 1u)
                ok = 0;
            if (k == 1u && at != row) {
                if (row != 99u && at != row + 1u)
                    ok = 0;
                if (at == 7u)
                    on_cursor = 1;
                if (row != 99u && rows > 0u) {
                    lo = ms - t0 < lo ? ms - t0 : lo;
                    hi = ms - t0 > hi ? ms - t0 : hi;
                }
                if (row != 99u)
                    rows++;
                row = at;
                t0 = ms;
            }
            if (k == 0u)
                row = 99;                     /* (off the screen: rows 0D to 0F) */
        }
        button(SMK_BTN_SEQ_PLAY);
        snprintf(what, sizeof what, "playing: one row marked, walking 00 01 02 .. (%u rows on, %u to %u ms a row)",
                 rows, lo, hi);
        check(ok && rows >= 11u && lo >= 125u - 20u && hi <= 125u + 20u, what);
        check(on_cursor, "playing: the cursor row marked too, when it plays");
        sim_run_ms(50);
        check(line_bg("03") == plain && line_bg("07") == cursor, "stopped: no row marked, the cursor bar as before");
    }

    printf(fails ? "V-TRACK VIEW: %d FAILED\n" : "V-TRACK VIEW: ok\n", fails);
    return fails != 0;
}
