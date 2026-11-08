/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ngSid (NextGenSid) - SID synthesis core modelled on the algorithms of VacSid 0.88
 * (Vacuum / Oliver Blasnik "R0M", DOS).
 *
 * This is a re-implementation of *observed behaviour*,
 * not a copy of the original code. It renders 8-bit unsigned PCM, either mono
 * or "pseudo stereo" (interleaved L,R), in blocks of 1024 samples exactly like
 * the original mixer, including its quirks (OR-combined waveforms, Borland
 * Pascal LCG noise, slew-limiting "filter", block-wise auto gain limiter).
 *
 * The digi channel ($D418 volume samples / NMI digis) is fed by the host layer
 * (see c64.c). Not yet modelled: the second SID at $D420, half-rate mode (AHSM),
 * 16-bit/GUS output paths.
 */
#ifndef NGSID_H
#define NGSID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGSID_BLOCK 1024

typedef struct ngsid ngsid;

/* Called once per emulated video frame (50 Hz PAL / 60 Hz NTSC), sample
 * accurate, from inside render(). The callback usually runs the 6510 play
 * routine, which calls ngsid_write(). */
typedef void (*ngsid_frame_fn)(void *user);

/* Called once per output sample, before the voices are advanced. Used by hosts that
 * run the emulated machine in lock step with the audio (sample accurate SID writes). */
typedef void (*ngsid_sample_fn)(void *user);

typedef struct ngsid_config {
    int sample_rate;      /* Hz, 11025..48000, default 44100                    */
    int pal;              /* 1 = PAL (50 Hz frames), 0 = NTSC (60 Hz)           */
    int stereo;           /* 1 = pseudo stereo, 0 = mono                        */
    int exact_clock;      /* 0 = authentic VacSid pitch (rounded clock, >>12),
                             1 = exact C64 clock                                */
    int limiter;          /* 0 = off, 1 = always on (VacSid), 2 = auto: on once digi is seen (default) */
    int filter_fallback;  /* 1 = force LP+BP when $D418 has no filter mode set
                             (VacSid default)                                   */
    int digi_wrap;        /* 1 = authentic VacSid digi scaling: 8-bit multiply, digi values
                             above 255 (volume > 8 with high nibbles) wrap around and crackle;
                             0 = full 16-bit multiply (default)                 */
    int digi_gain;        /* digi (sample) level in percent, 100 = as computed (default) */
    int digi_lp;          /* one-pole low-pass on the digi channel in Hz, 0 = off (default) */
    int filter_clamp;     /* 1 = saturate filter overflow instead of the original's
                             wrap-around negate (default 0 = original behaviour) */
} ngsid_config;

void ngsid_config_default(ngsid_config *cfg);

ngsid *ngsid_create(const ngsid_config *cfg);
void ngsid_destroy(ngsid *s);
void ngsid_reset(ngsid *s);

void ngsid_set_frame_callback(ngsid *s, ngsid_frame_fn fn, void *user);
void ngsid_set_sample_callback(ngsid *s, ngsid_sample_fn fn, void *user);

/* Digi channel value (VacSid). The host sets it when an NMI routine has written a
 * new volume nibble: value = nibble * main_volume * 2 (0..450). It holds until changed and is
 * added to every output sample (scaled by the master gain). */
void ngsid_set_digi(ngsid *s, unsigned value);

/* Change the callback rate (default 50 Hz PAL / 60 Hz NTSC, VacSid's fixed rates). Needed for
 * tunes that program the CIA timer (multi-speed). Takes effect from the next frame. */
void ngsid_set_frame_rate(ngsid *s, double hz);

/* Write SID register 0x00..0x18 ($D400..$D418). */
void ngsid_write(ngsid *s, unsigned reg, uint8_t val);

/* Read $D41B (OSC3) / $D41C (ENV3). Extension, not present in VacSid 0.88. */
uint8_t ngsid_read(const ngsid *s, unsigned reg);

/* Number of interleaved channels in the output (1 or 2). */
int ngsid_channels(const ngsid *s);

/* ------------------------------------------------------------ introspection (for visualisers) */
typedef struct {
    uint16_t freq;     /* SID frequency register */
    uint16_t pw;       /* 12 bit pulse width register */
    uint8_t ctrl;      /* control register ($D404 style: gate, sync, ring, test, waveform bits) */
    uint8_t level;     /* current envelope level 0..127 */
    uint8_t state;     /* 0 attack, 1 decay/sustain, 2 hold, 3 release */
    uint8_t sustain;   /* sustain level 0..15 */
} ngsid_voice_info;

void ngsid_get_voice_info(const ngsid *s, int voice, ngsid_voice_info *out);
/* Last rendered block of a voice, envelope scaled unsigned samples, NGSID_BLOCK entries. */
const uint8_t *ngsid_voice_wave(const ngsid *s, int voice);
const uint8_t *ngsid_digi_wave(const ngsid *s);              /* last rendered digi block, NGSID_BLOCK entries (0 = silent) */
uint8_t ngsid_shadow_reg(const ngsid *s, unsigned reg);     /* last value written to reg 0..0x18 */
void ngsid_set_vol_mod(ngsid *s, int active, unsigned nibble); /* per-sample voice volume (fast $D418 bursts) */
/* Hold the DAC level of a voice whose waveform was switched off (test-bit / waveform-switching digis):
 * while on, the voice outputs `wave` (8 bit, before the envelope) instead of the oscillator. */
void ngsid_set_hold(ngsid *s, int voice, int on, uint8_t wave);
void ngsid_set_digi_gain(ngsid *s, int percent);            /* live change of cfg.digi_gain */
void ngsid_set_voice_mute(ngsid *s, unsigned mask);         /* bit n set = voice n silent, bit 3 = digi channel */

/* Render `frames` sample frames of unsigned 8-bit PCM (silence = 0x00 here,
 * exactly as VacSid; the sound card driver of the original used unsigned data
 * with a DC offset supplied by the SID volume/digi path). Returns frames. */
size_t ngsid_render(ngsid *s, uint8_t *out, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* NGSID_H */
