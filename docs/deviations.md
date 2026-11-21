# Deviations from the tickets

Where an implementation differs from what its ticket asked for, or settles something the ticket left open. Newest
ticket first.

## #14 USB MIDI: notes and clock

Commit 5a1a13e.

### Not stated in the ticket, settled here

- **MIDI notes in play only.** They play the selected track as the keys do (one note at a time per track, any
  channel), but with REC on they write nothing into the rows. Their velocity is ignored, as the keys' is when playing.
- **Clock out only while playing.** On its own tempo V-Track sends a start, a pulse with row 0, 24 pulses per quarter
  (every 6th with a row) and a stop. While stopped it sends no clock.
- **Continue (FB) is treated as start.** V-Track has no song position; playback starts from the current entry's
  first row.
- **When incoming clock counts as present.** It counts as present for half a second after the last pulse, start or
  stop. In that time the rows follow the pulses (the first pulse after a start is row 0's, every 6th pulse after
  that starts the next row). V-Track then sends no clock, start or stop of its own.
- **Velocity out:** the row volume 0..F, scaled to 0..127 (at least 1). Live notes go out at 127.
- **Note off out:** sent when a note ends (cut, release, a new note, the live note over it, stop).

### Open (from the review, not yet fixed)

- **Start and stop out go missing near incoming clock.** A MIDI stop or any pulse, even while stopped, makes incoming
  clock count as present for 500 ms. If SEQ PLAY is pressed in that time, V-Track sends no start (and a stop pressed
  then sends no stop). Once the 500 ms are over, its own clock runs without a start before it. The same happens after
  a MIDI start when the clock then stops. Fix: decide whether clock is present from playback state, not from the
  last pulse alone; always send start and stop when V-Track plays on its own tempo.
- **Hand-back to the tempo is late.** While incoming clock drives the rows, the tempo's row and pulse counters
  (`player.acc`, `pacc`) are frozen and not reset when it ends. After a MIDI start, the first row on the tempo comes
  about 500 ms plus a whole row late; if clock came in during V-Track's own playback, the old part of a row carries
  over. The header in player.c says "from the row's start". Fix: reset both counters when incoming clock ends.
- **MIDI start and stop overwrite a pending SEQ PLAY.** `player_midi_start` and `player_midi_stop` write `player.req`
  from the audio ISR; its comment says it goes from the main loop to the ISR. A MIDI start wins over a user stop in
  the same block. Fix: start or stop directly, without `req`, or document the rule.
- **Stuck note after an overflow.** `app_midi_lost` is set by the ISR and cleared by the main loop without a
  handshake. A note on still in the note ring can play after the release sweep while its note off was dropped. This
  needs 32 notes queued in one frame, so it is unlikely.
- **Echo loop.** MIDI notes in go out again on channels 1 to 4 (live notes are sent, as the ticket asks). A DAW with
  MIDI thru or monitoring sends them back and re-triggers them. Possible fix: do not send notes that came in over MIDI,
  or make sending them a setting.
- **Code clean-up (judgement calls).**
  - Name the 6 pulses per row (`PLAYER_ROW_PULSES`).
  - Rename `etick` (the glossary avoids "tick"); also unclear: `ext`, `pacc`, `sent`, `app_mq`.
  - Pack and unpack MIDI packets with one helper instead of by hand in player.c, app.c and vt_sim.h.
  - Derive the held-notes bitmap index one way only.
  - Group the MIDI clock state (`ext`, `etick`, `pacc`).
  - Move "the player sends ..." from app.c's header to player.c.

## #13 Saving projects: four slots, autosave, VTRACK data family

Commit 1988bb9.

### Differs from the ticket

- **Autosave while the sequencer plays.** The ticket says that the current project is autosaved. An erase silences
  the audio for some 40 ms, so while the sequencer plays (or a key is held) the autosave only writes into sectors
  that were erased in advance, during stop. In a very long playing or live-recording session these can run out; the
  autosave then waits until stop, and a power cut in that time loses more than the last few seconds. **Decided
  (2026-11-21): kept for now.** The alternative, autosaving anyway after some time (for example 20 s) and accepting
  a short dropout, can be revisited later.
- **Audio hash in the save/load test.** The criterion asks that the reloaded project plays identically (audio hash).
  The chip keeps state from whatever played before, and playback starts at a different point within the audio block.
  So the test builds its project with silent edits and hashes a fixed length from the first non-zero sample. The
  hash is the same for the project before the save and after the power cycle and load.
- **Chip and channel per track: stored, but only the current assignment accepted.** The format records chip, channel,
  volume and mute for each track (ADR 0002). Loading accepts only NES with channel = track, the only assignment the
  firmware can play so far. A later ticket that adds chip or channel selection widens this check; the format itself
  needs no new version for it.
- **Mix: stored, no controls.** Track volume and mute are saved and loaded. There is no UI for them until #12.
- **Clearing another firmware's data.** This happens not only on the first boot but on every boot where the store
  window's first sector does not hold V-Track's tag `SMK37DATAVTRACK`. It is never read first. A window that is
  already erased only gets the tag (16 bytes) written. Because of this, the shell test now expects these 16 bytes on
  first boot, where it used to expect no writes at all.

### Not stated in the ticket, settled here

- **Only the store window is written.** The flash hooks (`storage_hw.c`) refuse every erase or write outside FL_DATA
  (0x9D000–0xBAFFF). The earlier code that released the stock firmware's USRFLASH area and its marker is removed.
- **Capacity.** The pool has 29 sectors of 4064 bytes, shared by the four slots and the autosave. The largest possible
  project (one full 64-row pattern copied into all 128 patterns) takes 11 sectors. With its autosave and one saved
  copy, a second such copy does not fit and is refused with "NOT SAVED: MEMORY FULL". A project of normal size
  takes one or two sectors.
- **Format versions.** The format is version 1. A slot written in another version shows as EMPTY and cannot be loaded.
  An autosave in another version gives a new project on boot.
- **"AUTOSAVE: MEMORY FULL"** only shows in the project view (GLOBE).
- **No room for a settings object.** The store has five objects (four slots, the autosave). A later settings ticket
  adds a sixth object.
