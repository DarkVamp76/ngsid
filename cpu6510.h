/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cpu6510.h - MOS 6510 / NMOS 6502 CPU core (official opcodes, decimal mode,
 * stable illegal opcodes). Memory access via callbacks; the 6510 I/O port
 * ($00/$01) belongs to the memory layer. */
#ifndef CPU6510_H
#define CPU6510_H

#include <stdint.h>

typedef struct cpu6510 cpu6510;

struct cpu6510 {
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    int jammed;                 /* set by KIL opcodes, pc stays on the opcode */
    uint64_t cycles;
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t val);
    void *ctx;
};

enum { CPU_C = 0x01, CPU_Z = 0x02, CPU_I = 0x04, CPU_D = 0x08,
       CPU_B = 0x10, CPU_U = 0x20, CPU_V = 0x40, CPU_N = 0x80 };

void cpu6510_init(cpu6510 *c, uint8_t (*rd)(void *, uint16_t),
                  void (*wr)(void *, uint16_t, uint8_t), void *ctx);
void cpu6510_reset(cpu6510 *c);                 /* loads PC from $FFFC */
int  cpu6510_step(cpu6510 *c);                  /* executes one instruction, returns cycles */
int  cpu6510_interrupt(cpu6510 *c, int nmi);    /* IRQ (maskable) or NMI; returns cycles or 0 */

#endif
