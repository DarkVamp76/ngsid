/* SPDX-License-Identifier: GPL-3.0-or-later */
/* engine.h - C interface to the playback engine: libsidplayfp (cycle exact 6510/VIC-II/CIA/PLA,
 * PSID and RSID) driving the ngSid VacSid style synth. */
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
    int no_vol_burst;        /* 1 = do not treat fast $D418 writes as digi samples */
} engine_opts;

/* Load a PSID/RSID image from memory and select `song` (1-based, 0 = default).
 * On failure returns NULL and writes a message to err. */
engine *engine_create(const uint8_t *buf, size_t len, int song, int rate, const ngsid_config *cfg,
                      const engine_opts *opts, char *err, size_t errlen);
void engine_destroy(engine *e);

/* Render `frames` stereo frames of unsigned 8-bit PCM (L,R interleaved, silence = 0). */
size_t engine_render(engine *e, uint8_t *raw, size_t frames);

/* The synth that is being played (for visualisation and live settings). */
ngsid *engine_synth(engine *e);

/* 0 while playback is healthy, otherwise a message. */
const char *engine_error(const engine *e);

/* Library version string. */
const char *engine_version(void);

#ifdef __cplusplus
}
#endif

#endif
