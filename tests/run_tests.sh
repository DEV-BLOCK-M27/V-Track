#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host tests of the V-Track sources (no hardware). Run from the repo root after ./build.sh:
#   tests/run_tests.sh
#
# DEVICE (seam A, tests/vt_sim.h): the firmware (firmware/src/vtrack.c) on a simulated SMK-37: input events and
#                   elapsed time in; audio, the screen, the pad and button LEDs, MIDI out and the flash store out.
#                   tests/vt_shell_test.c: the app shell (starts into the V-Track screen, silent, LEDs dark, no
#                   flash written); the screen in build/host/shell.ppm. tests/vt_note_test.c: a key plays track 1,
#                   NES Pulse 1, at the 2A03's pitch, one note at a time; a held C4 in build/host/note.wav.
#                   tests/vt_tracks_test.c: K3 selects tracks 1 to 4 (NES Pulse 1, Pulse 2, Triangle, Noise), the
#                   screen names each, the keys play each; track 3's screen in build/host/tracks.ppm.
# NES chip core (seam B, tests/nes_test.c): register states in, samples out: duty waveforms, period -> frequency,
#                   the triangle's staircase, the noise's periods and LFSR modes, the 2A03 mixer, no aliasing on
#                   high notes.
# Storage (tests/storage_test.c): A/B objects, torn writes. SMK flash map (tests/smk_flash_test.c): the allow-lists
#                   never reach the protected ranges (head, BTIF / USRTRIM, USR, key_mac); the store owner tag
#                   (tests/smk_owner_test.c).
# USB (tests/usb_desc_test.c, tests/uac_test.c): the descriptor layouts, the USB audio ring, packets and resampler.
# Update path: the M-UPGRADE entry (tests/ota_test.c), the update loader (tests/ldr_test.c), the package layout
#                   (tests/smk_pkg_test.py), the installer CLI (tests/install_test.py).
# Input (tests/input_test.c): the FM-1 key / button scan of hal/fm1_input.h (the hardware layer, kept as it is).
# Display (tests/theme_test.c, tests/text_ref_test.c, tests/text_spacing_test.py): palettes, the text renderer.
# Target cost (tests/target_budget.py): the loop instructions of the ISRs in the pi32v2 disassembly
#                   (tests/target_budget.txt, +10 %).
# Sanitizers (ASan + UBSan, when the compiler has them; SANITIZE=0 skips): the device, the stored-data and update paths
#                   again.
# web/: Felucca's pages and browser emulator, not run until their V-Track port (#16, #17).
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
PKG=build/felucca-smk37.fwsc
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

$CC -o "$OUT/storage_test" tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"

$CC -o "$OUT/input_test" tests/input_test.c
run "FM-1 keys and buttons (hal/fm1_input.h): fast press, long release, bouncy contacts, glitches, encoders" "$OUT/input_test"

HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/vtrack.c)
$CC -O2 -DT_CDC=1 -DHALF_FRAMES=$HALF -o "$OUT/uac_test" tests/uac_test.c -lm
run "USB audio input: descriptors (with CDC), ring and packets, 44.1 -> 48 kHz resampler, 48 kHz stream" "$OUT/uac_test"
$CC -O2 -DT_CDC=0 -DHALF_FRAMES=$HALF -o "$OUT/uac_test_nocdc" tests/uac_test.c -lm
run "USB audio input: descriptors (without CDC), ring and packets, resampler, 48 kHz stream" "$OUT/uac_test_nocdc"
# USB descriptor layouts (#67): CDC UAC LAYOUT CDC-presented 48K; layout 0 and the console left out = 1.0's bytes
# (+ the 48 kHz rate with 48K = 1, the default; 48K = 0: byte for byte; + the AC collection's audio streaming
# interface first, FELUCCA_UAC_AS_FIRST = 1, the default since 1.6)
for v in 1.1.0.1.1 1.1.1.1.1 1.1.2.1.1 1.1.3.1.1 1.1.0.0.1 1.1.2.0.1 0.1.0.1.1 1.1.0.1.0 1.1.0.0.0 1.1.2.1.0 0.1.0.1.0 \
         1.0.0.1.1 1.0.1.1.1 1.0.0.0.1 0.0.0.1.1; do
    IFS=. read -r t_cdc t_uac t_lay t_on t_48 <<EOF
$v
EOF
    $CC -DT_CDC="$t_cdc" -DT_UAC="$t_uac" -DT_LAYOUT="$t_lay" -DT_ON="$t_on" -DT_48K="$t_48" -o "$OUT/usb_desc_test" \
        tests/usb_desc_test.c
    run "USB descriptors: CDC $t_cdc (presented $t_on), UAC $t_uac (48 kHz $t_48), layout $t_lay" "$OUT/usb_desc_test"
done
# FELUCCA_UAC_AS_FIRST=0 (#67): the AC collection of 1.0 .. 1.5 (MIDI first), 1.0's bytes
for v in 1.1.0.1.1 1.1.0.0.1 1.1.0.1.0 1.1.0.0.0 0.1.0.1.1; do
    IFS=. read -r t_cdc t_uac t_lay t_on t_48 <<EOF
$v
EOF
    $CC -DT_CDC="$t_cdc" -DT_UAC="$t_uac" -DT_LAYOUT="$t_lay" -DT_ON="$t_on" -DT_48K="$t_48" -DT_ASF=0 \
        -o "$OUT/usb_desc_test" tests/usb_desc_test.c
    run "USB descriptors (AC collection MIDI first): CDC $t_cdc (presented $t_on), UAC $t_uac (48 kHz $t_48)" \
        "$OUT/usb_desc_test"
done

# the SMK-37 flash map, the store owner tag
$CC -DBOARD_SMK37=1 -Ifirmware/hal -o "$OUT/smk_flash_test" tests/smk_flash_test.c
run "SMK-37 flash map: allow-lists never reach the head, BTIF/USRTRIM, USR, key_mac; store layout" "$OUT/smk_flash_test"
$CC -DBOARD_SMK37=1 -Ifirmware/hal -o "$OUT/smk_owner_test" tests/smk_owner_test.c
run "SMK-37 store owner tag: another firmware's store cleared, ours kept, nothing else written" "$OUT/smk_owner_test"

[ -f "$PKG" ] && [ -f build/gen/ui_fonts.h ] || { echo "run ./build.sh first"; exit 1; }

# the update path on the SMK layout: the package build.sh made, and an "other app" (the head of this one)
head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py --board smk37 build/felucca.bin build/loader/ota.bin "$OUT/smk_new.fwsc" >/dev/null
python3 tools/fm1pkg_make.py --board smk37 "$OUT/old_app.bin" build/loader/ota.bin "$OUT/smk_old.fwsc" >/dev/null
run "SMK-37 package layout (fm1pkg_make --board smk37): PRCT 0x9D000, stock region table" python3 tests/smk_pkg_test.py "$OUT/smk_new.fwsc"
$CC -DBOARD_SMK37=1 -DOWN_PKG=1 -o "$OUT/ota_test_smk" tests/ota_test.c
run "M-UPGRADE entry, SMK-37 Pro build (staging 0xBB000, parm ota-SMK-37 Pro_016)" "$OUT/ota_test_smk" "$PKG"
for m in 1 2; do                         # the MODEL=elite / mke builds of ota.c
    $CC -DBOARD_SMK37=1 -DT_MODEL=$m -o "$OUT/ota_test_smk_m$m" tests/ota_test.c
    run "M-UPGRADE entry, SMK model $m build (MODEL=elite / mke: its own parm identity)" "$OUT/ota_test_smk_m$m" "$OUT/smk_new.fwsc"
done
for m in pro elite mke; do               # the packages build.sh made (MODEL=...), when present
    f=$PKG; id="SMK-37 Pro_900"
    [ $m = elite ] && f=build/felucca-smk37-elite.fwsc && id="SMK-37 Elite_900"
    [ $m = mke ] && f=build/felucca-smk37-mke.fwsc && id="MKE-P37_900"
    [ -f $f ] && run "SMK package $f: identity $id, stock region table" python3 tests/smk_pkg_test.py $f "$id"
done
$CC -DBOARD_SMK37=1 -o "$OUT/ldr_test_smk" tests/ldr_test.c
run "update loader, SMK-37 Pro: app area 0x4000-0x9CFFF, protected ranges untouched" "$OUT/ldr_test_smk" "$OUT/smk_old.fwsc" "$OUT/smk_new.fwsc"

# seam A: the device on the simulated SMK-37
VT="-Ibuild/gen -Ifirmware/src -Ifirmware/hal -Itests"
$CC $VT -o "$OUT/vt_shell_test" tests/vt_shell_test.c
run "DEVICE: the app shell boots into the V-Track screen; silence, dark LEDs, no flash written" \
    "$OUT/vt_shell_test" "$OUT"
$CC $VT -o "$OUT/vt_note_test" tests/vt_note_test.c -lm
run "DEVICE: a key plays track 1 (NES Pulse 1) at the 2A03's pitch while held, one note at a time" \
    "$OUT/vt_note_test" "$OUT"
$CC $VT -o "$OUT/vt_tracks_test" tests/vt_tracks_test.c -lm
run "DEVICE: K3 selects tracks 1 to 4 (NES Pulse 1, Pulse 2, Triangle, Noise), named on screen, each played live" \
    "$OUT/vt_tracks_test" "$OUT"

# seam B: the NES chip core alone
$CC -o "$OUT/nes_test" tests/nes_test.c -lm
run "NES chip core: pulse, triangle, noise (periods, LFSR modes), the 2A03 mixer, no aliasing on high notes" \
    "$OUT/nes_test"

# the display code (firmware/src/gfx.c)
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/theme_test" tests/theme_test.c -lm
run "themes: contrast, text blending and font metrics" "$OUT/theme_test"
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/text_ref_test" tests/text_ref_test.c -lm
run "text: pens, kerning and pixels equal the reference renderer" "$OUT/text_ref_test"
if python3 -c "import PIL" 2>/dev/null; then
    run "text spacing: glyph gaps against the font's own (16x supersampled), S M <= 0.25 px, L <= 0.5 px" \
        python3 tests/text_spacing_test.py build/gen/ui_fonts.h
else
    echo "== skip text spacing (python3 has no Pillow)"
fi

# ASan + UBSan: the device and the stored-data and update paths
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize=signed-integer-overflow,shift,bounds,object-size,pointer-overflow"
SAN="$SAN -fno-sanitize-recover=undefined -w"
san_ok() {
    printf 'int main(void){return 0;}\n' > "$OUT/san_probe.c" &&
        ${CC%% *} $SAN -o "$OUT/san_probe" "$OUT/san_probe.c" 2>/dev/null && "$OUT/san_probe"
}
if [ "${SANITIZE:-1}" = 0 ]; then
    echo "== skip the sanitizer runs (SANITIZE=0)"
elif ! san_ok; then
    echo "== skip the sanitizer runs (the compiler has no ASan / UBSan)"
else
    mkdir -p "$OUT/asan"
    A="$OUT/asan"
    SCC="${CC%% *} $SAN"
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
    $SCC -DBOARD_SMK37=1 -o "$A/ldr_test" tests/ldr_test.c
    run "ASan/UBSan: update loader, SMK-37 Pro" "$A/ldr_test" "$OUT/smk_old.fwsc" "$OUT/smk_new.fwsc"
    $SCC -DBOARD_SMK37=1 -DOWN_PKG=1 -o "$A/ota_test" tests/ota_test.c
    run "ASan/UBSan: M-UPGRADE entry, SMK-37 Pro" "$A/ota_test" "$PKG"
    $SCC -o "$A/storage_test" tests/storage_test.c
    run "ASan/UBSan: flash storage" "$A/storage_test"
    $SCC -DBOARD_SMK37=1 -Ifirmware/hal -o "$A/smk_owner_test" tests/smk_owner_test.c
    run "ASan/UBSan: SMK-37 store owner tag" "$A/smk_owner_test"
    $SCC $VT -o "$A/vt_shell_test" tests/vt_shell_test.c
    run "ASan/UBSan: DEVICE, the app shell" "$A/vt_shell_test"
    $SCC $VT -o "$A/vt_note_test" tests/vt_note_test.c -lm
    run "ASan/UBSan: DEVICE, a note on track 1" "$A/vt_note_test"
    $SCC $VT -o "$A/vt_tracks_test" tests/vt_tracks_test.c -lm
    run "ASan/UBSan: DEVICE, the four tracks" "$A/vt_tracks_test"
    $SCC -o "$A/nes_test" tests/nes_test.c -lm
    run "ASan/UBSan: NES chip core" "$A/nes_test"
fi

run "target cost of the ISRs (pi32v2 disassembly)" python3 tests/target_budget.py build/felucca.dis tests/target_budget.txt

run "installer CLI (fm1_install.py) against a simulated device" python3 tests/install_test.py

# web/: still Felucca's pages (the editor and backup protocols of the removed editor.c, the emulator of its sources);
# their V-Track port and tests come with #16 (the browser emulator) and #17 (the release)
echo "== skip the web pages and the browser emulator (Felucca's; ported in #16 / #17)"

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
