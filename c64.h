/* SPDX-License-Identifier: GPL-3.0-or-later */
/* c64.h - minimal C64 machine for SID playback: 64K RAM, PLA banking ($01),
 * synthetic KERNAL (IRQ/NMI/BRK vector stubs only), VIC raster IRQ, two CIAs
 * (timers A/B, ICR), SID window forwarded to ngsid.
 *
 * Not emulated: BASIC ROM and character ROM (those areas read as RAM),
 * keyboard/joystick, TOD clock, VIC video/badlines/sprites. */
#ifndef C64_H
#define C64_H

#include <stdint.h>
#include "cpu6510.h"
#include "ngsid.h"

typedef struct {
    uint8_t pra, prb, ddra, ddrb;
    uint16_t ta, tb, ta_latch, tb_latch;
    uint8_t cra, crb, icr, mask;
    uint8_t sdr, tod[4];
} c64_cia;

/* Virtual idle loop: while the CPU sits at this address the machine just lets time pass
 * (like a JMP * that lives in no memory, so a tune can never overwrite it). */
#define C64_IDLE_PC 0xFFF2

typedef struct c64 {
    cpu6510 cpu;
    uint8_t ram[65536];
    uint8_t color[1024];
    uint8_t kernal[8192];
    uint8_t ddr, port;              /* 6510 port $00/$01 */
    uint8_t vic[64];
    uint8_t vic_irq, vic_mask;      /* $D019 / $D01A (low 4 bits) */
    uint16_t raster_cmp, raster_line;
    int line_cyc, cycles_per_line, lines;
    c64_cia cia[2];
    int pal, nmi_prev;
    uint8_t sid_bus;                /* last value written to the SID window */
    uint64_t cycle;                 /* total elapsed CPU cycles */
    uint64_t sid_writes;            /* number of writes to the SID window (diagnostics) */
    int idle_cli;                   /* at the idle address: 1 = enable interrupts (CLI), 0 = leave I alone */
    int in_nmi;                     /* an NMI routine is running (VacSid digi mode) */
    uint8_t nmi_sp;                 /* stack pointer before the NMI was taken */
    int no_vol_burst;               /* 1 = old behaviour: fast $D418 writes only set the block volume */
    int vol_mod;                    /* 1 = in a $D418 burst also modulate the voices per sample (default) */
    int vol_digi;                   /* fast $D418 writes outside NMI (Galway volume-register oscillator) */
    int vol_fast;                   /* consecutive $D418 writes closer than 1500 cycles */
    uint64_t vol_last;              /* cycle of the last $D418 write */
    uint8_t vol_base, vol_prev, vol_nib;
    uint8_t main_vol;               /* volume nibble last written outside NMI routines */
    ngsid *sid;                    /* not owned */
} c64;

c64 *c64_create(ngsid *sid, int pal);
void c64_destroy(c64 *c);
void c64_reset(c64 *c);                      /* clears RAM, sets up stub KERNAL, default vectors */

uint8_t c64_peek(c64 *c, uint16_t addr);     /* as the CPU sees it (banking, I/O) */
void c64_poke(c64 *c, uint16_t addr, uint8_t val);

int c64_step(c64 *c);                        /* one instruction + devices + IRQ/NMI; returns cycles */

/* JSR-like call of a routine: A = a, X = Y = 0, returns on its RTS.
 * Returns cycles used, -1 on timeout, -2 if the CPU jammed. */
int c64_call(c64 *c, uint16_t addr, uint8_t a, uint32_t max_cycles);

#endif
