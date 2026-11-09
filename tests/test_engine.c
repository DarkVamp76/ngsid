/* SPDX-License-Identifier: GPL-3.0-or-later */
/* engine_test - plays small hand made PSID/RSID images through the whole chain
 * (libsidplayfp machine -> ngSid synth) and checks that sound comes out. */
#include "engine.h"
#include "psid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* header + code at $1000 */
static size_t make_image(uint8_t *img, int rsid, unsigned init, unsigned play, const uint8_t *code, size_t n) {
    memset(img, 0, 0x7C);
    memcpy(img, rsid ? "RSID" : "PSID", 4);
    put16(img + 4, 2); put16(img + 6, 0x7C);
    put16(img + 8, 0);                               /* load address in the payload */
    put16(img + 10, init); put16(img + 12, play);
    put16(img + 14, 1); put16(img + 16, 1);
    memcpy(img + 22, "test", 4);
    put16(img + 0x76, 0x0004);                       /* PAL */
    img[0x7C] = 0x00; img[0x7D] = 0x10;              /* load $1000 */
    memcpy(img + 0x7E, code, n);
    return 0x7E + n;
}

static double play_rms(const uint8_t *img, size_t len, double secs, int *peak, const char **err) {
    ngsid_config cfg; ngsid_config_default(&cfg);
    engine_opts o = { -1, -1 };
    char msg[160];
    engine *e = engine_create(img, len, 1, 44100, &cfg, &o, msg, sizeof msg);
    if (!e) { printf("    create failed: %s\n", msg); *err = "create"; return -1; }
    int16_t raw[2048];
    double sum = 0, sq = 0; long cnt = 0; int pk = 0;
    for (int k = 0; k < (int)(secs * 44100 / 1024); k++) {
        engine_render(e, raw, 1024);
        for (int i = 0; i < 2048; i += 2) { sum += raw[i]; sq += (double)raw[i] * raw[i]; cnt++; int a = raw[i] < 0 ? -raw[i] : raw[i]; if (a > pk) pk = a; }
    }
    *err = engine_error(e);
    double m = sum / (double)cnt, v = sq / (double)cnt - m * m;
    engine_destroy(e);
    *peak = pk;
    return v > 0 ? __builtin_sqrt(v) : 0;
}

int main(void) {
    uint8_t img[512];
    const char *err;
    int pk;

    /* PSID: init sets volume, voice 1 pulse A-4 sustain, play does nothing */
    {
        const uint8_t code[] = {
            0xA9, 0x0F, 0x8D, 0x18, 0xD4,            /* init: LDA #$0F STA $D418 */
            0xA9, 0x1C, 0x8D, 0x01, 0xD4,            /*       LDA #$1C STA $D401 */
            0xA9, 0x08, 0x8D, 0x03, 0xD4,            /*       LDA #$08 STA $D403 */
            0xA9, 0x00, 0x8D, 0x05, 0xD4,            /*       AD */
            0xA9, 0xF0, 0x8D, 0x06, 0xD4,            /*       SR */
            0xA9, 0x41, 0x8D, 0x04, 0xD4,            /*       pulse + gate */
            0x60,                                     /*       RTS */
            0x60                                      /* play: RTS (at +0x1F) */
        };
        size_t n = make_image(img, 0, 0x1000, 0x101F, code, sizeof code);
        double r = play_rms(img, n, 2.0, &pk, &err);
        printf("PSID pulse tone\n");
        CHECK(r > 5.0, "audible output (ac rms %.1f)", r);
        CHECK(err == NULL, "no engine error");
    }

    /* RSID: raster interrupt (via the KERNAL vector at $0314) toggles gate-free noise on voice 3 */
    {
        const uint8_t code[] = {
            0x78,                                     /* SEI */
            0xA9, 0x0F, 0x8D, 0x18, 0xD4,            /* volume */
            0xA9, 0xFF, 0x8D, 0x0F, 0xD4,            /* voice 3 freq hi */
            0xA9, 0x00, 0x8D, 0x13, 0xD4,            /* AD */
            0xA9, 0xF0, 0x8D, 0x14, 0xD4,            /* SR */
            0xA9, 0x81, 0x8D, 0x12, 0xD4,            /* noise + gate */
            0x4C, 0x1F, 0x10                          /* JMP * */
        };
        size_t n = make_image(img, 1, 0x1000, 0, code, sizeof code);
        double r = play_rms(img, n, 2.0, &pk, &err);
        printf("RSID noise (main loop, no interrupts)\n");
        CHECK(r > 5.0, "audible output (ac rms %.1f)", r);
        CHECK(err == NULL, "no engine error");
    }

    /* header parser */
    {
        psid_info in;
        const uint8_t code[] = { 0x60 };
        size_t n = make_image(img, 0, 0x1000, 0, code, sizeof code);
        CHECK(psid_parse(img, n, &in) == 0 && in.load == 0x1000 && in.songs == 1, "psid_parse reads a header");
        CHECK(psid_parse((const uint8_t *)"nonsense", 8, &in) != 0, "psid_parse rejects garbage");
    }

    printf(fails ? "engine_test: %d FAILED\n" : "engine_test: all ok\n", fails);
    return fails != 0;
}
