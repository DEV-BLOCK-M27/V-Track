/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Saving projects on the simulated SMK-37 (seam A, tests/vt_sim.h). GLOBE opens the project view: four slots with
 * their names, the project memory's fullness. K2 selects a slot, K4 sets the name's character (shift + K4 its
 * cursor); > saves to the slot (over a saved project: > again), < loads it (< again), shift + < starts a new project
 * (shift + < again). The current project is autosaved; the store belongs to the VTRACK data family.
 *   tests/vt_project_test [OUT_DIR]   (OUT_DIR: the project view as project.ppm) */
#include "vt_sim.h"

#define K2 1u
#define K3 2u
#define K4 3u
#define K5 4u
#define K6 5u
#define K7 6u

static int fails;
static void check(int ok, const char *what)
{
    printf("%-84s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void press(uint32_t b, int shift)
{
    if (shift)
        sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    sim_button(b, 1);
    sim_run_ms(30);
    sim_button(b, 0);
    sim_run_ms(30);
    if (shift)
        sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
}
static void turn(uint32_t e, int32_t steps, int shift)
{
    if (shift)
        sim_button(SMK_BTN_KNOB_BANK, 1);
    sim_run_ms(30);
    sim_encoder(e, steps);
    sim_run_ms(30);
    if (shift)
        sim_button(SMK_BTN_KNOB_BANK, 0);
    sim_run_ms(30);
}
static uint64_t audio_hash(uint32_t ms)      /* SEQ PLAY for ms, then stop: the audio's FNV-1a hash (0: too short) */
{
    uint32_t n, i, k;
    const int32_t *a;
    uint64_t h = 1469598103934665603ull;
    sim_audio(&n);
    press(SMK_BTN_SEQ_PLAY, 0);
    sim_run_ms(ms + 200u);
    press(SMK_BTN_SEQ_PLAY, 0);
    sim_run_ms(100);
    a = sim_audio(&n);
    for (i = 0; i < 2u * n && !a[i]; i++)    /* from the first sound on (the start waits for an audio block) */
        ;
    i &= ~1u;
    for (k = 0; k < 2u * ms * 44u && i + k < 2u * n; k++)   /* ms of it */
        h = (h ^ (uint32_t)a[i + k]) * 1099511628211ull;
    return k == 2u * ms * 44u ? h : 0u;
}

static int store_window_only(const uint8_t *before)   /* the flash changed only inside the store window */
{
    uint32_t i;
    for (i = 0; i < SIM_FLASH_SIZE; i++)
        if ((i < FL_DATA_LO || i >= FL_DATA_HI) && sim_flash()[i] != before[i])
            return 0;
    return sim_flash_refused() == 0;
}

static int memory_shown(void)                /* the fullness the project view shows, -1 if none */
{
    char s[16];
    int n;
    for (n = 0; n <= 100; n++) {
        snprintf(s, sizeof s, "MEMORY %d%%", n);
        if (sim_screen_text(s))
            return n;
    }
    return -1;
}

/* the stored project of slot (0..3) as the flash holds it: the first part's bytes (storage.c: a sector from the
 * second of the store window on, its 32-byte header "VTRK", object, part, ..; project.c: the stored format) */
static const uint8_t *stored_slot(uint32_t slot)
{
    uint32_t off, best = 0, seq = 0;
    for (off = FL_DATA_LO + 0x1000u; off < FL_DATA_HI; off += 0x1000u) {
        const uint8_t *h = sim_flash() + off;
        uint32_t q = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
        if (!memcmp(h, "VTRK", 4) && h[4] == slot && h[5] == 0 && q > seq)
            best = off, seq = q;
    }
    return best ? sim_flash() + best + 32u : 0;
}

static uint16_t shown[240 * 240];
static const char *out_dir;

/* row r of track t: note (semitones from C-4), instrument, volume 0..15, entered with K5 to K7 (no sound: the chip
 * stays as it was at power-on, so two plays of the same project render the same samples) */
static void enter_row(uint32_t t, uint32_t r, int32_t note, int32_t ins, int32_t vol)
{
    turn(K3, -3, 0);
    turn(K3, (int32_t)t, 0);
    turn(K2, -64, 0);
    turn(K2, (int32_t)r, 0);
    turn(K5, note < 0 ? note - 1 : note + 1, 0);   /* the first detent: C-4, then semitones */
    turn(K6, ins + 1, 0);                    /* .. 00, then on */
    turn(K7, vol - 16, 0);                   /* .. F, then down */
}

/* a project built at the controls: notes with instruments on tracks 1, 3 and 4, a second order-list entry, 150 BPM */
static void build_project(void)
{
    enter_row(0, 0, 0, 0, 15);
    enter_row(0, 4, 4, 0, 12);
    enter_row(0, 8, 7, 1, 15);
    enter_row(2, 0, -12, 1, 15);
    enter_row(2, 8, -5, 1, 15);
    enter_row(3, 0, 0, 2, 15);
    enter_row(3, 4, 0, 4, 9);
    enter_row(3, 12, 0, 3, 15);
    turn(K4, 30, 0);
    press(SMK_BTN_ARP, 0);                   /* a second entry (new patterns), the cursor back to the first */
    press(SMK_BTN_RIGHT, 0);
    turn(K2, -1, 0);
    press(SMK_BTN_SEQ, 0);
    turn(K3, -3, 0);
    turn(K2, -64, 0);
}

static void save_load_test(void)
{
    static uint8_t before[SIM_FLASH_SIZE];
    uint64_t h_built, h_loaded;
    char why[96];
    int mem;
    sim_power_on();
    memcpy(before, sim_flash(), SIM_FLASH_SIZE);
    build_project();
    memcpy(shown, sim_frame(), sizeof shown);
    h_built = audio_hash(3000);

    press(SMK_BTN_GLOBE, 0);
    check(sim_screen_text("PROJECT"), "GLOBE opens the project view");
    check(sim_screen_text("1 EMPTY") && sim_screen_text("4 EMPTY"), "four slots, all empty at first");
    turn(K4, 0, 0);
    turn(K4, 3, 1);                          /* the name's cursor past "NEW": an A */
    turn(K4, 1, 0);
    turn(K2, 1, 0);                          /* slot 2 */
    mem = memory_shown();
    check(mem >= 0 && mem < 10, "the project memory's fullness is shown");
    press(SMK_BTN_RIGHT, 0);
    check(sim_screen_text("SAVED TO SLOT 2"), "> saves to the slot");
    check(sim_screen_text("2 NEWA"), "the slot shows the project's name");
    check(sim_screen_tidy(why, sizeof why), why);
    {   /* 'V' 'T' version, name[8], bpm, then each track's chip, channel, volume, mute */
        const uint8_t *b = stored_slot(1);
        static const uint8_t TRACKS[16] = {1, 0, 127, 0, 1, 1, 127, 0, 1, 2, 127, 0, 1, 3, 127, 0};
        check(b && b[0] == 'V' && b[1] == 'T' && b[2] == 1 && !memcmp(b + 3, "NEWA", 5) && b[11] == 150 && !b[12],
              "the stored format: its version (1), the name, the tempo");
        check(b && !memcmp(b + 13, TRACKS, sizeof TRACKS),
              ".. each track's chip and channel (NES: Pulse 1, Pulse 2, Triangle, Noise), the mix");
    }
    if (out_dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/project.ppm", out_dir);
        sim_save_ppm(path);
    }
    check(memory_shown() > mem && memory_shown() <= mem + 4, "the fullness grows by the project's share");

    press(SMK_BTN_LEFT, 1);
    check(sim_screen_text("NEW PROJECT? SHIFT < AGAIN"), "shift + < asks before starting a new project");
    press(SMK_BTN_LEFT, 1);
    check(sim_screen_text("NEW PROJECT"), "shift + < again: a new project");

    sim_power_off();
    sim_power_on();
    press(SMK_BTN_GLOBE, 0);
    check(sim_screen_text("2 NEWA") && sim_screen_text("1 EMPTY"), "after a power cycle the slot is still there");
    turn(K2, 1, 0);
    press(SMK_BTN_LEFT, 0);
    check(sim_screen_text("LOAD SLOT 2? < AGAIN"), "< asks before loading");
    press(SMK_BTN_LEFT, 0);
    check(sim_screen_text("LOADED SLOT 2"), "< again loads it");
    press(SMK_BTN_SEQ, 0);
    sim_run_ms(100);
    check(!memcmp(shown, sim_frame(), sizeof shown), "the loaded project shows as it did (the pattern view)");
    h_loaded = audio_hash(3000);
    check(h_built != 0u && h_loaded == h_built, "the loaded project plays identically (audio hash)");
    check(store_window_only(before), "only the store window was written, nothing refused");
}

/* the last edits after a power cut: an edited row and the tempo come back; an edit right before the cut (not yet
 * quiet) may be lost, but never the project autosaved before it */
static void autosave_test(void)
{
    char line[64];
    sim_power_on();
    press(SMK_BTN_SEQ, 0);
    enter_row(1, 2, 2, 3, 10);               /* track 2, row 02: D-4 03 A */
    turn(K4, -20, 0);                        /* the tempo: 130 BPM */
    sim_run_ms(4000);                        /* quiet: autosaved */
    sim_power_off();
    sim_power_on();
    press(SMK_BTN_SEQ, 0);
    turn(K3, 1, 0);
    turn(K2, 2, 0);
    sim_run_ms(100);
    check(sim_screen_line("02", line, sizeof line) && !strncmp(line, "02 D-4 03 A", 11),
          "the autosave brings the last edit back after a power cut");
    check(sim_screen_text("T2") && sim_screen_text("130 BPM"), "the tempo too");
    enter_row(1, 5, 4, 3, 10);               /* an edit, then the power cut 2.5 s later: */
    sim_run_ms(2500);
    sim_power_off();
    sim_power_on();
    press(SMK_BTN_SEQ, 0);
    turn(K3, 1, 0);
    turn(K2, 5, 0);
    sim_run_ms(100);
    check(sim_screen_line("05", line, sizeof line) && !strncmp(line, "05 E-4 03 A", 11),
          "an edit autosaved within a few seconds");
    check(sim_screen_line("02", line, sizeof line) && !strncmp(line, "02 D-4 03 A", 11), "the edits before it kept");
}

/* another firmware's store (Felucca's tag; under it even data shaped like V-Track's): cleared on the first boot,
 * never read as a project; the store then holds V-Track's tag only, nothing outside the window touched */
static void other_family_test(void)
{
    static uint8_t before[SIM_FLASH_SIZE];
    static const char FELUCCA[16] = "SMK37DATAFELUCCA", VTRACK[16] = "SMK37DATAVTRACK";
    uint8_t sector[0x1000];
    uint32_t i, clear = 1;
    sim_power_off();
    memset(sector, 0xFF, sizeof sector);
    memcpy(sector, FELUCCA, sizeof FELUCCA);
    sim_flash_set(FL_DATA_LO, sector, sizeof sector);   /* (the slots and autosave the tests above left stay) */
    memset(sector, 0x11, sizeof sector);
    sim_flash_set(FL_DATA_HI - 0x1000u, sector, sizeof sector);
    memset(sector, 0x33, sizeof sector);
    sim_flash_set(0xC1000u, sector, sizeof sector);      /* USRTRIM: the unit's calibration */
    memcpy(before, sim_flash(), SIM_FLASH_SIZE);
    sim_power_on();
    press(SMK_BTN_GLOBE, 0);
    check(sim_screen_text("1 EMPTY") && sim_screen_text("2 EMPTY") && sim_screen_text("3 EMPTY") &&
              sim_screen_text("4 EMPTY"),
          "another family's store: no slot read from it");
    check(sim_screen_text("PROJECT") && sim_screen_text("NEW") && sim_screen_text("MEMORY 0%"),
          ".. nor the autosave: a new project, the memory empty");
    for (i = FL_DATA_LO; i < FL_DATA_HI; i++)
        clear &= i < FL_DATA_LO + 16u ? sim_flash()[i] == (uint8_t)VTRACK[i - FL_DATA_LO] : sim_flash()[i] == 0xFFu;
    check(clear, "the store window cleared, V-Track's tag SMK37DATAVTRACK in its first sector");
    check(store_window_only(before), "nothing outside the store window touched (the calibration kept)");
    sim_power_off();
    sim_power_on();
    press(SMK_BTN_GLOBE, 0);
    press(SMK_BTN_RIGHT, 0);
    sim_power_off();
    memcpy(before, sim_flash(), SIM_FLASH_SIZE);
    sim_power_on();
    press(SMK_BTN_GLOBE, 0);
    check(sim_screen_text("1 NEW") && !memcmp(before, sim_flash(), SIM_FLASH_SIZE),
          "V-Track's own store kept at the next boot, nothing erased");
}

/* a project too big for the memory left: refused with a message, nothing written, the slot keeps its project.
 * The big one: a pattern of 64 full rows pasted into all 32 patterns of all 4 tracks (some 11 of the 29 sectors);
 * with its autosave and a copy in slot 2 there is no room left for another copy */
static void oversized_test(void)
{
    static uint8_t before[SIM_FLASH_SIZE];
    char line[64];
    uint32_t r, t, p;
    sim_power_off();
    sim_power_on();                          /* slot 1: the small project "NEW" the test above saved */
    press(SMK_BTN_SEQ, 0);
    turn(K2, 48, 1);                         /* 64 rows */
    for (r = 0; r < 64u; r++) {
        turn(K5, 1 + (int32_t)(r % 12u), 0);
        turn(K6, 1 + (int32_t)(r % 5u), 0);
        turn(K7, -1 - (int32_t)(r % 8u), 0);
        turn(K2, 1, 0);
    }
    press(SMK_BTN_SCALE, 0);                 /* copied */
    for (t = 0; t < 4u; t++) {
        turn(K3, -3, 0);
        turn(K3, (int32_t)t, 0);
        for (p = 0; p < 32u; p++) {
            press(SMK_BTN_SCALE, 1);         /* pasted, the next pattern */
            press(SMK_BTN_RIGHT, 1);
        }
    }
    press(SMK_BTN_GLOBE, 0);
    turn(K4, -1, 0);                         /* named "MEW" */
    sim_run_ms(4000);                        /* autosaved */
    check(memory_shown() > 30 && memory_shown() < 50, "a big project: its autosave takes a third of the memory");
    turn(K2, 1, 0);
    press(SMK_BTN_RIGHT, 0);
    check(sim_screen_text("SAVED TO SLOT 2") && sim_screen_text("2 MEW"), "saved once: it fits");
    turn(K2, -1, 0);
    press(SMK_BTN_RIGHT, 0);
    check(sim_screen_text("SAVE OVER SLOT 1? > AGAIN"), "> over a saved project asks first");
    memcpy(before, sim_flash(), SIM_FLASH_SIZE);
    press(SMK_BTN_RIGHT, 0);
    check(sim_screen_text("NOT SAVED: MEMORY FULL"), "saved again over slot 1: refused, with a message");
    check(!memcmp(before, sim_flash(), SIM_FLASH_SIZE), "nothing written");
    check(sim_screen_text("1 NEW") && sim_screen_text("2 MEW"), "slot 1 keeps its project's name");
    press(SMK_BTN_LEFT, 0);
    press(SMK_BTN_LEFT, 0);
    check(sim_screen_text("LOADED SLOT 1") && sim_screen_text("NEW"), "slot 1 loads its own project");
    press(SMK_BTN_SEQ, 0);
    sim_run_ms(100);
    check(sim_screen_line("00", line, sizeof line) && !strncmp(line, "00 ... .. . ...", 15) &&
              sim_screen_text("16 ROWS"),
          ".. the small one, not the big one");
    check(sim_flash_refused() == 0, "the driver refused nothing");
}

int main(int argc, char **argv)
{
    out_dir = argc > 1 ? argv[1] : 0;
    save_load_test();
    autosave_test();
    other_family_test();
    oversized_test();
    printf("%s\n", fails ? "PROJECT TEST FAILED" : "project test passed");
    return fails != 0;
}
