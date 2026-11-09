/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ngsid.c - see ngsid.h
 *
 * Every rule below was read from the behaviour of VACSID.EXE 0.88. Where the
 * original is unclear, the choice is marked with ASSUMPTION.
 */
#include "ngsid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NVOICES 3

/* Decay/release times in seconds (SID datasheet). */
static const double k_times[16] = {
    0.006, 0.024, 0.048, 0.072, 0.114, 0.168, 0.204, 0.240,
    0.300, 0.750, 1.500, 2.400, 3.000, 9.000, 15.000, 24.000};

/* Level decrement per envelope step, indexed by level >> 3. */
static const uint8_t k_decstep[16] = {1, 2, 3, 4, 6, 6, 6, 8, 8, 8, 8, 8, 8, 8, 8, 8};

/* Default per-voice left/right weights: voice0 L77/R50,
 * voice1 L50/R77, voice2 L63/R63 once mapped through the mixer. The first
 * element of each pair feeds the *right* output byte, the second the left. */
static const uint8_t k_amp_default[6] = {0x32, 0x4D, 0x4D, 0x32, 0x3F, 0x3F};
/* order: R1 L1 R2 L2 R3 L3. Hard pan: voice 1 left, voice 2 centre, voice 3 right */

typedef struct {
    uint32_t inc;      /* phase increment, 1/256 cycle * Hz */
    uint32_t rem;      /* fractional remainder of the rate division */
    uint16_t phase;    /* low byte = waveform phase, high byte = cycle count */
    uint16_t pw8;      /* pulse width, 8 bit */
    uint32_t counter;  /* envelope sample counter */
    uint32_t period;   /* current envelope step period (samples) */
    uint32_t att, dec, rel;
    uint8_t sustain;   /* 0..15 */
    uint8_t state;     /* 0 attack, 1 decay, 2 hold, 3 release */
    uint8_t level;     /* 0..127 */
    uint8_t ctrl;      /* shadow of control register */
    uint8_t wrap;      /* sync flag: (old cycle count - new cycle count) */
    uint16_t filt_y;   /* filter state */
    uint32_t seed;     /* Borland Pascal Random() seed, drives the noise */
    uint16_t noise_last;
    uint8_t last_wave; /* for OSC3 readback */
    uint8_t hold_on, hold_wave; /* DAC hold after waveform-off (see ngsid_set_hold) */
    uint16_t hold_cnt, hold_last; /* samples held in the block being rendered / in the last block */
    uint8_t buf[NGSID_BLOCK];
} voice_t;

struct ngsid {
    ngsid_config cfg;
    int rate;
    uint32_t clock_g;      /* VacSid's rounded clock in Hz */
    int64_t frame_inc;     /* frame timer step, 16.16 fixed point Hz (50/60 << 16) */
    int64_t frame_t;       /* frame timer, units of 1/65536 */
    ngsid_frame_fn frame_cb;
    void *frame_user;
    ngsid_sample_fn sample_cb;
    void *sample_user;
    unsigned mute_mask;    /* visualiser/host feature, not in the original */
    uint16_t digi;
    int vmod_on; unsigned vmod_nib;   /* per-sample volume modulation of the voices */
    uint8_t vol_buf[NGSID_BLOCK];
    float digi_lp_y;       /* state of the optional digi low-pass */
    uint8_t digi_buf[NGSID_BLOCK];

    uint8_t shadow[0x20];
    voice_t v[NVOICES];

    uint32_t att_tab[16], dec_tab[16];
    uint16_t coef;         /* filter coefficient */
    uint8_t sid_vol;       /* $D418 low nibble */
    uint8_t master;        /* auto gain */
    uint8_t amp[6];
    uint8_t overload;
    uint8_t digi_seen;     /* digi/volume-burst seen: enables the auto limiter */
    uint16_t hold;
    uint16_t release_int;

    uint8_t *tab;          /* 128*256 table: round(a*b/128) */

    uint8_t outbuf[NGSID_BLOCK * 2];
    size_t out_pos, out_len;
};

/* ---------------------------------------------------------------- helpers */

static uint32_t round_pos(double x) { return (uint32_t)floor(x + 0.5); }

static void build_tables(ngsid *s) {
    for (int a = 0; a < 128; a++)
        for (int b = 0; b < 256; b++)
            s->tab[a * 256 + b] = (uint8_t)round_pos(a * b / 128.0);

    /* attack  = round(t * rate / 1536)   ( = (t/12) * (rate/128) )
     * decay   = round(t * rate * 0.08)   ( = rate/100 * 8 * t   )
     * both at least 1 sample */
    for (int i = 0; i < 16; i++) {
        uint32_t a = round_pos(k_times[i] * s->rate / 1536.0);
        uint32_t d = round_pos(k_times[i] * s->rate * 0.08);
        s->att_tab[i] = a ? a : 1;
        s->dec_tab[i] = d ? d : 1;
    }
}

static void update_freq(ngsid *s, int v) {
    voice_t *o = &s->v[v];
    uint32_t f = ((uint32_t)s->shadow[v * 7 + 1] << 8) | s->shadow[v * 7 + 0];
    if (s->cfg.exact_clock) {
        double clk = s->cfg.pal ? 985248.0 : 1022727.0;
        o->inc = (uint32_t)(clk * f / 65536.0);
    } else {
        /* ((G >> 12) * freq16) >> 4, all integer, as in VACSID.EXE */
        o->inc = ((s->clock_g >> 12) * f) >> 4;
    }
}

static void update_pw(ngsid *s, int v) {
    uint32_t pw = (((uint32_t)(s->shadow[v * 7 + 3] & 0x0F)) << 8) + s->shadow[v * 7 + 2];
    s->v[v].pw8 = (uint16_t)(pw >> 4);
}

/* ASSUMPTION: the original calls a routine (0x1FFB) after AD/SR writes. Its
 * exact job was not read; we re-select the period of the current state and
 * guard against a counter that can never hit the new period. */
static void fix_period(voice_t *o) {
    uint32_t p = o->period;
    switch (o->state) {
    case 0: p = o->att; break;
    case 1: p = o->dec; break;
    case 3: p = o->rel; break;
    default: break;
    }
    o->period = p;
    if (o->counter >= p) o->counter = 0;
}

static void update_cutoff(ngsid *s) {
    uint32_t c = ((uint32_t)s->shadow[0x16] << 3) | (s->shadow[0x15] & 7);
    uint32_t f = ((c << 8) / 45) + 180;
    s->coef = (uint16_t)round_pos(32768.0 * f / s->rate);
}

/* ------------------------------------------------------------------ write */

void ngsid_write(ngsid *s, unsigned reg, uint8_t val) {
    if (reg > 0x18) return;
    s->shadow[reg] = val;

    if (reg < 21) {
        int v = (int)(reg / 7), r = (int)(reg % 7);
        voice_t *o = &s->v[v];
        switch (r) {
        case 0:
        case 1: update_freq(s, v); break;
        case 2:
        case 3: update_pw(s, v); break;
        case 4: {
            uint8_t old = o->ctrl;
            if ((val & 1) != (old & 1)) {
                update_pw(s, v);
                if (val & 1) {
                    o->period = o->att;
                    o->state = 0;
                } else {
                    o->period = o->rel;
                    o->state = 3;
                }
                if (o->counter >= o->period) o->counter = 0;
            }
            o->ctrl = val;
            break;
        }
        case 5:
            o->att = s->att_tab[val >> 4];
            o->dec = s->dec_tab[val & 15];
            fix_period(o);
            break;
        case 6:
            o->rel = s->dec_tab[val & 15];
            o->sustain = val >> 4;
            fix_period(o);
            break;
        }
        return;
    }

    switch (reg) {
    case 0x15:
    case 0x16: update_cutoff(s); break;
    case 0x17: break; /* routing in bits 0..2, resonance in bits 4..7 (unused by VacSid's filter) */
    case 0x18:
        if (s->cfg.filter_fallback && (val >> 4) == 0) s->shadow[0x18] |= 0x30;
        s->sid_vol = val & 0x0F;
        break;
    }
}

uint8_t ngsid_read(const ngsid *s, unsigned reg) {
    if (reg == 0x1B) return s->v[2].last_wave;
    if (reg == 0x1C) return (uint8_t)(s->v[2].level << 1);
    return 0;
}

/* --------------------------------------------------------------- oscillator */

static uint8_t gen_wave(voice_t *o, const voice_t *src) {
    const uint8_t c = o->ctrl;
    const uint8_t p = (uint8_t)(o->phase & 0xFF);
    uint8_t w = 0;

    if (c & 0x80) { /* noise: only if no other waveform bit is set */
        if (c & 0x70) return 0;
        uint16_t nb = (uint16_t)(o->phase >> 4);
        w = (uint8_t)(o->seed >> 16); /* value *before* the seed update */
        if (nb != o->noise_last) {
            o->noise_last = nb;
            o->seed = o->seed * 134775813u + 1u; /* Borland Pascal Random() */
        }
        return w;
    }
    if (c & 0x40) w |= (o->pw8 >= p) ? 0xFF : 0x00;
    if (c & 0x20) w |= p;
    if (c & 0x10) {
        uint8_t t = (uint8_t)(p << 1);
        if (p & 0x80) t = (uint8_t)(0xFE - t);
        if ((c & 0x04) && (((src->phase & 0xFF) + 0x40) & 0x80)) t = (uint8_t)(0xFF - t);
        w |= t;
    }
    return w;
}

static void voice_step(ngsid *s, int v, int i) {
    voice_t *o = &s->v[v];
    voice_t *src = &s->v[(v + 2) % NVOICES]; /* sync/ring source: 0<-2, 1<-0, 2<-1 */
    uint8_t wave = 0;

    if ((o->ctrl & 0x02) && (src->wrap & 1)) { /* hard sync */
        o->phase = 0;
        src->wrap = 0;
    }
    if (o->ctrl & 0x08) { /* test bit */
        o->wrap = 1;
        o->phase = 0;
        wave = 0;
    } else {
        uint32_t acc = o->rem + o->inc;
        uint32_t q = acc / (uint32_t)s->rate;
        o->rem = acc % (uint32_t)s->rate;
        uint8_t old_hi = (uint8_t)(o->phase >> 8);
        o->phase = (uint16_t)(o->phase + q);
        o->wrap = (uint8_t)(old_hi - (uint8_t)(o->phase >> 8));
        wave = gen_wave(o, src);
    }
    if (o->hold_on) { wave = o->hold_wave; o->hold_cnt++; }
    o->last_wave = wave;

    /* envelope */
    if (o->state == 0) {
        if (++o->counter == o->period) {
            o->counter = 0;
            if (++o->level & 0x80) {
                o->level--;
                o->state = 1;
                o->period = o->dec;
            }
        }
    } else if (o->state != 2) {
        int hold_level = (o->state == 1) ? (o->sustain << 3) : 0;
        if (++o->counter == o->period) {
            o->counter = 0;
            int al = o->level;
            if (al > hold_level) {
                al -= k_decstep[al >> 3];
                if (al < 0) al = 0;
                o->level = (uint8_t)al;
            }
        }
    }
    o->buf[i] = s->tab[((unsigned)o->level << 8) | wave];
}

/* ------------------------------------------------------------------ filter */

/* Slew-rate-limited follower with mode mixing, applied in place to a voice
 * buffer. 16-bit signed arithmetic is emulated on purpose (the original
 * overflows for high cutoff at low sample rates). */
static void filter_voice(ngsid *s, int v, int n) {
    voice_t *o = &s->v[v];
    uint8_t peak = 0;
    for (int i = 0; i < n; i++)
        if (o->buf[i] > peak) peak = o->buf[i];
    uint32_t pk = (uint32_t)(peak >> 2);
    int16_t step = (int16_t)(((uint32_t)s->coef * pk) >> 6);
    int16_t y = (int16_t)o->filt_y;
    const int mode = s->shadow[0x18] & 0x70;

    for (int i = 0; i < n; i++) {
        int16_t x = (int16_t)(((uint16_t)o->buf[i] << 8) >> 2);
        int16_t d = (int16_t)(x - y);
        if (d < 0) d = (int16_t)-d;
        int16_t cx = step;

        if (x != y) {
            if (x > y) {
                if (!(d < step)) { cx >>= 1; if (cx == 0) cx = 1; }
                y = (int16_t)(y + cx);
                if (x < y) y = x;
            } else {
                if (!(d < step)) { cx >>= 1; if (cx == 0) cx = 1; }
                y = (int16_t)(y - cx);
                if (y < 0) y = x;
                if (x > y) y = x;
            }
        }
        int16_t hp = (int16_t)(x - y);
        if (hp < 0) hp = (int16_t)-hp;
        uint16_t lp = (uint16_t)y, h = (uint16_t)hp;

        switch (mode) {
        case 0x00: lp = 0; h = 0; break;
        case 0x10: h = 0; break;
        case 0x20: lp >>= 1; break;
        case 0x30: h >>= 2; break;
        case 0x40: lp = 0; break;
        case 0x70: break;
        default: /* 0x50, 0x60 */ h >>= 2; break;
        }
        uint16_t r = (uint16_t)(lp + h) >> 6;
        uint8_t out = (uint8_t)r;
        if (r >> 8) out = s->cfg.filter_clamp ? 255 : (uint8_t)(-out);
        o->buf[i] = out;
    }
    o->filt_y = (uint16_t)y;
}

/* --------------------------------------------------------------------- mix */

static void frame_tick(ngsid *s) {
    s->frame_t -= s->frame_inc;
    if (s->frame_t < 0) {
        s->frame_t += (int64_t)s->rate << 16;
        if (s->frame_cb) s->frame_cb(s->frame_user);
    }
}

static void render_block(ngsid *s) {
    const int n = NGSID_BLOCK;
    const uint8_t *T = s->tab;
    for (int v = 0; v < NVOICES; v++) s->v[v].hold_cnt = 0;

    for (int i = 0; i < n; i++) {
        frame_tick(s);
        if (s->sample_cb) s->sample_cb(s->sample_user);
        /* The original does an 8-bit MUL (only the low byte of the digi value takes part), which
         * wraps for digi values above 255. Default here: proper 16-bit multiply with clipping. */
        unsigned dg = s->cfg.digi_wrap ? (unsigned)(s->digi & 0xFF) : (unsigned)s->digi;
        unsigned db = (((dg * s->master) >> 8) * (unsigned)s->cfg.digi_gain) / 100u;
        if (db > 255) db = 255;
        if (s->cfg.digi_lp > 0) {   /* optional: the C64's analogue output stage rounds off the 4 bit steps */
            float a = 1.0f - expf(-6.2831853f * (float)s->cfg.digi_lp / (float)s->rate);
            s->digi_lp_y += a * ((float)db - s->digi_lp_y);
            db = (unsigned)(s->digi_lp_y + 0.5f);
        }
        s->digi_buf[i] = (uint8_t)db;
        s->vol_buf[i] = (uint8_t)(s->vmod_on ? s->vmod_nib : s->sid_vol);
        for (int v = 0; v < NVOICES; v++) voice_step(s, v, i);
    }
    for (int v = 0; v < NVOICES; v++) s->v[v].hold_last = s->v[v].hold_cnt;
    if (s->mute_mask) {
        for (int v = 0; v < NVOICES; v++)
            if (s->mute_mask & (1u << v)) memset(s->v[v].buf, 0, sizeof s->v[v].buf);
        if (s->mute_mask & 8u) memset(s->digi_buf, 0, sizeof s->digi_buf);
    }
    for (int v = 0; v < NVOICES; v++)
        if (s->shadow[0x17] & (1u << v)) filter_voice(s, v, n);

    uint8_t vol[6];
    for (int k = 0; k < 6; k++)
        vol[k] = (uint8_t)((((s->amp[k] * s->master) >> 8) * s->sid_vol) >> 4);

    uint8_t *o = s->outbuf;
    if (!s->cfg.stereo) {
        unsigned dl = ((unsigned)s->master * s->sid_vol) >> 5;
        for (int i = 0; i < n; i++) {
            unsigned sum = T[(dl << 8) | s->v[0].buf[i]] + T[(dl << 8) | s->v[1].buf[i]] +
                           T[(dl << 8) | s->v[2].buf[i]];
            unsigned out = (sum >> 1) + s->digi_buf[i];
            if (out > 255) out = 255;
            if (out > 0xD8) s->overload = 2;
            *o++ = (uint8_t)out;
        }
        s->out_len = (size_t)n;
    } else {
        for (int i = 0; i < n; i++) {
            unsigned left = 0, right = 0;
            if (s->vmod_on || s->vol_buf[i] != s->sid_vol) {
                /* volume changed inside the block: apply it per sample */
                for (int k = 0; k < 6; k++)
                    vol[k] = (uint8_t)((((s->amp[k] * s->master) >> 8) * s->vol_buf[i]) >> 4);
            }
            for (int v = 0; v < NVOICES; v++) {
                right += T[((unsigned)vol[2 * v] << 8) | s->v[v].buf[i]];
                left += T[((unsigned)vol[2 * v + 1] << 8) | s->v[v].buf[i]];
            }
            left += s->digi_buf[i];
            right += s->digi_buf[i];
            if (left > 255) left = 255;
            if (right > 255) right = 255;
            if (left > 0xD8) s->overload = 2;
            if (right > 0xD8) s->overload = 2;
            *o++ = (uint8_t)left;
            *o++ = (uint8_t)right;
        }
        s->out_len = (size_t)n * 2;
    }
    s->out_pos = 0;

    if (s->cfg.limiter == 1 || (s->cfg.limiter == 2 && s->digi_seen)) {
        if (s->overload) {
            s->hold = 2;
            /* the original subtracts with 8-bit wrap-around; we clamp at 0 */
            s->master = (s->master > s->overload) ? (uint8_t)(s->master - s->overload) : 0;
            s->overload = 0;
        } else if (s->hold == 0) {
            s->hold = s->release_int;
            if (s->master != 0xFF) s->master++;
        }
        s->hold--;
    }
}

/* ------------------------------------------------------------- public API */

void ngsid_config_default(ngsid_config *c) {
    c->sample_rate = 44100;
    c->pal = 1;
    c->stereo = 1;
    c->exact_clock = 0;
    c->limiter = 2;
    c->filter_fallback = 1;
    c->digi_wrap = 0;
    c->digi_gain = 60;
    c->digi_lp = 0;
}

void ngsid_set_frame_rate(ngsid *s, double hz) {
    if (hz < 1.0) hz = 1.0;
    if (hz > 1000.0) hz = 1000.0;
    s->frame_inc = (int64_t)(hz * 65536.0 + 0.5);
}

void ngsid_set_sample_callback(ngsid *s, ngsid_sample_fn fn, void *user) {
    s->sample_cb = fn;
    s->sample_user = user;
}

void ngsid_set_digi(ngsid *s, unsigned value) { s->digi = (uint16_t)value; if (value) s->digi_seen = 1; }

void ngsid_set_frame_callback(ngsid *s, ngsid_frame_fn fn, void *user) {
    s->frame_cb = fn;
    s->frame_user = user;
}

int ngsid_channels(const ngsid *s) { return s->cfg.stereo ? 2 : 1; }

void ngsid_get_voice_info(const ngsid *s, int voice, ngsid_voice_info *out) {
    memset(out, 0, sizeof *out);
    if (voice < 0 || voice >= NVOICES) return;
    const voice_t *o = &s->v[voice];
    out->freq = (uint16_t)((s->shadow[voice * 7 + 1] << 8) | s->shadow[voice * 7]);
    out->pw = (uint16_t)(((s->shadow[voice * 7 + 3] & 0x0F) << 8) | s->shadow[voice * 7 + 2]);
    out->ctrl = o->ctrl;
    out->level = o->level;
    out->state = o->state;
    out->sustain = o->sustain;
}

const uint8_t *ngsid_voice_wave(const ngsid *s, int voice) {
    return (voice >= 0 && voice < NVOICES) ? s->v[voice].buf : NULL;
}

const uint8_t *ngsid_digi_wave(const ngsid *s) { return s->digi_buf; }

uint8_t ngsid_shadow_reg(const ngsid *s, unsigned reg) { return reg < 0x20 ? s->shadow[reg] : 0; }
void ngsid_set_vol_mod(ngsid *s, int active, unsigned nibble) { s->vmod_on = active; s->vmod_nib = nibble & 15u; }
void ngsid_set_hold(ngsid *s, int voice, int on, uint8_t wave) {
    if (voice < 0 || voice >= NVOICES) return;
    s->v[voice].hold_on = on ? 1 : 0;
    s->v[voice].hold_wave = wave;
}
int ngsid_voice_hold(const ngsid *s, int voice) { return (voice >= 0 && voice < NVOICES) ? s->v[voice].hold_last : 0; }
void ngsid_set_digi_gain(ngsid *s, int percent) { s->cfg.digi_gain = percent < 0 ? 0 : (percent > 400 ? 400 : percent); }
void ngsid_set_voice_mute(ngsid *s, unsigned mask) { s->mute_mask = mask & 15u; }

void ngsid_reset(ngsid *s) {
    memset(s->shadow, 0, sizeof s->shadow);
    memset(s->v, 0, sizeof s->v);
    for (int v = 0; v < NVOICES; v++) {
        s->v[v].state = 3; /* release, level 0, as the original init does */
        s->v[v].att = s->att_tab[0];
        s->v[v].dec = s->dec_tab[0];
        s->v[v].rel = s->dec_tab[0];
        s->v[v].period = s->v[v].rel;
        s->v[v].seed = 0x12345678u + (uint32_t)v * 0x01010101u; /* ASSUMPTION: original seed unknown */
    }
    memcpy(s->amp, k_amp_default, sizeof s->amp);
    s->sid_vol = 15;
    s->master = 140;
    s->hold = 20;
    s->release_int = 1;
    s->overload = 0;
    s->digi_seen = 0;
    s->frame_t = 0;
    s->digi = 0;
    s->out_pos = s->out_len = 0;
    update_cutoff(s); /* ASSUMPTION: start with the coefficient of cutoff 0 */
}

ngsid *ngsid_create(const ngsid_config *cfg) {
    ngsid *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (cfg) s->cfg = *cfg; else ngsid_config_default(&s->cfg);
    s->rate = s->cfg.sample_rate;
    if (s->rate < 8000 || s->rate > 400000) { free(s); return NULL; }
    s->clock_g = s->cfg.pal ? 982000u : 1021800u; /* rounded clocks of VacSid */
    s->frame_inc = (int64_t)(s->cfg.pal ? 50 : 60) << 16;
    s->tab = malloc(128 * 256);
    if (!s->tab) { free(s); return NULL; }
    build_tables(s);
    ngsid_reset(s);
    return s;
}

void ngsid_destroy(ngsid *s) {
    if (!s) return;
    free(s->tab);
    free(s);
}

size_t ngsid_render(ngsid *s, uint8_t *out, size_t frames) {
    const size_t ch = (size_t)ngsid_channels(s);
    size_t done = 0;
    while (done < frames) {
        if (s->out_pos >= s->out_len) render_block(s);
        size_t avail = (s->out_len - s->out_pos) / ch;
        size_t take = frames - done < avail ? frames - done : avail;
        memcpy(out + done * ch, s->outbuf + s->out_pos, take * ch);
        s->out_pos += take * ch;
        done += take;
    }
    return frames;
}
