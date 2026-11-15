# A chip per track, one shared instance per chip

Each track chooses a chip and one of its channels (for example NES Pulse 2, Game Boy Wave). Tracks that choose the same chip share one instance of it: each channel can be taken by one track only, and the chip mixes its channels as the original does; different chips are then summed. We considered one chip per project (authentic, but no combinations) and an own chip instance per track (any channel on any track, but four NES side by side no longer sound like one console). The shared instance keeps both: four NES tracks sound exactly like an NES, and mixed projects are possible.

## Consequences

Instruments belong to one chip; a row can only use instruments of its track's chip. Each chip advances in its own frame rate (NES and Game Boy about 60 Hz, SID 50 Hz) under one common BPM. The stored project records chip and channel per track from the first release, even while only the NES exists.
