# Building V-Track

V-Track builds for the SMK-37 (Pro, Elite, MKE-P37) only. The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `felucca-smk37.fwsc` | the installable package (app + loader) |

## Prerequisites

- Python 3 with Pillow and fontTools: `pip3 install Pillow fonttools` (the UI font is rasterised at build time).
  `PYTHON=/path/to/python3 ./build.sh` picks another interpreter (a virtualenv).
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server), Linux x86-64 only:

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

  On macOS (or with `JIELI_DOCKER=1`) the build runs each tool in a `linux/amd64` `debian:bookworm-slim` container
  (Docker Desktop; Rosetta on Apple silicon; keep the source tree in a folder Docker can share).
- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Node.js (optional, for the web tests).

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`MODEL=elite` or `MODEL=mke` builds the same firmware under the SMK-37 Elite's or the MKE-P37's model name
(`build/felucca-smk37-elite.fwsc`, `build/felucca-smk37-mke.fwsc`), so the stock updater still takes that unit's factory
package. `./build.sh --release 1.0` makes a release build: identity `SMK-37 Pro_910`, version string `v1.0`, the
package `build/felucca-smk37-1.0.fwsc` and `build/release-1.0/` (the package, the app, `SHA256SUMS`, `LICENSE`,
`LICENSING.md`, `LICENSES/`). The intermediate files (`build/felucca.bin`, `build/loader/ota.bin`, `build/felucca.dis`)
are shared: build the Pro again before `tests/run_tests.sh`.

Build options (environment, `0` or `1`; defaults in `firmware/src/vtrack.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_FLASH` | 1 | the flash driver and storage |
| `FELUCCA_OTA` | 1 | update entry (needs `FELUCCA_FLASH`) |
| `FELUCCA_CDC` | 0 | USB serial (the device has no console behind it) |
| `FELUCCA_UAC` | 1 | USB audio input (the output, stereo, 44.1 or 48 kHz) |
| `FELUCCA_UAC_48K` | 1 | `0`: the USB audio input at 44.1 kHz only (`firmware/src/usb.c`) |
| `FELUCCA_UAC_AS_FIRST` | 1 | the audio control header lists the audio streaming interface before the MIDI one (older macOS) |
| `FELUCCA_CPU2` | 0 | `1`: the second core's code is linked in (nothing hands it work) |

`FELUCCA_USB_LAYOUT` (`0` to `3`, default `0`) picks other USB descriptor layouts for testing; see
`firmware/src/usb.c`.

## The source

`firmware/src/vtrack.c` is the one compilation unit: the hardware layer (`firmware/hal/`, header-only), the display
code, USB, the audio ISR, storage, the update entry, then the app (`firmware/src/app.c`) and the main loop
(`firmware/src/main.c`). The update loader is `firmware/loader/`.

## Tests

```
tests/run_tests.sh
```

Runs the host tests. Run it after `./build.sh` (it uses `build/` and needs
`AC79_SDK` set as for the build).

The device tests run the firmware's own sources on a simulated SMK-37 (`tests/vt_sim.h`, seam A): a test sends input
events (keys with velocity, pads, encoders, faders, buttons, wheels, USB MIDI) and lets time pass, then reads back what
a player would see, hear and keep: the rendered audio, the 240 x 240 screen and the text on it, the pad and button LEDs,
MIDI out and the flash store. The other suites cover flash storage, the SMK flash map and its allow-list, the store
owner tag, USB descriptors and USB audio, the update entry and loader, the package layout, the command-line installer
and the display code. `tests/target_budget.py` checks the loop cost of the ISRs in the target disassembly against
`tests/target_budget.txt`; `BUDGET_UPDATE=1` rewrites it after an intended change.

The browser emulator (`web/emu/`) still builds Felucca's sources; its V-Track port is a later step.

## Install

Use the web installer in Chrome or Edge:
<https://hugelton.github.io/Felucca/webapp/installer/>. It installs the released package.

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/felucca-smk37.fwsc
python3 tools/fm1_install.py --info          # identity of the connected keyboard
```

Or, to install your own build from the web installer, make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
python3 web/make_site.py build/felucca.fwsc dev /tmp/felucca-site
cd /tmp/felucca-site && python3 -m http.server 8000
# open http://localhost:8000/webapp/installer/
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
