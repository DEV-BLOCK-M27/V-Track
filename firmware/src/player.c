/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The player: turns the tracks into chip register states (nes.c). app.c calls it:
 *   player_init()                    power-on: every track silent
 *   player_note_on(track, note)      a live note (MIDI number) on track 0..3: it takes the track's channel over
 *   player_note_off(track, note)     its release: silent, if it is the note the track plays
 *   player_audio(out, n)             in the audio ISR: n stereo frames, Q15, interleaved L R
 * So far: live notes on track 1, which plays NES Pulse 1 at 50 % duty and full volume, one note at a time. */
#define PLAYER_TRACKS 4u

static struct {
    nes_t nes;
    /* what each track plays, one word (the audio ISR reads it whole): the note (MIDI number + 1; 0 = none) << 16 |
     * its 2A03 period */
    volatile uint32_t live[PLAYER_TRACKS];
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

static void player_init(void)
{
    uint32_t i;
    nes_init(&player.nes);
    for (i = 0; i < PLAYER_TRACKS; i++)
        player.live[i] = 0;
}

static void player_note_on(uint32_t track, uint32_t note)
{
    if (track < PLAYER_TRACKS && note < 128u)
        player.live[track] = (note + 1u) << 16 | player_nes_period(note);
}

static void player_note_off(uint32_t track, uint32_t note)
{
    if (track < PLAYER_TRACKS && player.live[track] >> 16 == note + 1u)
        player.live[track] = 0;
}

static void player_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k;
    for (; n; n -= k, out += 2u * k) {
        uint32_t live = player.live[0];
        nes_pulse_t r = {(uint16_t)live, 2, 15, live != 0};
        k = n < CTL ? n : CTL;
        nes_set_pulse(&player.nes, 0, &r);
        nes_render(&player.nes, player.mono, k);
        for (i = 0; i < k; i++)
            out[2u * i] = out[2u * i + 1u] = player.mono[i];
    }
}
