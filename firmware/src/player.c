/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The player: turns the tracks into chip register states (nes.c). app.c calls it:
 *   player_init()                    power-on: every track silent
 *   player_note_on(track, note)      a live note (MIDI number) on track 0..3: it takes the track's channel over
 *   player_note_off(track, note)     its release: silent, if it is the note the track plays
 *   player_play() / player_stop()    the sequencer: looped playback of the pattern (project.c) from its first
 *                                    row / silence; it takes effect at the next audio block
 *   player_playing(), player_row()   whether it plays, and the row playing
 *   player_audio(out, n)             in the audio ISR: n stereo frames, Q15, interleaved L R
 *   player_track_name(track)         the chip and channel track 0..3 plays, as the screen names it ("NES · PULSE 1")
 * A new project's tracks 1 to 4 play NES Pulse 1, Pulse 2, Triangle and Noise (ADR 0002: one shared 2A03).
 * One row is one sixteenth at the project's BPM: a row starts with the first audio block (CTL samples, 0.7 ms) after
 * its time, the remainder carried, so the rows keep the tempo exactly on average (a NES sound driver writes the
 * registers once a 60 Hz frame). The tempo is read each block: a change applies from the row playing on. A row's
 * note takes the track's channel; a cut silences it, and so does a release (no instruments yet); an empty row
 * leaves it as it is. A live note held on a track sounds over what the
 * pattern plays there; its release gives the channel back to the pattern.
 * So far: one note at a time a track: the pulses at 50 % duty, full volume; the triangle; the noise in long mode
 * (short mode is the chip core's, not yet reachable from here), full volume, its period from the note as in
 * FamiTracker: 15 - note mod 16, so a higher note a shorter period, wrapping every 16 notes. */
#define PLAYER_TRACKS 4u
#define PLAYER_PULSE1 0u                 /* the tracks' channels */
#define PLAYER_PULSE2 1u
#define PLAYER_TRIANGLE 2u
#define PLAYER_NOISE 3u
#define PLAYER_ROW_UNITS (FS * 15u)      /* a row: FS * 60 / (4 BPM) samples = PLAYER_ROW_UNITS / BPM */
#define PLAYER_REQ_PLAY 1u
#define PLAYER_REQ_STOP 2u

static const char *const PLAYER_NAME[PLAYER_TRACKS] = {"NES \xB7 PULSE 1", "NES \xB7 PULSE 2", "NES \xB7 TRIANGLE",
                                                       "NES \xB7 NOISE"};   /* (\xB7: the font's middle dot) */

static struct {
    nes_t nes;
    /* what each track plays, one word (the audio ISR reads it whole): the note (MIDI number + 1; 0 = none) << 16 |
     * its channel's period (the 11-bit timer period; the noise: its period 0..15) */
    volatile uint32_t live[PLAYER_TRACKS];
    uint32_t seq[PLAYER_TRACKS];         /* the same for what the pattern plays (the audio ISR's own) */
    volatile uint8_t req;                /* PLAYER_REQ_*: from the main loop to the audio ISR, 0 = taken */
    volatile uint8_t playing, row;
    uint32_t acc;                        /* the row's time so far, in 1 / BPM samples */
    int32_t mono[CTL];
} player;

/* CPU / (16 f) for C1 .. B1 (MIDI 24 .. 35), Q8: an octave up halves it */
static const uint32_t PLAYER_NES_C1[12] = {875644, 826498, 780110, 736326, 694999, 655992,
                                           619174, 584422, 551621, 520661, 491439, 463856};

static uint16_t player_nes_period(uint32_t note)   /* the 2A03's 11-bit timer period nearest to the note's pitch */
{
    uint32_t o, t;
    if (note < 24u)
        return 0x7FF;
    o = (note - 24u) / 12u;
    if (o > 15u)
        return 0;
    t = (PLAYER_NES_C1[(note - 24u) % 12u] + (1u << (o + 7u))) >> (o + 8u);   /* round(CPU / (16 f)) = t + 1 */
    return (uint16_t)(t > 0x800u ? 0x7FF : t ? t - 1u : 0u);
}

static uint16_t player_period(uint32_t track, uint32_t note)   /* the note's period on the track's channel */
{
    if (track == PLAYER_TRIANGLE)
        return player_nes_period(note + 12u);         /* the triangle's timer steps 32 a period, the pulse's 16 */
    if (track == PLAYER_NOISE)
        return (uint16_t)(15u - note % 16u);
    return player_nes_period(note);
}

static const char *player_track_name(uint32_t track) { return track < PLAYER_TRACKS ? PLAYER_NAME[track] : ""; }

static void player_init(void)
{
    uint32_t i;
    nes_init(&player.nes);
    for (i = 0; i < PLAYER_TRACKS; i++)
        player.live[i] = player.seq[i] = 0;
    player.req = player.playing = player.row = 0;
}

static void player_play(void) { player.req = PLAYER_REQ_PLAY; }
static void player_stop(void) { player.req = PLAYER_REQ_STOP; }
static int player_playing(void) { return player.req ? player.req == PLAYER_REQ_PLAY : player.playing; }
static uint32_t player_row(void) { return player.req == PLAYER_REQ_PLAY ? 0u : player.row; }

static void player_note_on(uint32_t track, uint32_t note)
{
    if (track < PLAYER_TRACKS && note < 128u)
        player.live[track] = (note + 1u) << 16 | player_period(track, note);
}

static void player_note_off(uint32_t track, uint32_t note)
{
    if (track < PLAYER_TRACKS && player.live[track] >> 16 == note + 1u)
        player.live[track] = 0;
}

static __attribute__((noinline)) void player_row_start(uint32_t row)   /* the audio ISR, once a row: its notes */
{
    uint32_t t, v;
    player.row = (uint8_t)row;
    for (t = 0; t < PLAYER_TRACKS; t++) {
        v = project_note(t, row);
        if (PROJECT_IS_NOTE(v))
            player.seq[t] = v << 16 | player_period(t, v - 1u);
        else if (v == PROJECT_CUT || v == PROJECT_RELEASE)
            player.seq[t] = 0;
    }
}

static __attribute__((noinline)) void player_transport(void)   /* the audio ISR: a play or stop asked for */
{
    uint32_t t;
    for (t = 0; t < PLAYER_TRACKS; t++)
        player.seq[t] = 0;
    player.acc = 0;
    player.playing = player.req == PLAYER_REQ_PLAY;
    if (player.playing)
        player_row_start(0);
    player.req = 0;                      /* (last: player_playing / player_row read the request until then) */
}

static uint32_t player_sounding(uint32_t t)   /* what track t's channel plays: the live note over the pattern's */
{
    uint32_t l = player.live[t];
    return l ? l : player.seq[t];
}

static void player_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k;
    if (player.req)
        player_transport();
    for (; n; n -= k, out += 2u * k) {
        uint32_t p1 = player_sounding(PLAYER_PULSE1), p2 = player_sounding(PLAYER_PULSE2),
                 tri = player_sounding(PLAYER_TRIANGLE), no = player_sounding(PLAYER_NOISE);
        nes_pulse_t r1 = {(uint16_t)p1, 2, 15, p1 != 0}, r2 = {(uint16_t)p2, 2, 15, p2 != 0};
        nes_triangle_t rt = {(uint16_t)tri, tri != 0};
        nes_noise_t rn = {(uint8_t)no, 0, 15, no != 0};
        k = n < CTL ? n : CTL;
        nes_set_pulse(&player.nes, 0, &r1);
        nes_set_pulse(&player.nes, 1, &r2);
        nes_set_triangle(&player.nes, &rt);
        nes_set_noise(&player.nes, &rn);
        nes_render(&player.nes, player.mono, k);
        for (i = 0; i < k; i++)
            out[2u * i] = out[2u * i + 1u] = player.mono[i];
        /* the row's time is up: the next row from the next block on */
        if (player.playing && (player.acc += k * project_bpm()) >= PLAYER_ROW_UNITS) {
            uint32_t rows = project_rows(0);   /* (one length for all four tracks so far) */
            player.acc -= PLAYER_ROW_UNITS;
            player_row_start(player.row + 1u < rows ? player.row + 1u : 0u);
        }
    }
}
