# Own groovebox on Felucca 1.6.1 with the SMK-37 layer

V-Track is its own 8-bit groovebox with its own UI and its own sound (one emulated chip per project: NES, Game Boy, SID), built on Felucca 1.6.1 plus genkigenki's SMK-37 board layer (branch `smk37`). From Felucca we keep the hardware layer, USB audio and MIDI, storage, update path and installer, the test system and the browser emulator; Felucca's engines, UI, reverb and chorus go. We chose 1.6.1 because it is the only Felucca base with an SMK-37 Pro layer tested on a real unit; Felucca 1.7+ moved the hardware into felucca-bsp, whose SMK-37 board (felucca-bsp#30) is unmerged and has never run on a Pro.

## Consequences

Felucca releases after 1.6.1 are not merged any more; fixes from them are taken over by hand when needed. Moving to felucca-bsp later means porting V-Track's own code onto it, not rebasing.
