/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_cpu.c - runs Klaus Dormann's 6502 functional test.
 * Usage: test_cpu 6502_functional_test.bin
 * The image is a 64K RAM dump loaded at $0000, execution starts at $0400,
 * success is the "jmp *" trap at $3469. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cpu6510.h"

static uint8_t ram[65536];

static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return ram[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; ram[a] = v; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s 6502_functional_test.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(ram, 1, sizeof ram, f);
    fclose(f);
    if (n != sizeof ram) { fprintf(stderr, "expected 65536 bytes, got %zu\n", n); return 2; }

    cpu6510 c;
    cpu6510_init(&c, rd, wr, NULL);
    c.pc = 0x0400;
    unsigned long long steps = 0;
    for (;;) {
        uint16_t old = c.pc;
        cpu6510_step(&c);
        steps++;
        if (c.jammed) { printf("JAM at $%04X\n", old); return 1; }
        if (c.pc == old) break;                 /* trap: jmp * */
        if (steps > 400000000ULL) { printf("timeout, pc=$%04X\n", c.pc); return 1; }
    }
    printf("trap at $%04X after %llu instructions, %llu cycles\n", c.pc, steps, (unsigned long long)c.cycles);
    if (c.pc == 0x3469) { printf("6502 functional test: PASS\n"); return 0; }
    printf("6502 functional test: FAIL (test case at $%04X, a=%02X x=%02X y=%02X sp=%02X p=%02X)\n",
           c.pc, c.a, c.x, c.y, c.sp, c.p);
    return 1;
}
