/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* V-TRACK: one compilation unit (the HAL is header-only). Each file uses what the files above it define, so the
 * order matters. Built on Felucca 1.6.1 with the SMK-37 board layer (docs/adr/0001): the hardware layer, USB audio
 * and MIDI, storage, the update path come from there; the app (app.c) is V-Track's own.
 *
 * The host simulator (tests/vt_sim.h) includes this file with VT_SIM defined: it brings its own simulated board
 * (input state, clock, audio DMA, display, pad LEDs, flash chip) and everything from here on runs as on the device. */

#include <stdint.h>

/* ---------------------------------------------------- build options --- */
/* build.py: FELUCCA_<NAME>=0 / 1 in the environment overrides these (the names are Felucca's: the kept modules
 * read them). */
#if !BOARD_SMK37
#error "V-Track builds for the SMK-37 only (BOARD_SMK37=1; tools/build.py --board smk37)"
#endif
#ifndef FELUCCA_FLASH
#define FELUCCA_FLASH 1          /* flash driver + storage.c */
#endif
#ifndef FELUCCA_OTA
#define FELUCCA_OTA 1            /* M-UPGRADE update entry (the installers); needs FELUCCA_FLASH */
#endif
#ifndef FELUCCA_OTA_DRYRUN
#define FELUCCA_OTA_DRYRUN 0     /* 1 = stage, ask "success", then undo: no record, no reset */
#endif
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0            /* no USB serial console: USB MIDI and USB audio only */
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1            /* USB audio input: the master output, 16-bit stereo (usb.c) */
#endif
#ifndef FELUCCA_UAC_TONE
#define FELUCCA_UAC_TONE 0       /* bench: the USB input sends test triangles instead of the music */
#endif
#ifndef FELUCCA_UART
#define FELUCCA_UART 0           /* the SMK's TRS jack stays unused (PH8 is a key-matrix row: never UART RX) */
#endif
#if FELUCCA_UART
#error "BOARD_SMK37: no TRS MIDI (PH8 is a key-matrix row on the SMK)"
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "SMK-37 Pro_900"   /* package identity (build.py: the .fwsc marker string) */
#endif
#ifndef FELUCCA_CPU2
#define FELUCCA_CPU2 0           /* one core: the second core's code is not built */
#endif
#define FM1_CPU1_USED FELUCCA_CPU2   /* hal/fm1_sys.h: whether every reset holds the second core first */
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "v0.0"   /* shown on the boot screen; build.py --release X.Y */
#endif
#if FELUCCA_OTA && !FELUCCA_FLASH
#error "FELUCCA_OTA needs FELUCCA_FLASH"
#endif

#define FS 44100u                /* the audio rate the app renders at (the I2S clock: ~44,117.6 Hz) */
#define HALF_FRAMES 128          /* I2S half buffer: 2.9 ms at 44.1 kHz (tests/run_tests.sh reads this line) */
#define CTL 32u                  /* frames per audio block: the half renders in blocks of CTL */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms; /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */
#define BOOTGUARD_MAGIC 0x42475244u
#ifndef VT_SIM
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));   /* boot loops (main.c) */
#endif

/* ------------------------------------------------------------- HAL --- */
#ifndef VT_SIM
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "smk_input.h"           /* SMK-37: keys, buttons, encoders, button LEDs, pads, faders, wheels, battery */
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_lcd_hw.h"
#include "libc.c"
#include "lcd.c"                 /* the display and the 16 RGB pads */
#if FELUCCA_FLASH
#include "fm1_flash.h"
#endif
#endif

/* ----------------------------------------------------- base, display --- */
#include "gfx.c"

/* ----------------------------------------------------- USB, audio --- */
#include "usb.c"
#ifdef VT_SIM_USB
#include VT_SIM_USB              /* the simulator: no USB controller; usb.c's MIDI rings are its host side */
#endif
#include "audio.c"

/* --------------------------------------------- storage, update --- */
#if FELUCCA_FLASH
#include "storage_hw.c"
#include "storage.c"
#endif
#if FELUCCA_OTA
#include "ota.c"
#include "ota_hw.c"
#endif

/* -------------------------------------------------------------- app --- */
#include "app.c"
#include "main.c"
