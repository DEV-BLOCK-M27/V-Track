/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * V-Track: Copyright (C) 2026 Manuel Rubin */
/* Boot and main loop. Boot order: WDT first, boot-loop guard, fatal vectors, guards (fm1_cstart); then the flash,
 * the LCD and the boot screen, input (TIMER5 IRQ, 10 kHz), audio (ALNK0 IRQ), USB. The main loop:
 *   vt_boot()             power-on up to the app's first screen
 *   vt_frame()            one main-loop frame (~60 a second): USB requests, the update path, the app's frame
 *   vt_idle()             between frames
 * The host simulator (tests/vt_sim.h) drives these three as the device's fm1_main does. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

#ifndef VT_SIM                                  /* (the simulator has no interrupts, faults or reset) */
/* TIMER5 outranks ALNK0, so the scan keeps its 100 us pace while a half buffer renders. Nested in ALNK0 it only scans
 * (GPIO + fm1_in, nothing the audio ISR touches) and counts ms; the USB poll waits for the first tick after the
 * render, and the time spent nested is handed to the audio ISR so its load figures stay render-only. The USB audio
 * stream cannot wait for the render (one packet per 1 ms frame): uac_service also runs nested. It touches only EP4
 * and the consumer side of the audio ring, and usb_poll never runs nested, so the two never interleave. */
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    felucca_dbg.timer_irqs++;
    fm1_input_tick();
    {   /* milliseconds from the 24 MHz TIMER4 (robust to a late tick) */
        static uint32_t last, acc;
        acc += t0 - last;
        last = t0;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
    if (usb_due)
        owed |= 1u;                             /* 2 kHz: all USB SIE traffic lives here */
    if (++sub == 10u)
        sub = 0;
    if (felucca_dbg.in_audio) {
        felucca_dbg.nested++;
#if FELUCCA_UAC
        if (usb_due)
            uac_service();
#endif
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
    owed = 0;
}
#endif
extern void isr_timer5(void);

static void timer5_start(void)                 /* OSC /4 = 6 MHz, PRD 600 -> 10 kHz */
{
    fm1_timer5_start(isr_timer5, 4);   /* above ALNK0 (3): the scan nests into the render (see fm1_timer5_irq) */
}

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

#ifndef VT_SIM
static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, UI_CRASH_BG);            /* fixed, outside the palettes */
    draw_text_line(0, 8, 240, &AF_M, "V-TRACK CRASH", UI_CRASH_INK, UI_CRASH_BG, 1);
    hexs(b, c->vec);
    draw_text_line(10, 40, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->pc);
    draw_text_line(10, 60, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->emu);
    draw_text_line(10, 84, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->dbg);
    draw_text_line(10, 102, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->rets);
    draw_text_line(10, 120, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}
#endif

static void enter_uboot(const char *why)       /* mask-ROM UBOOT with RAM intact (debug / stock updater) */
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 110, 240, &AF_M, why, T_THEME, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                      /* intentional reset: not a failed boot */
    fm1_enter_uboot();
}

#if FELUCCA_FLASH
static void flash_boot(void)                    /* flash_ok (storage_hw.c): the expected part answered */
{
    uint32_t f = irq_save();
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());
    irq_restore(f);
    if (!flash_ok)
        return;
    fl_plain_window_init();                     /* flash above 0x9D000 reads as plaintext through XIP */
    smk_trim_load(fm1_xip_ptr(SMK_TRIM_FLASH));   /* the factory key / pad velocity curves (USRTRIM, read only) */
#if FELUCCA_OTA
    ota_boot_cleanup();                         /* staging area left by an update */
#endif
}
#endif

static void vt_boot(void)
{
    if (felucca_dbg.magic != DBG_MAGIC) {
        memset(&felucca_dbg, 0, sizeof felucca_dbg);
        felucca_dbg.magic = DBG_MAGIC;
    }
    felucca_dbg.boots++;
    felucca_dbg.max_us = 0;
    felucca_dbg.in_audio = 0;                   /* .noinit: a reset inside the audio ISR left it set, and TIMER5
                                                   would treat every tick as nested (no USB poll) */
    fm1_input_init();                           /* (the default velocity curves; flash_boot reads the unit's) */
#if FELUCCA_FLASH
    flash_boot();
#endif
    lcd_init();
    app_boot_screen();
    audio_init();
    usb_start();
    timer5_start();
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(430);                          /* the boot screen */
    app_start();
}

static void vt_frame(void)
{
    static uint32_t uboot_t0;
    fm1_wdt_feed();
    usb_retry(fm1_ms);
    if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
        bootguard.pending = 0;
        bootguard.failed = 0;
    }
    if ((fm1_in.buttons & FM1_OCT_BOTH) != FM1_OCT_BOTH)            /* OCTAVE- + OCTAVE+ held 5 s: UBOOT */
        uboot_t0 = fm1_ms;
    else if (fm1_ms - uboot_t0 > 5000u)
        enter_uboot("UBOOT");
#if FELUCCA_OTA
    ota_service();                                  /* M-UPGRADE handshake */
    if (usb.ota_req) {                              /* M-UPGRADE upgrade command */
        usb.ota_req = 0;
        if (flash_ok)
            ota_session();                          /* returns only if nothing was committed */
        lcd_fill(0, 0, 240, 240, T_BG);
        app_start();                                /* (the app draws its screen again) */
    }
#endif
    if (usb.uboot_req)                              /* SysEx F0 22 24 35 7D F7 from the host */
        enter_uboot("UBOOT (USB)");
    felucca_dbg.ui_frames++;
    app_frame();
    padled_flush();                                 /* the pads' colours, between LCD transfers */
}

static void vt_idle(void)
{
    app_idle();
#if FELUCCA_OTA
    ota_service();                                  /* replies without waiting for the next frame */
#endif
}

#ifndef VT_SIM
static void fm1_main(void)
{
    vt_boot();
    for (;;) {
        uint32_t m = fm1_ms;
        vt_frame();
        while (fm1_ms - m < 15u)                    /* ~60 frames/s at most */
            vt_idle();
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
#if FELUCCA_CPU2
    fm1_cpu1_halt();                           /* a second core still running from before this reset (a watchdog, a
                                                * fault, another firmware) would read the RAM cleared below */
#endif
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* flash driver code that must run from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
#endif
