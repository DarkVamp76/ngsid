/* SPDX-License-Identifier: GPL-3.0-or-later */
/* psid.c - PSID/RSID header parser, see psid.h */
#include "psid.h"

#include <stdlib.h>
#include <string.h>

enum { E_SHORT = -1, E_MAGIC = -2, E_VERSION = -3, E_OFFSET = -4, E_RANGE = -5, E_INIT = -6 };

const char *psid_strerror(int code) {
    switch (code) {
    case 0:         return "ok";
    case E_SHORT:   return "file too short";
    case E_MAGIC:   return "not a PSID/RSID file";
    case E_VERSION: return "unsupported version";
    case E_OFFSET:  return "bad data offset";
    case E_RANGE:   return "tune does not fit into 64K";
    case E_INIT:    return "bad song number";
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

