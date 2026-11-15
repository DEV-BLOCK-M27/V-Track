# V-Track

V-Track is an 8-bit groovebox firmware for the M-VAVE SMK-37: a tracker for four tracks, each sounding like a channel of a real sound chip of the 8-bit era.

## Language

### Sound

**Chip**:
An emulated sound chip of the 8-bit era: NES (2A03), Game Boy (DMG) or SID. Each track chooses one; tracks that choose the same chip share it, so each of its channels is used at most once.
_Avoid_: engine, synth, sound engine

**Channel** (Kanal):
One sound generator of the chip, such as Pulse 1, Triangle or Noise. Plays one note at a time.
_Avoid_: voice, oscillator, operator

**Instrument**:
A stored, chip-specific sound definition of a project that a row calls by number: on NES and Game Boy sequences for volume, arpeggio, pitch and duty; on SID wave, pulse and filter tables with an ADSR.
_Avoid_: preset, patch, sound

**Frame**:
The chip's own clock tick (NES and Game Boy about 60 Hz, SID 50 Hz) at which instrument sequences and effects advance.
_Avoid_: tick, control block

### Sequencing

**Track** (Spur):
One of the four sequencer lanes. Each track plays one channel of the chip it has chosen.
_Avoid_: part, lane

**Project** (Projekt):
One piece of music: the chip and channel of each track, its instruments, its patterns and its order list.
_Avoid_: song (when the whole piece is meant), session

**Pattern**:
A numbered list of rows for one track, 16 to 64 rows long.
_Avoid_: phrase, clip, sequence

**Row** (Zeile):
One sixteenth in a pattern: note, instrument, volume and one effect.
_Avoid_: step, line

**Cut**:
A note-column entry (`---`) that silences the track's channel at once.
_Avoid_: note off, stop

**Release**:
A note-column entry (`===`) that sends the playing instrument into its release part.
_Avoid_: note off, key up

**Effect** (Effekt):
A command in a row's effect column, written as a letter and two hex digits in FamiTracker style (for example `0xy` arpeggio).
_Avoid_: FX (that word means audio effects such as delay), parameter lock

**Order list** (Order-Liste):
The arrangement of a project: a list of entries, each naming one pattern per track, played top to bottom.
_Avoid_: song chain, frames, sequence
