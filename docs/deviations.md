# Deviations from the tickets

Where an implementation differs from what its ticket asked for, or settles something the ticket left open. Newest
ticket first.

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
