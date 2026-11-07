/* SPDX-License-Identifier: GPL-3.0-or-later */
/* psid.c - see psid.h */
#include "psid.h"

#include <stdlib.h>
#include <string.h>

enum { E_SHORT = -1, E_MAGIC = -2, E_VERSION = -3, E_OFFSET = -4, E_RANGE = -5, E_INIT = -6, E_TIMEOUT = -7, E_JAM = -8 };

const char *psid_strerror(int code) {
    switch (code) {
    case 0:         return "ok";
    case E_SHORT:   return "file too short";
    case E_MAGIC:   return "not a PSID/RSID file";
    case E_VERSION: return "unsupported version";
    case E_OFFSET:  return "bad data offset";
    case E_RANGE:   return "tune does not fit into 64K";
    case E_INIT:    return "bad song number";
    case E_TIMEOUT: return "init routine did not return";
    case E_JAM:     return "CPU jammed";
    default:        return "error";
    }
}

static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

static void copy_str(char *dst, const uint8_t *src) {
    memcpy(dst, src, 32);
    dst[32] = 0;
}

int psid_parse(const uint8_t *buf, size_t len, psid_info *o) {
    memset(o, 0, sizeof *o);
    if (len < 0x76) return E_SHORT;
    if (!memcmp(buf, "PSID", 4)) o->rsid = 0;
    else if (!memcmp(buf, "RSID", 4)) o->rsid = 1;
    else return E_MAGIC;

    o->version = (int)be16(buf + 4);
    if (o->version < 1 || o->version > 4) return E_VERSION;
    size_t off = be16(buf + 6);
    if (off < 0x76 || off > len) return E_OFFSET;
    if (o->version >= 2 && (off < 0x7C || len < 0x7C)) return E_OFFSET;

    o->load = (uint16_t)be16(buf + 8);
    o->init = (uint16_t)be16(buf + 10);
    o->play = (uint16_t)be16(buf + 12);
    o->songs = (int)be16(buf + 14);
    o->start_song = (int)be16(buf + 16);
    o->speed = (uint32_t)buf[18] << 24 | (uint32_t)buf[19] << 16 | (uint32_t)buf[20] << 8 | buf[21];
    copy_str(o->name, buf + 22);
    copy_str(o->author, buf + 54);
    copy_str(o->released, buf + 86);
    if (o->songs < 1) o->songs = 1;
    if (o->songs > 256) o->songs = 256;
    if (o->start_song < 1 || o->start_song > o->songs) o->start_song = 1;

    if (o->version >= 2) {
        o->flags = (uint16_t)be16(buf + 0x76);
        o->clock = (o->flags >> 2) & 3;
        o->sid_model = (o->flags >> 4) & 3;
        if (o->version >= 3 && buf[0x7A]) o->sid2 = (uint16_t)(0xD000 | buf[0x7A] << 4);
        if (o->version >= 4 && buf[0x7B]) o->sid3 = (uint16_t)(0xD000 | buf[0x7B] << 4);
    }

    const uint8_t *p = buf + off;
    size_t n = len - off;
    if (o->load == 0) {                         /* load address is the first two payload bytes (LE) */
        if (n < 2) return E_SHORT;
        o->load = (uint16_t)(p[0] | p[1] << 8);
        p += 2; n -= 2;
    }
    if (o->init == 0) o->init = o->load;
    if ((size_t)o->load + n > 0x10000) return E_RANGE;
    o->data = p;
    o->data_len = n;
    return 0;
}

/* $01 value used while the tune runs, depending on where its code lives */
static uint8_t bank_for(uint16_t addr) {
    if (addr < 0xA000) return 0x37;
    if (addr < 0xD000) return 0x36;
    return 0x35;                /* RAM everywhere except I/O at $D000-$DFFF */
}

#define IDLE_ADDR C64_IDLE_PC

static uint32_t frame_cycles(const c64 *c) { return (uint32_t)(c->cycles_per_line * c->lines); }

int psid_start(c64 *c, const psid_info *i, int song) {
    if (song < 1 || song > i->songs) return E_INIT;
    c64_reset(c);
    memcpy(&c->ram[i->load], i->data, i->data_len);

    /* power-on state the KERNAL would leave: CIA1 timer A running at ~60 Hz with IRQ enabled */
    uint16_t latch = c->pal ? 0x4025 : 0x4295;
    c->cia[0].ta_latch = c->cia[0].ta = latch;
    c->cia[0].cra = 0x01;
    c->cia[0].mask = 0x01;
    c->vic[0x11] = 0x1B;

    c->idle_cli = (i->play == 0 || i->rsid);        /* IRQ-driven tunes need interrupts on at idle */

    if (i->rsid) {
        /* RSID: the tune is a program for a real C64. init may run forever (main loop), so it is not
         * called like a subroutine: start it with interrupts enabled and let it return into the idle loop. */
        c64_poke(c, 1, 0x37);
        c->ram[0x1FF] = (uint8_t)((IDLE_ADDR - 1) >> 8);
        c->ram[0x1FE] = (uint8_t)(IDLE_ADDR - 1);
        c->cpu.sp = 0xFD;
        c->cpu.a = (uint8_t)(song - 1); c->cpu.x = c->cpu.y = 0;
        c->cpu.p = CPU_U;
        c->cpu.pc = i->init;
        return 0;
    }

    c64_poke(c, 1, bank_for(i->init));
    c->cpu.p |= CPU_I;
    int r = c64_call(c, i->init, (uint8_t)(song - 1), 20000000u);
    if (r == -1) return E_TIMEOUT;
    if (r < 0) return E_JAM;

    c->cpu.pc = IDLE_ADDR;
    return 0;
}

struct psid_player {
    c64 *c;
    const psid_info *info;
    double cps, acc;            /* machine cycles per sample, target cycle count */
    uint64_t next_frame;
    uint32_t frame_cycles;
    int play_mode, err, cia_timed;
};

int psid_force_vbi;

static void player_tick(void *user) {
    psid_player *p = (psid_player *)user;
    c64 *c = p->c;
    if (p->err) return;
    p->acc += p->cps;
    while ((double)c->cycle < p->acc) {
        if (p->play_mode && c->cycle >= p->next_frame && !c->in_nmi &&
            c->cpu.pc == IDLE_ADDR) {
            c64_poke(c, 1, bank_for(p->info->play));
            c->cpu.p |= CPU_I;
            int r = c64_call(c, p->info->play, 0, 1000000u);
            if (r < 0) { p->err = r == -1 ? E_TIMEOUT : E_JAM; return; }
            c->cpu.pc = IDLE_ADDR;
            {   /* speed bit set: the tune times play by CIA1 timer A (re-read, it may be reprogrammed) */
                uint32_t period = p->frame_cycles;
                if (p->cia_timed) {
                    period = (uint32_t)c->cia[0].ta_latch + 1u;
                    if (period < 200) period = 200;
                }
                p->next_frame += period;
            }
            continue;
        }
        c64_step(c);
        if (c->cpu.jammed) { p->err = E_JAM; return; }
    }
}

psid_player *psid_player_create(c64 *c, ngsid *s, const psid_info *info, int song, int sample_rate, int *err) {
    int rc = psid_start(c, info, song);
    if (err) *err = rc;
    if (rc) return NULL;
    psid_player *p = (psid_player *)calloc(1, sizeof *p);
    if (!p) { if (err) *err = -99; return NULL; }
    p->c = c;
    p->info = info;
    p->frame_cycles = frame_cycles(c);
    p->cps = (c->pal ? 985248.0 : 1022727.0) / sample_rate;
    p->acc = (double)c->cycle;
    p->next_frame = c->cycle + p->frame_cycles;
    p->play_mode = info->play != 0 && !info->rsid;
    p->cia_timed = !psid_force_vbi && (info->speed >> (song - 1 > 31 ? 31 : song - 1) & 1u) != 0;
    if (p->cia_timed) {
        uint32_t period = (uint32_t)c->cia[0].ta_latch + 1u;
        if (period < 200) period = 200;
        p->next_frame = c->cycle + period;
    }
    ngsid_set_sample_callback(s, player_tick, p);
    return p;
}

void psid_player_destroy(psid_player *p) { free(p); }
int psid_player_error(const psid_player *p) { return p->err; }
