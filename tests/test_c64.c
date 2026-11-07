/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-checks for the C64 memory/IO layer. */
#include "../c64.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) printf("  ok   ");                      \
        else { printf("  FAIL "); failures++; }           \
        printf(__VA_ARGS__);                              \
        printf("\n");                                     \
    } while (0)

static void load(c64 *c, uint16_t addr, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) c->ram[addr + i] = p[i];
}

int main(void) {
    ngsid_config cfg;
    ngsid_config_default(&cfg);
    ngsid *sid = ngsid_create(&cfg);
    c64 *c = c64_create(sid, 1);

    /* --- banking --- */
    c->ram[0xE000] = 0xAB; c->ram[0xFFFC] = 0x11;
    c64_poke(c, 1, 0x37);
    CHECK(c64_peek(c, 0xFFFC) == 0xE2, "KERNAL visible at $37 (reset vector low = $E2)");
    CHECK(c64_peek(c, 0xE000) == 0x60, "KERNAL stub bytes at $E000");
    c64_poke(c, 1, 0x35);
    CHECK(c64_peek(c, 0xE000) == 0xAB && c64_peek(c, 0xFFFC) == 0x11, "$35: RAM at $E000, I/O still on");
    c64_poke(c, 1, 0x34);
    c64_poke(c, 0xD020, 0x55);
    CHECK(c->ram[0xD020] == 0x55, "$34: $D000 area is RAM");
    c64_poke(c, 1, 0x35);
    c64_poke(c, 0xD020, 0x06);
    CHECK(c->vic[0x20] == 0x06 && c->ram[0xD020] == 0x55, "$35: $D020 goes to VIC, not RAM");
    c64_poke(c, 1, 0x33);
    CHECK(c64_peek(c, 0xFFFC) == 0xE2 && c64_peek(c, 0xD020) == 0x55, "$33: KERNAL + RAM at $D000 (no I/O)");
    c64_poke(c, 1, 0x35);
    c64_poke(c, 0xD800, 0xFF);
    CHECK(c64_peek(c, 0xD800) == 0xFF && c->color[0] == 0x0F, "color RAM is 4 bit");

    /* --- SID window + mirroring --- */
    ngsid_reset(sid);
    c64_poke(c, 0xD7F8, 0x0F);                  /* $D418 mirror: volume 15 */
    c64_poke(c, 0xD460 + 5, 0x00);              /* reg 5 AD */
    c64_poke(c, 0xD460 + 6, 0xF0);              /* reg 6 SR */
    c64_poke(c, 0xD460 + 1, 0x20);              /* freq hi */
    c64_poke(c, 0xD460 + 4, 0x41);              /* pulse + gate */
    c64_poke(c, 0xD460 + 3, 0x08);              /* pw hi */
    uint8_t buf[4096 * 2];
    size_t n = ngsid_render(sid, buf, 4096) * (size_t)ngsid_channels(sid);
    int lo = 255, hi = 0;
    for (size_t i = 0; i < n; i++) { if (buf[i] < lo) lo = buf[i]; if (buf[i] > hi) hi = buf[i]; }
    CHECK(hi - lo > 10, "SID mirror at $D460/$D7F8 produces sound (range %d)", hi - lo);

    /* --- subroutine call --- */
    static const uint8_t sub[] = { 0xA9, 0x42, 0x8D, 0x00, 0xC3, 0x60 };    /* LDA #$42 STA $C300 RTS */
    load(c, 0xC400, sub, sizeof sub);
    int used = c64_call(c, 0xC400, 0, 1000);
    CHECK(used == 2 + 4 + 6 && c64_peek(c, 0xC300) == 0x42, "c64_call runs routine (%d cycles)", used);

    /* --- CIA1 timer A IRQ through the KERNAL stub --- */
    c64_reset(c);
    c64_poke(c, 1, 0x35);
    static const uint8_t isr[] = { 0xEE, 0x00, 0xC2,            /* INC $C200 */
                                   0xAD, 0x0D, 0xDC,            /* LDA $DC0D (acknowledge) */
                                   0x4C, 0x81, 0xEA };          /* JMP $EA81 */
    static const uint8_t idle[] = { 0x58, 0x4C, 0x01, 0xC0 };   /* CLI ; JMP $C001 */
    load(c, 0xC100, isr, sizeof isr);
    load(c, 0xC000, idle, sizeof idle);
    c64_poke(c, 0x0314, 0x00); c64_poke(c, 0x0315, 0xC1);
    c64_poke(c, 1, 0x37);                                       /* IRQ must go through KERNAL $FF48 */
    c64_poke(c, 0xDC04, 0xE7); c64_poke(c, 0xDC05, 0x03);       /* latch 999 -> period 1000 */
    c64_poke(c, 0xDC0D, 0x81);
    c64_poke(c, 0xDC0E, 0x11);                                  /* force load + start */
    c->cpu.pc = 0xC000;
    while (c->cycle < 5100) c64_step(c);
    CHECK(c64_peek(c, 0xC200) == 5, "CIA1 timer A: %d IRQs in 5100 cycles, period 1000 (expect 5)", c64_peek(c, 0xC200));

    /* --- raster IRQ (PAL: 63 cycles x 312 lines) --- */
    c64_reset(c);
    static const uint8_t risr[] = { 0xEE, 0x00, 0xC2, 0xA9, 0x01, 0x8D, 0x19, 0xD0, 0x40 };   /* ... RTI */
    load(c, 0xC100, risr, sizeof risr);
    load(c, 0xC000, idle, sizeof idle);
    c64_poke(c, 0x0314, 0x00); c64_poke(c, 0x0315, 0xC1);
    c64_poke(c, 1, 0x35);
    c64_poke(c, 0xFFFE, 0x00);                                  /* RAM vector: IRQ -> $C100 directly */
    c64_poke(c, 0xFFFF, 0xC1);
    c64_poke(c, 0xD012, 100); c64_poke(c, 0xD01A, 0x01);
    c->cpu.pc = 0xC000;
    while (c->cycle < 3 * 63 * 312) c64_step(c);
    CHECK(c64_peek(c, 0xC200) == 3, "raster IRQ: %d in 3 frames (expect 3)", c64_peek(c, 0xC200));

    c64_destroy(c);
    ngsid_destroy(sid);
    printf(failures ? "FAILED: %d\n" : "all checks passed\n", failures);
    return failures != 0;
}
