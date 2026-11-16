/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* The player: turns the tracks into chip register states (nes.c). app.c calls it:
 *   player_init()                    power-on: every track silent
 *   player_note_on(track, note, ins) a live note (MIDI number) on track 0..3 with instrument ins (0..15, or
 *                                    PLAYER_NO_INS): it takes the track's channel over
 *   player_note_off(track, note)     its key up: silent, if it is the note the track plays
 *   player_play() / player_stop()    the sequencer: looped playback of the patterns (project.c) from their first
 *                                    row / silence; it takes effect at the next audio block. The loop is as long
 *                                    as the longest of the four; a shorter one is silent (empty rows) past its end
 *   player_playing(), player_row()   whether it plays, and the row playing
 *   player_audio(out, n)             in the audio ISR: n stereo frames, Q15, interleaved L R
 *   player_track_name(track)         the chip and channel track 0..3 plays, as the screen names it ("NES · PULSE 1")
 * A new project's tracks 1 to 4 play NES Pulse 1, Pulse 2, Triangle and Noise (ADR 0002: one shared 2A03).
 * One row is one sixteenth at the project's BPM: a row starts with the first audio block (CTL samples, 0.7 ms) after
 * its time, the remainder carried, so the rows keep the tempo exactly on average. The tempo is read each block: a
 * change applies from the row playing on.
 * A row's instrument column sets the track's instrument (it stays until another), its volume column the track's
 * volume (15 at the start; it applies to the note sounding too). A row's note starts the instrument: each of its
 * sequences (project.c: volume, arpeggio, pitch, duty or mode) from its first step, a step each NES frame (NTSC,
 * 60.0988 Hz, counted from the note), looping to its loop point at its end or else holding its last value (the
 * pitch: adding no more). Not yet released, a sequence with a release point loops there to its loop point if that
 * comes before it, else holds it. A release (===) jumps every sequence with a release point to the step after it;
 * after the release a sequence loops only to a loop point past its release point. A cut silences at once. The
 * volume a channel gets: row volume x sequence volume / 15, at least 1 when both are non-zero; 0 silences it (the
 * triangle, which has no volume, too). Arpeggio steps add semitones to the note, pitch steps add to the period each
 * frame (+ lower; the noise: its period index), duty steps set the pulse duty (0..3) or the noise mode (0 long, 1
 * short). Without an instrument (none set, a free slot, another chip's) a note plays as a plain tone: the pulses at
 * 50 % duty, the noise long, full volume times the row volume; a release silences it.
 * A live note held on a track sounds over what the pattern plays there, with its own instrument, at full row
 * volume; its key up silences it and gives the channel back to the pattern. The noise's period comes from the note
 * as in FamiTracker: 15 - note mod 16, so a higher note a shorter period, wrapping every 16 notes. */
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

#define PLAYER_NO_INS 0xFFu
#define PLAYER_FRAME_HZ 600988u          /* the NES frame rate, NTSC: 60.0988 Hz, in 1 / 10000 Hz */
#define PLAYER_FRAME_UNITS (FS * 10000u) /* a frame: PLAYER_FRAME_UNITS / PLAYER_FRAME_HZ samples */
#define PLAYER_SEQ_END 0xFFu             /* a sequence past its end: its last value holds */

typedef struct {                         /* what plays on a track: the pattern's, or a live note */
    uint8_t note;                        /* MIDI number + 1; 0 = silent */
    uint8_t ins;                         /* the instrument playing, 0..15 or PLAYER_NO_INS */
    uint8_t vol;                         /* the row volume 0..15 */
    uint8_t released;
    uint8_t pos[PROJECT_SEQS];           /* each sequence's step, or PLAYER_SEQ_END */
    uint8_t s_vol, s_duty;               /* the volume and duty sequences' values now */
    int8_t s_arp;
    int16_t pitch;                       /* the pitch sequence's sum so far */
    uint32_t acc;                        /* the frame's time so far, in 1 / PLAYER_FRAME_HZ samples */
    uint16_t period;                     /* the register state it makes: the period (noise: 0..15), */
    uint8_t duty, volume;                /* duty (noise: mode), volume 0..15 (0: the channel off) */
} player_voice_t;

static struct {
    nes_t nes;
    /* the live note each track plays, one word (the audio ISR reads it whole): a count (each note on another) << 24
     * | note (MIDI number + 1; 0 = none) << 16 | instrument (0..15, PLAYER_NO_INS) */
    volatile uint32_t live[PLAYER_TRACKS];
    uint32_t live_seen[PLAYER_TRACKS];   /* the audio ISR's own: the live word its voice plays */
    player_voice_t voice[PLAYER_TRACKS][2];   /* [0] the pattern's, [1] the live note's */
    uint8_t ins[PLAYER_TRACKS];          /* each track's instrument (the rows set it) */
    volatile uint8_t req;                /* PLAYER_REQ_*: from the main loop to the audio ISR, 0 = taken */
    volatile uint8_t playing, row;
    uint8_t count;                       /* the main loop's note-on count */
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
    memset(player.voice, 0, sizeof player.voice);
    for (i = 0; i < PLAYER_TRACKS; i++) {
        player.live[i] = player.live_seen[i] = 0;
        player.ins[i] = PLAYER_NO_INS;
        player.voice[i][0].vol = player.voice[i][1].vol = 15;
    }
    player.req = player.playing = player.row = player.count = 0;
}

static void player_play(void) { player.req = PLAYER_REQ_PLAY; }
static void player_stop(void) { player.req = PLAYER_REQ_STOP; }
static int player_playing(void) { return player.req ? player.req == PLAYER_REQ_PLAY : player.playing; }
static uint32_t player_row(void) { return player.req == PLAYER_REQ_PLAY ? 0u : player.row; }

static void player_note_on(uint32_t track, uint32_t note, uint32_t ins)
{
    if (track < PLAYER_TRACKS && note < 128u)
        player.live[track] = (uint32_t)++player.count << 24 | (note + 1u) << 16 |
                             (ins < PROJECT_INSTRUMENTS ? ins : PLAYER_NO_INS);
}

static void player_note_off(uint32_t track, uint32_t note)
{
    if (track < PLAYER_TRACKS && (player.live[track] >> 16 & 0xFFu) == note + 1u)
        player.live[track] = 0;
}

/* --- a voice: its instrument's sequences (the audio ISR; once a frame or a row, out of the per-block loop) --- */

static void player_voice_out(uint32_t t, player_voice_t *v)   /* its register state from where it is */
{
    int32_t n = (int32_t)v->note - 1 + v->s_arp, p;
    uint32_t vol = v->vol * (uint32_t)v->s_vol / 15u;
    if (!vol && v->vol && v->s_vol)
        vol = 1;
    if (!v->note)
        vol = 0;
    n = n < 0 ? 0 : n > 127 ? 127 : n;
    p = (int32_t)player_period(t, (uint32_t)n) + v->pitch;
    if (t == PLAYER_NOISE)
        p = p < 0 ? 0 : p > 15 ? 15 : p;
    else
        p = p < 0 ? 0 : p > 0x7FF ? 0x7FF : p;
    v->period = (uint16_t)p;
    v->duty = t == PLAYER_NOISE ? v->s_duty & 1u : v->s_duty & 3u;
    v->volume = (uint8_t)vol;
}

static void player_voice_step(player_voice_t *v)   /* each sequence's value at its step */
{
    uint32_t s;
    for (s = 0; s < PROJECT_SEQS; s++) {
        const project_seq_t *q;
        int32_t x;
        if (v->pos[s] == PLAYER_SEQ_END)
            continue;
        q = project_seq(v->ins, s);
        if (v->pos[s] >= q->len) {       /* (it got shorter) */
            v->pos[s] = PLAYER_SEQ_END;
            continue;
        }
        x = q->v[v->pos[s]];
        if (s == PROJECT_SEQ_VOL)
            v->s_vol = (uint8_t)(x < 0 ? 0 : x > 15 ? 15 : x);
        else if (s == PROJECT_SEQ_ARP)
            v->s_arp = (int8_t)x;
        else if (s == PROJECT_SEQ_PITCH)
            v->pitch = (int16_t)(v->pitch + x);
        else
            v->s_duty = (uint8_t)x;
    }
}

static void player_voice_next(player_voice_t *v)   /* a frame: each sequence a step on */
{
    uint32_t s;
    for (s = 0; s < PROJECT_SEQS; s++) {
        const project_seq_t *q = project_seq(v->ins, s);
        uint32_t pos = v->pos[s], loop = q->loop, rel = q->release;
        if (pos == PLAYER_SEQ_END)
            continue;
        if (!v->released && rel != PROJECT_SEQ_NONE && pos == rel)   /* the release point: loop back or hold */
            pos = loop != PROJECT_SEQ_NONE && loop < rel ? loop : rel;
        else if (pos + 1u < q->len)
            pos++;
        else if (loop != PROJECT_SEQ_NONE && !(v->released && rel != PROJECT_SEQ_NONE && loop <= rel))
            pos = loop;
        else
            pos = PLAYER_SEQ_END;
        v->pos[s] = (uint8_t)pos;
    }
    player_voice_step(v);
}

static void player_voice_start(uint32_t t, player_voice_t *v, uint32_t note, uint32_t ins)
{
    uint32_t s;
    v->note = (uint8_t)(note + 1u);
    v->ins = (uint8_t)(ins < PROJECT_INSTRUMENTS && project_ins_chip(ins) == PROJECT_CHIP_NES ? ins : PLAYER_NO_INS);
    v->released = 0;
    v->s_vol = 15;
    v->s_arp = 0;
    v->s_duty = t == PLAYER_NOISE ? 0u : 2u;
    v->pitch = 0;
    v->acc = 0;
    for (s = 0; s < PROJECT_SEQS; s++)
        v->pos[s] = v->ins == PLAYER_NO_INS ? PLAYER_SEQ_END : 0u;
    player_voice_step(v);
    player_voice_out(t, v);
}

static void player_voice_release(uint32_t t, player_voice_t *v)
{
    uint32_t s;
    if (!v->note)
        return;
    if (v->ins == PLAYER_NO_INS) {
        v->note = 0;
    } else if (!v->released) {
        v->released = 1;
        for (s = 0; s < PROJECT_SEQS; s++) {
            const project_seq_t *q = project_seq(v->ins, s);
            if (q->release != PROJECT_SEQ_NONE && v->pos[s] != PLAYER_SEQ_END)
                v->pos[s] = (uint8_t)(q->release + 1u < q->len ? q->release + 1u : PLAYER_SEQ_END);
        }
        player_voice_step(v);
    }
    player_voice_out(t, v);
}

/* the audio ISR, once a block: the voices' frames, the live notes asked for */
static __attribute__((noinline)) void player_voices(uint32_t k)
{
    uint32_t t, i;
    for (t = 0; t < PLAYER_TRACKS; t++) {
        uint32_t l = player.live[t];
        if (l != player.live_seen[t]) {
            player.live_seen[t] = l;
            player.voice[t][1].vol = 15;
            if (l >> 16 & 0xFFu)
                player_voice_start(t, &player.voice[t][1], (l >> 16 & 0xFFu) - 1u, l & 0xFFu);
            else
                player.voice[t][1].note = 0;
        }
        for (i = 0; i < 2u; i++) {
            player_voice_t *v = &player.voice[t][i];
            if (!v->note || v->ins == PLAYER_NO_INS)
                continue;
            if ((v->acc += k * PLAYER_FRAME_HZ) >= PLAYER_FRAME_UNITS) {
                v->acc -= PLAYER_FRAME_UNITS;
                player_voice_next(v);
                player_voice_out(t, v);
            }
        }
    }
}

static uint32_t player_rows(void)        /* the loop: as long as the longest of the tracks' patterns */
{
    uint32_t t, n = 0;
    for (t = 0; t < PLAYER_TRACKS; t++) {
        uint32_t r = project_rows(t);
        n = r > n ? r : n;
    }
    return n;
}

/* the audio ISR, once a row: its instruments, volumes and notes; past the loop's end, row 0 */
static __attribute__((noinline)) void player_row_start(uint32_t row)
{
    uint32_t t, v;
    if (row >= player_rows())
        row = 0;
    player.row = (uint8_t)row;
    for (t = 0; t < PLAYER_TRACKS; t++) {
        player_voice_t *pv = &player.voice[t][0];
        if ((v = project_field(t, row, PROJECT_F_INS)) != PROJECT_EMPTY)
            player.ins[t] = (uint8_t)(v - 1u);
        if ((v = project_field(t, row, PROJECT_F_VOL)) != PROJECT_EMPTY)
            pv->vol = (uint8_t)(v - 1u);
        v = project_field(t, row, PROJECT_F_NOTE);
        if (PROJECT_IS_NOTE(v))
            player_voice_start(t, pv, v - 1u, player.ins[t]);
        else if (v == PROJECT_RELEASE)
            player_voice_release(t, pv);
        else if (v == PROJECT_CUT)
            pv->note = 0;
        player_voice_out(t, pv);
    }
}

static __attribute__((noinline)) void player_transport(void)   /* the audio ISR: a play or stop asked for */
{
    uint32_t t;
    for (t = 0; t < PLAYER_TRACKS; t++) {
        player.voice[t][0].note = 0;
        player.voice[t][0].vol = 15;
        player.voice[t][0].volume = 0;
        player.ins[t] = PLAYER_NO_INS;
    }
    player.acc = 0;
    player.playing = player.req == PLAYER_REQ_PLAY;
    if (player.playing)
        player_row_start(0);
    player.req = 0;                      /* (last: player_playing / player_row read the request until then) */
}

static const player_voice_t *player_sounding(uint32_t t)   /* track t's channel: the live note over the pattern's */
{
    return player.voice[t][1].note ? &player.voice[t][1] : &player.voice[t][0];
}

static void player_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k;
    if (player.req)
        player_transport();
    for (; n; n -= k, out += 2u * k) {
        const player_voice_t *v1, *v2, *vt, *vn;
        k = n < CTL ? n : CTL;
        player_voices(k);
        v1 = player_sounding(PLAYER_PULSE1);
        v2 = player_sounding(PLAYER_PULSE2);
        vt = player_sounding(PLAYER_TRIANGLE);
        vn = player_sounding(PLAYER_NOISE);
        {
            nes_pulse_t r1 = {v1->period, v1->duty, v1->volume, v1->volume != 0},
                        r2 = {v2->period, v2->duty, v2->volume, v2->volume != 0};
            nes_triangle_t rt = {vt->period, vt->volume != 0};
            nes_noise_t rn = {(uint8_t)vn->period, vn->duty, vn->volume, vn->volume != 0};
            nes_set_pulse(&player.nes, 0, &r1);
            nes_set_pulse(&player.nes, 1, &r2);
            nes_set_triangle(&player.nes, &rt);
            nes_set_noise(&player.nes, &rn);
        }
        nes_render(&player.nes, player.mono, k);
        for (i = 0; i < k; i++)
            out[2u * i] = out[2u * i + 1u] = player.mono[i];
        /* the row's time is up: the next row from the next block on */
        if (player.playing && (player.acc += k * project_bpm()) >= PLAYER_ROW_UNITS) {
            player.acc -= PLAYER_ROW_UNITS;
            player_row_start(player.row + 1u);
        }
    }
}
