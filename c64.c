/* SPDX-License-Identifier: GPL-3.0-or-later */
/* c64.c - see c64.h */
#include "c64.h"

#include <stdlib.h>
#include <string.h>

#define CALL_TRAP 0xFFF0

/* ------------------------------------------------------------------ KERNAL stub */

static void put(uint8_t *k, uint16_t addr, const uint8_t *bytes, size_t n) {
    memcpy(k + (addr - 0xE000), bytes, n);
}

static void kernal_stub(uint8_t *k) {
    memset(k, 0x60, 8192);                      /* unknown KERNAL routine: just RTS */
    static const uint8_t irq_exit[] = { 0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40 };      /* PLA TAY PLA TAX PLA RTI */
    static const uint8_t irq_def[]  = { 0x4C, 0x7E, 0xEA };                         /* JMP $EA7E */
    static const uint8_t irq_ack[]  = { 0xAD, 0x0D, 0xDC };                         /* LDA $DC0D (falls into $EA81) */
    static const uint8_t irq_ent[]  = { 0x48, 0x8A, 0x48, 0x98, 0x48, 0xBA, 0xBD, 0x04, 0x01,
                                        0x29, 0x10, 0xF0, 0x03, 0x6C, 0x16, 0x03, 0x6C, 0x14, 0x03 };
    static const uint8_t nmi_ent[]  = { 0x78, 0x6C, 0x18, 0x03 };                   /* SEI JMP ($0318) */
    static const uint8_t rti[]      = { 0x40 };
    static const uint8_t reset[]    = { 0x4C, 0xE2, 0xFC };                         /* park */
    static const uint8_t vectors[]  = { 0x43, 0xFE, 0xE2, 0xFC, 0x48, 0xFF };
    put(k, 0xEA31, irq_def, sizeof irq_def);
    put(k, 0xEA7E, irq_ack, sizeof irq_ack);
    put(k, 0xEA81, irq_exit, sizeof irq_exit);
    put(k, 0xFE43, nmi_ent, sizeof nmi_ent);
    put(k, 0xFE47, rti, sizeof rti);
    put(k, 0xFEBC, irq_exit, sizeof irq_exit);                                       /* NMI/IRQ exit used by many routines */
    put(k, 0xFCE2, reset, sizeof reset);
    put(k, 0xFF48, irq_ent, sizeof irq_ent);
    put(k, 0xFFFA, vectors, sizeof vectors);
}

/* ------------------------------------------------------------------ CIA */

static uint8_t cia_read(c64_cia *c, unsigned r) {
    switch (r & 15) {
    case 0x0: return (uint8_t)((c->pra & c->ddra) | ~c->ddra);
    case 0x1: return (uint8_t)((c->prb & c->ddrb) | ~c->ddrb);
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return (uint8_t)c->ta;
    case 0x5: return (uint8_t)(c->ta >> 8);
    case 0x6: return (uint8_t)c->tb;
    case 0x7: return (uint8_t)(c->tb >> 8);
    case 0x8: case 0x9: case 0xA: case 0xB: return c->tod[(r & 15) - 8];
    case 0xC: return c->sdr;
    case 0xD: { uint8_t v = c->icr; if (c->icr & c->mask) v |= 0x80; c->icr = 0; return v; }
    case 0xE: return c->cra & (uint8_t)~0x10;
    default:  return c->crb & (uint8_t)~0x10;
    }
}

static void cia_write(c64_cia *c, unsigned r, uint8_t v) {
    switch (r & 15) {
    case 0x0: c->pra = v; break;
    case 0x1: c->prb = v; break;
    case 0x2: c->ddra = v; break;
    case 0x3: c->ddrb = v; break;
    case 0x4: c->ta_latch = (uint16_t)((c->ta_latch & 0xFF00) | v); break;
    case 0x5: c->ta_latch = (uint16_t)((c->ta_latch & 0x00FF) | (v << 8));
              if (!(c->cra & 1)) c->ta = c->ta_latch;
              break;
    case 0x6: c->tb_latch = (uint16_t)((c->tb_latch & 0xFF00) | v); break;
    case 0x7: c->tb_latch = (uint16_t)((c->tb_latch & 0x00FF) | (v << 8));
              if (!(c->crb & 1)) c->tb = c->tb_latch;
              break;
    case 0x8: case 0x9: case 0xA: case 0xB: c->tod[(r & 15) - 8] = v; break;
    case 0xC: c->sdr = v; break;
    case 0xD: if (v & 0x80) c->mask |= (uint8_t)(v & 0x1F); else c->mask &= (uint8_t)~(v & 0x1F); break;
    case 0xE: c->cra = v; if (v & 0x10) c->ta = c->ta_latch; break;
    default:  c->crb = v; if (v & 0x10) c->tb = c->tb_latch; break;
    }
}

/* Advance n cycles. A timer with value N underflows N+1 cycles after (re)load. */
static void cia_tick(c64_cia *c, int n) {
    unsigned ta_events = 0;
    if ((c->cra & 0x21) == 0x01) {              /* running, counting phi2 */
        unsigned k = (unsigned)n;
        while (k) {
            if (k <= c->ta) { c->ta = (uint16_t)(c->ta - k); break; }
            k -= (unsigned)c->ta + 1;
            c->ta = c->ta_latch;
            c->icr |= 0x01;
            ta_events++;
            if (c->cra & 0x08) { c->cra &= (uint8_t)~1; break; }       /* one-shot */
        }
    }
    if (c->crb & 1) {
        int src = (c->crb >> 5) & 3;
        unsigned k = 0;
        if (src == 0) k = (unsigned)n;
        else if (src == 2) k = ta_events;
        int phi = (src == 0);
        while (k) {
            if (k <= c->tb) { c->tb = (uint16_t)(c->tb - k); break; }
            if (phi) k -= (unsigned)c->tb + 1; else k -= 1 + (k > c->tb ? c->tb : 0);
            c->tb = c->tb_latch;
            c->icr |= 0x02;
            if (c->crb & 0x08) { c->crb &= (uint8_t)~1; break; }
            if (!phi) break;
        }
    }
}

/* ------------------------------------------------------------------ VIC (raster IRQ only) */

static void vic_tick(c64 *c, int n) {
    c->line_cyc += n;
    while (c->line_cyc >= c->cycles_per_line) {
        c->line_cyc -= c->cycles_per_line;
        if (++c->raster_line >= c->lines) c->raster_line = 0;
        if (c->raster_line == c->raster_cmp) c->vic_irq |= 0x01;
    }
}

static int vic_irq_line(const c64 *c) { return (c->vic_irq & c->vic_mask & 0x0F) != 0; }

/* ------------------------------------------------------------------ bus */

static inline unsigned port_cfg(const c64 *c) { return (unsigned)((c->port | ~c->ddr) & 7); }

static uint8_t io_read(c64 *c, uint16_t a) {
    switch (a >> 8) {
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        unsigned r = a & 0x3F;
        switch (r) {
        case 0x11: return (uint8_t)((c->vic[0x11] & 0x7F) | ((c->raster_line >> 1) & 0x80));
        case 0x12: return (uint8_t)c->raster_line;
        case 0x19: return (uint8_t)(c->vic_irq | 0x70 | (vic_irq_line(c) ? 0x80 : 0));
        case 0x1A: return (uint8_t)(c->vic_mask | 0xF0);
        default:   return r >= 0x2F ? 0xFF : c->vic[r];
        }
    }
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
        unsigned r = a & 0x1F;
        if (r == 0x1B || r == 0x1C) return ngsid_read(c->sid, r);
        return (r == 0x19 || r == 0x1A) ? 0xFF : c->sid_bus;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: return (uint8_t)(c->color[a & 0x3FF] | 0xF0);
    case 0xDC: return cia_read(&c->cia[0], a);
    case 0xDD: return cia_read(&c->cia[1], a);
    default:   return 0xFF;
    }
}

static void io_write(c64 *c, uint16_t a, uint8_t v) {
    switch (a >> 8) {
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        unsigned r = a & 0x3F;
        if (r == 0x11) c->raster_cmp = (uint16_t)((c->raster_cmp & 0xFF) | ((v & 0x80) << 1));
        else if (r == 0x12) c->raster_cmp = (uint16_t)((c->raster_cmp & 0x100) | v);
        else if (r == 0x19) { c->vic_irq &= (uint8_t)~(v & 0x0F); return; }
        else if (r == 0x1A) { c->vic_mask = v & 0x0F; return; }
        if (r < 0x2F) c->vic[r] = v;
        break; }
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
        unsigned r = a & 0x1F;
        c->sid_bus = v;
        c->sid_writes++;
        if (r == 0x18 && c->in_nmi) {
            /* VacSid digi: the volume nibble written by an NMI routine is a sample value,
             * played as nibble * main volume * 2; the main volume itself stays untouched. */
            ngsid_write(c->sid, r, (uint8_t)((v & 0xF0) | c->main_vol));
            ngsid_set_digi(c->sid, (unsigned)(v & 0x0F) * c->main_vol * 2u);
        } else if (r == 0x18) {
            /* Several tunes (Arkanoid, ...) toggle the volume nibble every 70..230 cycles from their
             * main loop to use it as an extra oscillator. A block-wise mixer would only ever see the
             * last value of each block, so such bursts are treated as digi samples, too, with the
             * volume from before the burst staying in force. */
            uint64_t dt = c->cycle - c->vol_last;
            c->vol_last = c->cycle;
            if (dt >= 1500) { c->vol_fast = 0; c->vol_prev = c->main_vol; }
            else if (c->vol_fast < 1000) c->vol_fast++;
            if (!c->no_vol_burst && !c->vol_digi && c->vol_fast >= 3) { c->vol_digi = 1; c->vol_base = c->vol_prev; }
            if (c->vol_digi) {
                c->vol_nib = v & 0x0F;
                if (c->vol_mod) ngsid_set_vol_mod(c->sid, 1, v & 0x0F);
                ngsid_write(c->sid, r, (uint8_t)((v & 0xF0) | c->vol_base));
                ngsid_set_digi(c->sid, (unsigned)(v & 0x0F) * c->vol_base * 2u);
            } else {
                c->main_vol = v & 0x0F;
                ngsid_write(c->sid, r, v);
            }
        } else if (r < 0x18) {
            ngsid_write(c->sid, r, v);
        }
        break; }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: c->color[a & 0x3FF] = v & 0x0F; break;
    case 0xDC: cia_write(&c->cia[0], a, v); break;
    case 0xDD: cia_write(&c->cia[1], a, v); break;
    default: break;
    }
}

static uint8_t bus_read(void *ctx, uint16_t a) {
    c64 *c = (c64 *)ctx;
    if (a < 2) return a == 0 ? c->ddr : (uint8_t)((c->port & c->ddr) | (~c->ddr & 0x37));
    unsigned cfg = port_cfg(c);
    if (a >= 0xD000 && a < 0xE000) return ((cfg & 3) && (cfg & 4)) ? io_read(c, a) : c->ram[a];
    if (a >= 0xE000 && (cfg & 2)) return c->kernal[a - 0xE000];
    return c->ram[a];            /* BASIC/char ROM not provided: RAM */
}

static void bus_write(void *ctx, uint16_t a, uint8_t v) {
    c64 *c = (c64 *)ctx;
    if (a < 2) { if (a == 0) c->ddr = v; else c->port = v; return; }
    if (a >= 0xD000 && a < 0xE000) {
        unsigned cfg = port_cfg(c);
        if ((cfg & 3) && (cfg & 4)) { io_write(c, a, v); return; }
    }
    c->ram[a] = v;
}

uint8_t c64_peek(c64 *c, uint16_t a) { return bus_read(c, a); }
void c64_poke(c64 *c, uint16_t a, uint8_t v) { bus_write(c, a, v); }

/* ------------------------------------------------------------------ machine */

c64 *c64_create(ngsid *sid, int pal) {
    c64 *c = (c64 *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->sid = sid;
    c->pal = pal;
    c->cycles_per_line = pal ? 63 : 65;
    c->lines = pal ? 312 : 263;
    cpu6510_init(&c->cpu, bus_read, bus_write, c);
    c64_reset(c);
    return c;
}

void c64_destroy(c64 *c) { free(c); }

void c64_reset(c64 *c) {
    memset(c->ram, 0, sizeof c->ram);
    memset(c->color, 0, sizeof c->color);
    memset(c->vic, 0, sizeof c->vic);
    memset(c->cia, 0, sizeof c->cia);
    c->cia[0].ta_latch = c->cia[0].tb_latch = c->cia[1].ta_latch = c->cia[1].tb_latch = 0xFFFF;
    c->cia[0].ta = c->cia[0].tb = c->cia[1].ta = c->cia[1].tb = 0xFFFF;
    c->ddr = 0x2F; c->port = 0x37;
    c->vic_irq = c->vic_mask = 0;
    c->raster_cmp = c->raster_line = 0; c->line_cyc = 0;
    c->nmi_prev = 0; c->cycle = 0; c->sid_bus = 0; c->sid_writes = 0;
    c->in_nmi = 0; c->nmi_sp = 0; c->main_vol = 15; c->idle_cli = 0;
    c->vol_mod = 1; c->vol_digi = 0; c->vol_fast = 0; c->vol_last = 0; c->vol_prev = 15; c->vol_base = 15; c->vol_nib = 15;
    kernal_stub(c->kernal);
    c->ram[0x0314] = 0x31; c->ram[0x0315] = 0xEA;      /* IRQ  -> $EA31 */
    c->ram[0x0316] = 0x81; c->ram[0x0317] = 0xEA;      /* BRK  -> $EA81 */
    c->ram[0x0318] = 0x47; c->ram[0x0319] = 0xFE;      /* NMI  -> $FE47 */
    c->ram[0x02A6] = (uint8_t)(c->pal ? 1 : 0);
    cpu6510_reset(&c->cpu);
}

static void advance(c64 *c, int n) {
    c->cycle += (uint64_t)n;
    vic_tick(c, n);
    cia_tick(&c->cia[0], n);
    cia_tick(&c->cia[1], n);
}

int c64_step(c64 *c) {
    int cyc;
    if (c->vol_digi && c->cycle - c->vol_last > 3000) {      /* the volume burst is over */
        c->vol_digi = 0;
        c->main_vol = c->vol_nib;
        ngsid_set_digi(c->sid, 0);
        ngsid_set_vol_mod(c->sid, 0, 0);
        ngsid_write(c->sid, 0x18, (uint8_t)((ngsid_shadow_reg(c->sid, 0x18) & 0xF0) | c->main_vol));
    }
    if (c->cpu.pc == C64_IDLE_PC) {
        if (c->idle_cli) c->cpu.p &= (uint8_t)~CPU_I;
        cyc = 3;
        c->cpu.cycles += 3;
    } else {
        cyc = cpu6510_step(&c->cpu);
    }
    if (c->in_nmi && c->cpu.sp >= c->nmi_sp) c->in_nmi = 0;       /* RTI of the NMI routine */
    advance(c, cyc);

    int nmi = (c->cia[1].icr & c->cia[1].mask) != 0;
    if (nmi && !c->nmi_prev) {
        uint8_t sp0 = c->cpu.sp;
        int k = cpu6510_interrupt(&c->cpu, 1);
        if (!c->in_nmi) { c->in_nmi = 1; c->nmi_sp = sp0; }
        advance(c, k); cyc += k;
    }
    c->nmi_prev = nmi;

    int irq = ((c->cia[0].icr & c->cia[0].mask) != 0) || vic_irq_line(c);
    if (irq) { int k = cpu6510_interrupt(&c->cpu, 0); if (k) { advance(c, k); cyc += k; } }
    return cyc;
}

int c64_call(c64 *c, uint16_t addr, uint8_t a, uint32_t max_cycles) {
    uint16_t ret = CALL_TRAP - 1;
    c->ram[0x1FF] = (uint8_t)(ret >> 8);
    c->ram[0x1FE] = (uint8_t)ret;
    c->cpu.sp = 0xFD;
    c->cpu.a = a; c->cpu.x = c->cpu.y = 0;
    c->cpu.pc = addr;
    c->cpu.jammed = 0;
    uint32_t used = 0;
    while (c->cpu.pc != CALL_TRAP) {
        if (used >= max_cycles) return -1;
        used += (uint32_t)c64_step(c);
        if (c->cpu.jammed) return -2;
    }
    return (int)used;
}
