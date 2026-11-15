/* SPDX-License-Identifier: GPL-3.0-only
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* tests/vt_sim.h: the simulated SMK-37 has no USB controller. main.c's calls into the USB driver's registers do
 * nothing; usb.c's MIDI rings (midi_enqueue, midi_out_q) and SysEx flags stay, the simulator is their host side.
 * Included by firmware/src/vtrack.c right after usb.c (VT_SIM_USB). */
#define usb_start() ((void)0)
#define usb_retry(ms) ((void)(ms))
#define usb_detach() ((void)0)
#define usb_poll() ((void)0)
#define uac_service() ((void)0)
