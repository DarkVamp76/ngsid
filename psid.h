/* SPDX-License-Identifier: GPL-3.0-or-later */
/* psid.h - PSID/RSID file parser and a frame driver on top of c64.h. */
#ifndef PSID_H
#define PSID_H

#include <stddef.h>
#include <stdint.h>
#include "c64.h"

typedef struct {
    int rsid;                   /* 1 = RSID, 0 = PSID */
    int version;
    uint16_t load, init, play;  /* load address already resolved (from payload if header says 0) */
    int songs, start_song;      /* 1-based */
    uint32_t speed;             /* bit n = 1: song n+1 is CIA timed */
    char name[33], author[33], released[33];
    uint16_t flags;             /* v2+ */
    int clock;                  /* 0 unknown, 1 PAL, 2 NTSC, 3 either */
    int sid_model;              /* 0 unknown, 1 6581, 2 8580, 3 either */
    uint16_t sid2, sid3;        /* second/third SID base address, 0 = none (not played yet) */
    const uint8_t *data;        /* payload, points into the caller's buffer */
    size_t data_len;
} psid_info;

/* Returns 0 on success, otherwise a negative code (see psid_strerror). */
int psid_parse(const uint8_t *buf, size_t len, psid_info *out);
const char *psid_strerror(int code);

/* Reset the machine, load the tune, run init for `song` (1-based).
 * Returns 0 or a negative code. For tunes with play == 0 the CPU is left
 * running an idle loop with interrupts enabled. */
int psid_start(c64 *c, const psid_info *info, int song);

/* Sample-locked player: runs the machine in step with the synth. Every output sample the
 * emulated C64 advances by clock/rate cycles, so SID writes (also fast multispeed IRQs and
 * NMI digi routines) land on the exact sample. PSID tunes with a play address get a call of
 * that routine once per video frame; everything else (play == 0, RSID) runs on real
 * CIA/VIC interrupts. Installs the synth's sample callback. */
/* Set to 1 to ignore the header's CIA speed bits and always time play by the video frame. */
extern int psid_force_vbi;
typedef struct psid_player psid_player;

psid_player *psid_player_create(c64 *c, ngsid *s, const psid_info *info, int song, int sample_rate, int *err);
void psid_player_destroy(psid_player *p);
int psid_player_error(const psid_player *p);         /* 0 while playback is healthy */

#endif
