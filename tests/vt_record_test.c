/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Editing and recording on the simulated SMK-37 (seam A, tests/vt_sim.h). In the pattern view PARA clears the
 * cursor row, CHORD inserts a row there (the rows below move down), KNOB BANK (shift) + CHORD deletes it (the rows
 * below move up). SCALE copies the track's pattern, shift + SCALE pastes it into the track's pattern (another
 * number). Shift + PARA undoes the last edit. With REC on a key writes its velocity into the volume column (1 to
 * F); shift + SEQ REC switches that off and on. With REC and PLAY on a key lands in the row playing.
 *   tests/vt_record_test [OUT_DIR]   (OUT_DIR: the screen after the real-time take as record.ppm) */
#include "vt_sim.h"

#define K2 1u                                /* encoders: K2 cursor, K5 note, K7 volume */
#define K4 3u
#define K5 4u
#define K7 6u
#define KEY_C4 12u                           /* keybed key 0 = C3 */
#define KEY_CS4 13u                          /* (a black key) */
#define KEY_E4 16u
#define KEY_G4 19u
#define KEY_C5 24u

static int fails;
static void check(int ok, const char *what)
{
    printf("%-96s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void press_vel(uint32_t key, uint32_t vel)   /* a key pressed (velocity vel) and released */
{
    sim_key(key, vel);
    sim_run_ms(40);
    sim_key(key, 0);
    sim_run_ms(40);
}

static void press(uint32_t key) { press_vel(key, 127); }

static void button(uint32_t b)               /* a button pressed and released */
{
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
}

static void shift_on(int on)                 /* KNOB BANK held / let go */
{
    sim_button(SMK_BTN_KNOB_BANK, on);
    sim_run_ms(30);
}

static void shifted(uint32_t b)              /* a button pressed and released with shift held */
{
    shift_on(1);
    button(b);
    shift_on(0);
}

static void turn(uint32_t e, int32_t steps)  /* an encoder turned */
{
    sim_encoder(e, steps);
    sim_run_ms(40);
}

static void cursor_to(int32_t row)           /* the cursor up to the top, then down to row */
{
    turn(K2, -64);
    turn(K2, row);
}

/* the screen line of row number s, checked against want */
static void line_is(const char *s, const char *want, const char *what)
{
    char got[256], msg[512];
    sim_screen_line(s, got, sizeof got);
    snprintf(msg, sizeof msg, "%s: \"%s\"", what, got);
    check(!strcmp(got, want), msg);
}

static void enter_ceg(void)                  /* rows 00 01 02: C-4 E-4 G-4 at full velocity, the cursor to 00 */
{
    cursor_to(0);
    press(KEY_C4);
    press(KEY_E4);
    press(KEY_G4);
    cursor_to(0);
}

#define EMPTY ".. . ... ... ... ..."     /* the rest of a line: instrument, volume, effect; the other 3 notes */
#define C4 "C-4 .. F ... ... ... ..."
#define E4 "E-4 .. F ... ... ... ..."
#define G4 "G-4 .. F ... ... ... ..."

int main(int argc, char **argv)
{
    sim_power_on();
    sim_run_ms(100);
    button(SMK_BTN_SEQ_REC);

    /* clear, insert, delete */
    enter_ceg();
    line_is("01", "01 " E4, "step entry, full velocity: E-4 at volume F");
    cursor_to(1);
    button(SMK_BTN_PARA);
    line_is("00", "00 " C4, "PARA on row 01: row 00 kept");
    line_is("01", "01 ... " EMPTY, "PARA on row 01: row 01 empty");
    line_is("02", "02 " G4, "PARA on row 01: row 02 kept");

    enter_ceg();
    cursor_to(1);
    button(SMK_BTN_CHORD);
    line_is("00", "00 " C4, "CHORD on row 01: row 00 kept");
    line_is("01", "01 ... " EMPTY, "CHORD on row 01: an empty row inserted");
    line_is("02", "02 " E4, "CHORD on row 01: E-4 moved down to 02");
    line_is("03", "03 " G4, "CHORD on row 01: G-4 moved down to 03");

    cursor_to(0);
    shifted(SMK_BTN_CHORD);
    line_is("00", "00 ... " EMPTY, "shift + CHORD on row 00: the empty row moved up to 00");
    line_is("01", "01 " E4, "shift + CHORD on row 00: E-4 moved up to 01");
    line_is("02", "02 " G4, "shift + CHORD on row 00: G-4 moved up to 02");
    line_is("03", "03 ... " EMPTY, "shift + CHORD on row 00: row 03 empty");

    /* copy and paste: P00 into P05, the same rows and length */
    turn(K2, 0);
    shift_on(1);
    turn(K2, 8);                             /* P00: 24 rows */
    shift_on(0);
    cursor_to(20);
    press(KEY_C5);                           /* row 14 */
    cursor_to(0);
    button(SMK_BTN_SCALE);
    shift_on(1);
    for (int i = 0; i < 5; i++)
        button(SMK_BTN_RIGHT);               /* P05 */
    shift_on(0);
    check(sim_screen_text("P05") && sim_screen_text("16 ROWS"), "shift + > five times: P05, empty, 16 rows");
    line_is("01", "01 ... " EMPTY, "P05 before the paste: row 01 empty");
    shifted(SMK_BTN_SCALE);
    check(sim_screen_text("P05") && sim_screen_text("24 ROWS"), "shift + SCALE: P05 has P00's length, 24 rows");
    line_is("00", "00 ... " EMPTY, "shift + SCALE: P05 row 00 as P00's");
    line_is("01", "01 " E4, "shift + SCALE: P05 row 01 as P00's");
    line_is("02", "02 " G4, "shift + SCALE: P05 row 02 as P00's");
    cursor_to(20);
    line_is("14", "14 C-5 .. F ... ... ... ...", "shift + SCALE: P05 row 14 as P00's");
    shift_on(1);
    for (int i = 0; i < 5; i++)
        button(SMK_BTN_LEFT);                /* back to P00 */
    shift_on(0);
    check(sim_screen_text("P00") && sim_screen_text("24 ROWS"), "P00 after the paste: still 24 rows");
    cursor_to(0);
    line_is("01", "01 " E4, "P00 after the paste: row 01 unchanged");

    /* undo: each edit undone restores the rows before it */
    button(SMK_BTN_PARA);                    /* clear row 00 (empty already): nothing to see, undone to nothing */
    cursor_to(1);
    button(SMK_BTN_PARA);                    /* clear row 01 */
    line_is("01", "01 ... " EMPTY, "PARA: row 01 cleared");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 " E4, "shift + PARA after a clear: E-4 back on row 01");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 " E4, "shift + PARA again: one level only, nothing changes");

    button(SMK_BTN_CHORD);                   /* insert at row 01 */
    shifted(SMK_BTN_PARA);
    line_is("01", "01 " E4, "undo after an insert: row 01 E-4 again");
    line_is("02", "02 " G4, "undo after an insert: row 02 G-4 again");

    shifted(SMK_BTN_CHORD);                  /* delete row 01 */
    line_is("01", "01 " G4, "delete row 01: G-4 moved up");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 " E4, "undo after a delete: row 01 E-4 again");
    line_is("02", "02 " G4, "undo after a delete: row 02 G-4 again");

    turn(K5, 2);                             /* E-4 -> G-4 over two turns: one edit */
    turn(K5, 1);
    turn(K7, -4);
    line_is("01", "01 G-4 .. B ... ... ... ...", "K5 +2 +1, K7 -4: G-4, volume B");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 G-4 .. F ... ... ... ...", "undo after K7: the volume back, the note kept");
    turn(K5, 2);
    turn(K5, 1);
    line_is("01", "01 A#4 .. F ... ... ... ...", "K5 +2 +1 again: A#4");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 G-4 .. F ... ... ... ...", "undo after K5 turned twice: both turns undone");

    turn(K5, 1);                             /* G#4, then K7 at its end (volume F already): no edit */
    turn(K7, 5);
    shifted(SMK_BTN_PARA);
    line_is("01", "01 G-4 .. F ... ... ... ...", "K5 +1, K7 at its end, undo: the note edit undone, G-4");
    turn(K5, 1);                             /* G#4, then away and back: two edits */
    turn(K2, 1);
    turn(K2, -1);
    turn(K5, 1);
    shifted(SMK_BTN_PARA);
    line_is("01", "01 G#4 .. F ... ... ... ...", "K5 on row 01, away and back, K5 again: undo takes the second only");
    turn(K5, -1);
    shift_on(1);
    turn(K2, 4);                             /* the length 24 -> 28 */
    shift_on(0);
    check(sim_screen_text("28 ROWS"), "shift + K2 +4: 28 rows");
    shifted(SMK_BTN_PARA);
    check(sim_screen_text("24 ROWS"), "undo after a length change: 24 rows again");
    cursor_to(1);

    press(KEY_CS4);                          /* step entry into row 01: C#4, the cursor to 02 */
    line_is("01", "01 C#4 .. F ... ... ... ...", "a key with REC: C#4 into row 01");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 G-4 .. F ... ... ... ...", "undo after step entry: G-4 again");

    shifted(SMK_BTN_RIGHT);                  /* P01: paste P00 over it */
    check(sim_screen_text("P01") && sim_screen_text("16 ROWS"), "P01: empty, 16 rows");
    shifted(SMK_BTN_SCALE);
    check(sim_screen_text("24 ROWS"), "shift + SCALE into P01: 24 rows");
    shifted(SMK_BTN_PARA);
    check(sim_screen_text("P01") && sim_screen_text("16 ROWS"), "undo after a paste: P01 16 rows again");
    cursor_to(0);
    line_is("01", "01 ... " EMPTY, "undo after a paste: P01 row 01 empty again");

    /* shift + key: cut (a white key) and release (a black key) */
    cursor_to(0);
    shift_on(1);
    press(KEY_C4);
    press(KEY_CS4);
    shift_on(0);
    line_is("00", "00 --- .. . ... ... ... ...", "shift + white key: a cut, no volume");
    line_is("01", "01 === .. . ... ... ... ...", "shift + black key: a release");
    shifted(SMK_BTN_PARA);
    line_is("01", "01 ... " EMPTY, "undo after a release: row 01 empty again");
    line_is("00", "00 --- .. . ... ... ... ...", "undo after a release: the cut before it kept");

    /* velocity into the volume column; shift + SEQ REC switches it off */
    cursor_to(4);
    press_vel(KEY_C4, 20);
    press_vel(KEY_C4, 100);
    line_is("04", "04 C-4 .. 2 ... ... ... ...", "a soft key (velocity 20): volume 2");
    line_is("05", "05 C-4 .. C ... ... ... ...", "a hard key (velocity 100): volume C");
    press_vel(KEY_C4, 1);
    line_is("06", "06 C-4 .. 1 ... ... ... ...", "the softest key (velocity 1): volume 1, never silent");
    check(sim_screen_text("VEL"), "the header shows VEL while velocity recording is on");
    shifted(SMK_BTN_SEQ_REC);
    check(sim_screen_text("REC") && !sim_screen_text("VEL"), "shift + SEQ REC: velocity off, REC still on");
    press_vel(KEY_C4, 20);
    press_vel(KEY_C4, 120);
    line_is("07", "07 C-4 .. . ... ... ... ...", "velocity off, a soft key: the volume column empty");
    line_is("08", "08 C-4 .. . ... ... ... ...", "velocity off, a hard key: the volume column empty");
    shifted(SMK_BTN_SEQ_REC);
    check(sim_screen_text("VEL"), "shift + SEQ REC again: velocity on");

    /* real time: REC and PLAY, keys land in the rows playing (P01, 16 rows, at 300 BPM: a row 50 ms) */
    for (int r = 0; r < 16; r++) {
        cursor_to(r);
        button(SMK_BTN_PARA);
    }
    cursor_to(0);
    turn(K4, 180);                           /* 300 BPM */
    sim_button(SMK_BTN_SEQ_PLAY, 1);         /* playing starts within a frame (15 ms); a key is taken within one */
    sim_run_ms(15);
    sim_button(SMK_BTN_SEQ_PLAY, 0);
    sim_run_ms(2 * 50 - 5);                  /* row 02, its 0 .. 30 ms in */
    sim_key(KEY_E4, 127);
    sim_run_ms(20);
    sim_key(KEY_E4, 0);
    sim_run_ms(3 * 50 - 20);                 /* row 05 */
    sim_key(KEY_G4, 127);
    sim_run_ms(20);
    sim_key(KEY_G4, 0);
    sim_run_ms(6 * 50 - 20);                 /* row 0B: a cut */
    shift_on(1);
    sim_key(KEY_C4, 127);
    sim_run_ms(10);
    sim_key(KEY_C4, 0);
    shift_on(0);
    sim_run_ms(20);
    button(SMK_BTN_SEQ_PLAY);
    line_is("00", "00 ... " EMPTY, "real time: row 00 empty");
    line_is("02", "02 " E4, "real time: E-4 played in row 02 landed there");
    line_is("03", "03 ... " EMPTY, "real time: row 03 empty");
    line_is("05", "05 " G4, "real time: G-4 played in row 05 landed there");
    line_is("0B", "0B --- .. . ... ... ... ...", "real time: shift + white key in row 0B: a cut there");
    line_is("01", "01 ... " EMPTY, "real time: the cursor row (00) did not take them, row 01 empty");
    shifted(SMK_BTN_PARA);
    line_is("0B", "0B ... " EMPTY, "undo after the real-time cut: row 0B empty again");
    line_is("05", "05 " G4, "undo after the real-time cut: the G-4 before it kept");

    /* the header at its fullest: VEL, REC, LOOP, an instrument (02 KICK) */
    {
        char why[160], msg[320];
        int ok;
        button(SMK_BTN_NOTE_REPEAT);         /* instrument 02 KICK selected: the header at its fullest */
        sim_pad(2, 100);
        sim_run_ms(40);
        sim_pad(2, 0);
        sim_run_ms(40);
        button(SMK_BTN_NOTE_REPEAT);
        shifted(SMK_BTN_SEQ_PLAY);           /* LOOP */
        ok = sim_screen_tidy(why, sizeof why);
        snprintf(msg, sizeof msg, "the header with VEL, REC, LOOP, INS02: no text cut or overlapping (%s)", why);
        check(ok && sim_screen_text("LOOP") && sim_screen_text("INS02"), msg);
    }

    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/record.ppm", argv[1]);
        sim_save_ppm(path);
    }
    printf("%s\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return fails != 0;
}
