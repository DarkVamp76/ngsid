/* SPDX-License-Identifier: GPL-3.0-or-later */
/* engine.h - C interface to the playback engine: libsidplayfp (cycle exact 6510/VIC-II/CIA/PLA,
 * PSID and RSID) with reSIDfp as the SID; the ngSid synth runs silently alongside for the displays. */
#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include "ngsid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct engine engine;

typedef struct {
    int pal_override;        /* -1 = from the tune, 1 = PAL, 0 = NTSC */
    int sid_model;           /* -1 = from the tune header, 0 = MOS 6581, 1 = MOS 8580 */
} engine_opts;

/* Load a PSID/RSID image from memory and select `song` (1-based, 0 = default).
 * On failure returns NULL and writes a message to err. */
engine *engine_create(const uint8_t *buf, size_t len, int song, int rate, const ngsid_config *cfg,
                      const engine_opts *opts, char *err, size_t errlen);
void engine_destroy(engine *e);

/* Render `frames` stereo frames of signed 16-bit PCM (L,R interleaved). */
size_t engine_render(engine *e, int16_t *out, size_t frames);

/* Mute voices: bits 0-2 = voices, bit 3 = samples (volume register). */
void engine_mute(engine *e, unsigned mask);

/* The synth that is being played (for visualisation only). */
ngsid *engine_synth(engine *e);

/* 0 while playback is healthy, otherwise a message. */
const char *engine_error(const engine *e);

/* Library version string. */
const char *engine_version(void);

#ifdef __cplusplus
}
#endif

#endif
