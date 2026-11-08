/* SPDX-License-Identifier: GPL-3.0-or-later */
/* psid.h - PSID/RSID header parser (for the info display; playback is done by libsidplayfp). */
#ifndef PSID_H
#define PSID_H

#include <stddef.h>
#include <stdint.h>

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

#endif
