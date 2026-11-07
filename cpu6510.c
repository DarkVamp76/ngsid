/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cpu6510.c - see cpu6510.h */
#include "cpu6510.h"

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IZX, M_IZY, M_IND, M_REL, M_COUNT };
enum { K_READ, K_STORE, K_RMW };

static uint8_t modetab[256];
static int modes_ready;

static void init_modes(void) {
    for (int op = 0; op < 256; op++) {
        int col = op & 15, row = op >> 4, odd = row & 1, m = M_IMP;
        switch (col) {
        case 0:  m = odd ? M_REL : (row == 2 ? M_ABS : (row >= 8 ? M_IMM : M_IMP)); break;
        case 1: case 3: m = odd ? M_IZY : M_IZX; break;
        case 2:  m = (row >= 8 && !odd) ? M_IMM : M_IMP; break;
        case 4: case 5: m = odd ? M_ZPX : M_ZP; break;
        case 6: case 7: m = odd ? ((row == 9 || row == 11) ? M_ZPY : M_ZPX) : M_ZP; break;
        case 8:  m = M_IMP; break;
        case 9:  m = odd ? M_ABY : M_IMM; break;
        case 10: m = (!odd && row < 8) ? M_ACC : M_IMP; break;
        case 11: m = odd ? M_ABY : M_IMM; break;
        case 12: m = row == 4 ? M_ABS : row == 6 ? M_IND : odd ? M_ABX : M_ABS; break;
        case 13: m = odd ? M_ABX : M_ABS; break;
        default: m = odd ? ((row == 9 || row == 11) ? M_ABY : M_ABX) : M_ABS; break;
        }
        modetab[op] = (uint8_t)m;
    }
    modes_ready = 1;
}

/* base cycles per [kind][mode]; page-cross penalty is added for K_READ ABX/ABY/IZY */
static const uint8_t cyc_tab[3][M_COUNT] = {
    /* IMP ACC IMM ZP ZPX ZPY ABS ABX ABY IZX IZY IND REL */
    {  2,  2,  2,  3,  4,  4,  4,  4,  4,  6,  5,  5,  2 },
    {  2,  2,  2,  3,  4,  4,  4,  5,  5,  6,  6,  5,  2 },
    {  2,  2,  2,  5,  6,  6,  6,  7,  7,  8,  8,  5,  2 },
};

void cpu6510_init(cpu6510 *c, uint8_t (*rd)(void *, uint16_t),
                  void (*wr)(void *, uint16_t, uint8_t), void *ctx) {
    if (!modes_ready) init_modes();
    c->read = rd; c->write = wr; c->ctx = ctx;
    c->a = c->x = c->y = 0; c->sp = 0xFD; c->p = CPU_I | CPU_U;
    c->pc = 0; c->jammed = 0; c->cycles = 0;
}

#define RD(a)   c->read(c->ctx, (uint16_t)(a))
#define WR(a,v) c->write(c->ctx, (uint16_t)(a), (uint8_t)(v))

void cpu6510_reset(cpu6510 *c) {
    c->sp = 0xFD; c->p = CPU_I | CPU_U; c->jammed = 0;
    c->pc = (uint16_t)(RD(0xFFFC) | (RD(0xFFFD) << 8));
}

static inline void setnz(cpu6510 *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(CPU_N | CPU_Z)) | (v & 0x80) | (v ? 0 : CPU_Z));
}
static inline void push(cpu6510 *c, uint8_t v) { WR(0x100 + c->sp, v); c->sp--; }
static inline uint8_t pop(cpu6510 *c) { c->sp++; return RD(0x100 + c->sp); }

static uint8_t op_asl(cpu6510 *c, uint8_t v) { c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7)); v = (uint8_t)(v << 1); setnz(c, v); return v; }
static uint8_t op_lsr(cpu6510 *c, uint8_t v) { c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1)); v >>= 1; setnz(c, v); return v; }
static uint8_t op_rol(cpu6510 *c, uint8_t v) { uint8_t ci = c->p & CPU_C; c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7)); v = (uint8_t)((v << 1) | ci); setnz(c, v); return v; }
static uint8_t op_ror(cpu6510 *c, uint8_t v) { uint8_t ci = c->p & CPU_C; c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1)); v = (uint8_t)((v >> 1) | (ci << 7)); setnz(c, v); return v; }

static void op_adc(cpu6510 *c, uint8_t b) {
    unsigned a = c->a, ci = c->p & CPU_C, bin = a + b + ci;
    if (c->p & CPU_D) {
        int al = (int)(a & 15) + (b & 15) + (int)ci;
        if (al >= 10) al = ((al + 6) & 15) + 16;
        int t = (int)(a & 0xF0) + (b & 0xF0) + al;
        c->p &= (uint8_t)~(CPU_N | CPU_V | CPU_Z | CPU_C);
        if (!(bin & 0xFF)) c->p |= CPU_Z;
        if (t & 0x80) c->p |= CPU_N;
        if ((a ^ (unsigned)t) & (b ^ (unsigned)t) & 0x80) c->p |= CPU_V;
        if (t >= 0xA0) t += 0x60;
        if (t >= 0x100) c->p |= CPU_C;
        c->a = (uint8_t)t;
    } else {
        c->p &= (uint8_t)~(CPU_N | CPU_V | CPU_Z | CPU_C);
        if (bin > 0xFF) c->p |= CPU_C;
        if ((a ^ bin) & (b ^ bin) & 0x80) c->p |= CPU_V;
        c->a = (uint8_t)bin;
        setnz(c, c->a);
    }
}

static void op_sbc(cpu6510 *c, uint8_t b) {
    unsigned a = c->a, borrow = (c->p & CPU_C) ? 0 : 1;
    int r = (int)a - b - (int)borrow;
    uint8_t rb = (uint8_t)r;
    uint8_t res = rb;
    if (c->p & CPU_D) {
        int al = (int)(a & 15) - (b & 15) - (int)borrow;
        if (al < 0) al = ((al - 6) & 15) - 16;
        int t = (int)(a & 0xF0) - (b & 0xF0) + al;
        if (t < 0) t -= 0x60;
        res = (uint8_t)t;
    }
    c->p &= (uint8_t)~(CPU_N | CPU_V | CPU_Z | CPU_C);
    if (r >= 0) c->p |= CPU_C;
    if ((a ^ b) & (a ^ rb) & 0x80) c->p |= CPU_V;
    setnz(c, rb);
    c->a = res;
}

static void op_cmp(cpu6510 *c, uint8_t r, uint8_t v) {
    c->p = (uint8_t)((c->p & ~CPU_C) | (r >= v ? CPU_C : 0));
    setnz(c, (uint8_t)(r - v));
}

int cpu6510_interrupt(cpu6510 *c, int nmi) {
    if (!nmi && (c->p & CPU_I)) return 0;
    push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc);
    push(c, (uint8_t)((c->p & ~CPU_B) | CPU_U));
    c->p |= CPU_I;
    uint16_t vec = nmi ? 0xFFFA : 0xFFFE;
    c->pc = (uint16_t)(RD(vec) | (RD(vec + 1) << 8));
    c->cycles += 7;
    return 7;
}

int cpu6510_step(cpu6510 *c) {
    uint16_t opaddr = c->pc;
    uint8_t op = RD(c->pc++);
    int mode = modetab[op], kind = K_READ, cross = 0, cyc;
    uint16_t ea = 0, base;
    uint8_t v;

    switch (mode) {
    case M_IMM: ea = c->pc++; break;
    case M_ZP:  ea = RD(c->pc++); break;
    case M_ZPX: ea = (uint8_t)(RD(c->pc++) + c->x); break;
    case M_ZPY: ea = (uint8_t)(RD(c->pc++) + c->y); break;
    case M_ABS: ea = (uint16_t)(RD(c->pc) | (RD(c->pc + 1) << 8)); c->pc += 2; break;
    case M_ABX: base = (uint16_t)(RD(c->pc) | (RD(c->pc + 1) << 8)); c->pc += 2;
                ea = (uint16_t)(base + c->x); cross = (base ^ ea) & 0xFF00; break;
    case M_ABY: base = (uint16_t)(RD(c->pc) | (RD(c->pc + 1) << 8)); c->pc += 2;
                ea = (uint16_t)(base + c->y); cross = (base ^ ea) & 0xFF00; break;
    case M_IZX: { uint8_t z = (uint8_t)(RD(c->pc++) + c->x);
                  ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8)); } break;
    case M_IZY: { uint8_t z = RD(c->pc++);
                  base = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                  ea = (uint16_t)(base + c->y); cross = (base ^ ea) & 0xFF00; } break;
    default: break;
    }

    /* illegal read-modify-write combos: SLO RLA SRE RRA DCP ISC */
    if ((op & 3) == 3 && (op < 0x80 || op >= 0xC0) && (op & 0x1F) != 0x0B) {
        v = RD(ea); WR(ea, v);
        switch (op >> 5) {
        case 0: v = op_asl(c, v); c->a |= v; setnz(c, c->a); break;
        case 1: v = op_rol(c, v); c->a &= v; setnz(c, c->a); break;
        case 2: v = op_lsr(c, v); c->a ^= v; setnz(c, c->a); break;
        case 3: v = op_ror(c, v); op_adc(c, v); break;
        case 6: v = (uint8_t)(v - 1); op_cmp(c, c->a, v); break;
        default: v = (uint8_t)(v + 1); op_sbc(c, v); break;
        }
        WR(ea, v);
        cyc = cyc_tab[K_RMW][mode];
        c->cycles += (uint64_t)cyc;
        return cyc;
    }

    switch (op) {
    /* ---- loads ---- */
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1:
        c->a = RD(ea); setnz(c, c->a); break;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        c->x = RD(ea); setnz(c, c->x); break;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        c->y = RD(ea); setnz(c, c->y); break;
    case 0xA7: case 0xB7: case 0xAF: case 0xBF: case 0xA3: case 0xB3: case 0xAB: /* LAX */
        c->a = c->x = RD(ea); setnz(c, c->a); break;
    /* ---- stores ---- */
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91:
        kind = K_STORE; WR(ea, c->a); break;
    case 0x86: case 0x96: case 0x8E: kind = K_STORE; WR(ea, c->x); break;
    case 0x84: case 0x94: case 0x8C: kind = K_STORE; WR(ea, c->y); break;
    case 0x87: case 0x97: case 0x8F: case 0x83: kind = K_STORE; WR(ea, c->a & c->x); break; /* SAX */
    /* ---- ALU ---- */
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11:
        c->a |= RD(ea); setnz(c, c->a); break;
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31:
        c->a &= RD(ea); setnz(c, c->a); break;
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51:
        c->a ^= RD(ea); setnz(c, c->a); break;
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71:
        op_adc(c, RD(ea)); break;
    case 0xE9: case 0xEB: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1:
        op_sbc(c, RD(ea)); break;
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1:
        op_cmp(c, c->a, RD(ea)); break;
    case 0xE0: case 0xE4: case 0xEC: op_cmp(c, c->x, RD(ea)); break;
    case 0xC0: case 0xC4: case 0xCC: op_cmp(c, c->y, RD(ea)); break;
    case 0x24: case 0x2C:
        v = RD(ea);
        c->p = (uint8_t)((c->p & ~(CPU_N | CPU_V | CPU_Z)) | (v & 0xC0) | ((c->a & v) ? 0 : CPU_Z));
        break;
    /* ---- shifts / inc / dec (memory) ---- */
    case 0x06: case 0x16: case 0x0E: case 0x1E: kind = K_RMW; v = RD(ea); WR(ea, v); WR(ea, op_asl(c, v)); break;
    case 0x26: case 0x36: case 0x2E: case 0x3E: kind = K_RMW; v = RD(ea); WR(ea, v); WR(ea, op_rol(c, v)); break;
    case 0x46: case 0x56: case 0x4E: case 0x5E: kind = K_RMW; v = RD(ea); WR(ea, v); WR(ea, op_lsr(c, v)); break;
    case 0x66: case 0x76: case 0x6E: case 0x7E: kind = K_RMW; v = RD(ea); WR(ea, v); WR(ea, op_ror(c, v)); break;
    case 0xE6: case 0xF6: case 0xEE: case 0xFE: kind = K_RMW; v = RD(ea); WR(ea, v); v++; setnz(c, v); WR(ea, v); break;
    case 0xC6: case 0xD6: case 0xCE: case 0xDE: kind = K_RMW; v = RD(ea); WR(ea, v); v--; setnz(c, v); WR(ea, v); break;
    /* ---- accumulator shifts ---- */
    case 0x0A: c->a = op_asl(c, c->a); break;
    case 0x2A: c->a = op_rol(c, c->a); break;
    case 0x4A: c->a = op_lsr(c, c->a); break;
    case 0x6A: c->a = op_ror(c, c->a); break;
    /* ---- register ops ---- */
    case 0xAA: c->x = c->a; setnz(c, c->x); break;
    case 0xA8: c->y = c->a; setnz(c, c->y); break;
    case 0xBA: c->x = c->sp; setnz(c, c->x); break;
    case 0x8A: c->a = c->x; setnz(c, c->a); break;
    case 0x9A: c->sp = c->x; break;
    case 0x98: c->a = c->y; setnz(c, c->a); break;
    case 0xCA: c->x--; setnz(c, c->x); break;
    case 0x88: c->y--; setnz(c, c->y); break;
    case 0xE8: c->x++; setnz(c, c->x); break;
    case 0xC8: c->y++; setnz(c, c->y); break;
    case 0x18: c->p &= (uint8_t)~CPU_C; break;
    case 0x38: c->p |= CPU_C; break;
    case 0x58: c->p &= (uint8_t)~CPU_I; break;
    case 0x78: c->p |= CPU_I; break;
    case 0xB8: c->p &= (uint8_t)~CPU_V; break;
    case 0xD8: c->p &= (uint8_t)~CPU_D; break;
    case 0xF8: c->p |= CPU_D; break;
    /* ---- stack ---- */
    case 0x48: push(c, c->a); cyc = 3; goto special;
    case 0x08: push(c, (uint8_t)(c->p | CPU_B | CPU_U)); cyc = 3; goto special;
    case 0x68: c->a = pop(c); setnz(c, c->a); cyc = 4; goto special;
    case 0x28: c->p = (uint8_t)((pop(c) & ~CPU_B) | CPU_U); cyc = 4; goto special;
    /* ---- flow ---- */
    case 0x4C: c->pc = ea; cyc = 3; goto special;
    case 0x6C: {
        uint16_t p = (uint16_t)(RD(c->pc) | (RD(c->pc + 1) << 8));
        uint16_t p2 = (uint16_t)((p & 0xFF00) | ((p + 1) & 0x00FF));   /* NMOS page-wrap bug */
        c->pc = (uint16_t)(RD(p) | (RD(p2) << 8));
        cyc = 5; goto special; }
    case 0x20: {
        uint16_t ret = (uint16_t)(c->pc - 1);
        push(c, (uint8_t)(ret >> 8)); push(c, (uint8_t)ret);
        c->pc = ea; cyc = 6; goto special; }
    case 0x60: { uint8_t lo = pop(c), hi = pop(c); c->pc = (uint16_t)((lo | (hi << 8)) + 1); cyc = 6; goto special; }
    case 0x40: { c->p = (uint8_t)((pop(c) & ~CPU_B) | CPU_U); uint8_t lo = pop(c), hi = pop(c);
                 c->pc = (uint16_t)(lo | (hi << 8)); cyc = 6; goto special; }
    case 0x00: {
        c->pc++;
        push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc);
        push(c, (uint8_t)(c->p | CPU_B | CPU_U));
        c->p |= CPU_I;
        c->pc = (uint16_t)(RD(0xFFFE) | (RD(0xFFFF) << 8));
        cyc = 7; goto special; }
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: {
        int8_t off = (int8_t)RD(c->pc++);
        int flag;
        switch (op >> 6) {
        case 0: flag = c->p & CPU_N; break;
        case 1: flag = c->p & CPU_V; break;
        case 2: flag = c->p & CPU_C; break;
        default: flag = c->p & CPU_Z; break;
        }
        int take = (op & 0x20) ? (flag != 0) : (flag == 0);
        cyc = 2;
        if (take) {
            uint16_t np = (uint16_t)(c->pc + off);
            cyc += 1 + (((c->pc ^ np) & 0xFF00) ? 1 : 0);
            c->pc = np;
        }
        goto special; }
    /* ---- stable illegals ---- */
    case 0x0B: case 0x2B:   /* ANC */
        c->a &= RD(ea); setnz(c, c->a); c->p = (uint8_t)((c->p & ~CPU_C) | (c->a >> 7)); break;
    case 0x4B:              /* ALR */
        c->a &= RD(ea); c->a = op_lsr(c, c->a); break;
    case 0x6B:              /* ARR */
        c->a &= RD(ea);
        c->a = (uint8_t)((c->a >> 1) | ((c->p & CPU_C) << 7));
        setnz(c, c->a);
        c->p = (uint8_t)(c->p & ~(CPU_C | CPU_V));
        if (c->a & 0x40) c->p |= CPU_C;
        if (((c->a >> 6) ^ (c->a >> 5)) & 1) c->p |= CPU_V;
        break;
    case 0xCB: {            /* AXS */
        uint8_t m = RD(ea); unsigned t = (unsigned)(c->a & c->x) - m;
        c->p = (uint8_t)((c->p & ~CPU_C) | ((c->a & c->x) >= m ? CPU_C : 0));
        c->x = (uint8_t)t; setnz(c, c->x); break; }
    /* ---- KIL ---- */
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        c->jammed = 1; c->pc = opaddr; cyc = 2; goto special;
    default:
        /* NOP variants and unstable illegals (XAA, LAS, TAS, SHA, SHX, SHY): operand fetch only */
        if (mode != M_IMP && mode != M_ACC && mode != M_REL) (void)RD(ea);
        break;
    }

    cyc = cyc_tab[kind][mode];
    if (kind == K_READ && cross && (mode == M_ABX || mode == M_ABY || mode == M_IZY)) cyc++;
special:
    c->cycles += (uint64_t)cyc;
    return cyc;
}
