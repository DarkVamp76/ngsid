/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-checks for the PSID parser and frame driver, using hand assembled tunes. */
#include "../psid.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) printf("  ok   ");                      \
        else { printf("  FAIL "); failures++; }           \
        printf(__VA_ARGS__);                              \
        printf("\n");                                     \
    } while (0)

static uint8_t file[0x7C + 0x400];

static const uint8_t setup[] = {
    0xA9, 0x0F, 0x8D, 0x18, 0xD4,       /* LDA #$0F  STA $D418 */
    0xA9, 0x00, 0x8D, 0x05, 0xD4,       /* AD = 0 */
    0xA9, 0xF0, 0x8D, 0x06, 0xD4,       /* SR = $F0 */
    0xA9, 0x10, 0x8D, 0x01, 0xD4,       /* freq hi */
    0xA9, 0x08, 0x8D, 0x03, 0xD4,       /* pw hi */
    0xA9, 0x41, 0x8D, 0x04, 0xD4,       /* pulse + gate */
};

static void build(uint16_t play, int rsid) {
    memset(file, 0, sizeof file);
    memcpy(file, rsid ? "RSID" : "PSID", 4);
    file[5] = 2; file[7] = 0x7C;
    file[8] = 0x10; file[9] = 0x00;                 /* load $1000 */
    file[10] = play ? 0x10 : 0x12; file[11] = 0x00; /* init */
    file[12] = (uint8_t)(play >> 8); file[13] = (uint8_t)play;
    file[15] = 3; file[17] = 2;                     /* 3 songs, start song 2 */
    strcpy((char *)file + 22, "Test Tune");
    strcpy((char *)file + 54, "ngsid");
    file[0x76] = 0; file[0x77] = 0x04;              /* flags: clock = PAL */
    uint8_t *d = file + 0x7C;
    /* $1000: init for play-driven mode */
    memcpy(d, setup, sizeof setup);
    d[sizeof setup] = 0x60;
    /* $1100: play: INC $C000 ; RTS */
    static const uint8_t play_code[] = { 0xEE, 0x00, 0xC0, 0x60 };
    memcpy(d + 0x100, play_code, sizeof play_code);
    /* $1200: init for IRQ mode: set vector $0314 -> $1300 then the setup */
    static const uint8_t vec[] = { 0xA9, 0x00, 0x8D, 0x14, 0x03, 0xA9, 0x13, 0x8D, 0x15, 0x03 };
    memcpy(d + 0x200, vec, sizeof vec);
    memcpy(d + 0x200 + sizeof vec, setup, sizeof setup);
    d[0x200 + sizeof vec + sizeof setup] = 0x60;
    /* $1300: ISR: INC $C000 ; LDA $DC0D ; JMP $EA81 */
    static const uint8_t isr[] = { 0xEE, 0x00, 0xC0, 0xAD, 0x0D, 0xDC, 0x4C, 0x81, 0xEA };
    memcpy(d + 0x300, isr, sizeof isr);
}


int main(int argc, char **argv) {
    psid_info info;
    build(0x1100, 0);
    if (argc > 1) {                          /* optional: write the test tune for the CLI smoke test */
        FILE *o = fopen(argv[1], "wb");
        if (o) { fwrite(file, 1, sizeof file, o); fclose(o); }
    }
    CHECK(psid_parse(file, sizeof file, &info) == 0, "parse PSID v2");
    CHECK(info.load == 0x1000 && info.init == 0x1000 && info.play == 0x1100, "addresses");
    CHECK(info.songs == 3 && info.start_song == 2 && info.clock == 1, "songs / start song / PAL flag");
    CHECK(!strcmp(info.name, "Test Tune") && !strcmp(info.author, "ngsid"), "text fields");
    CHECK(info.data_len == 0x400, "payload length %zu", info.data_len);

    uint8_t bad[16] = "XXXX";
    CHECK(psid_parse(bad, sizeof bad, &info) != 0, "garbage rejected");

    /* load address taken from the payload */
    {
        uint8_t f2[0x7C + 6];
        memset(f2, 0, sizeof f2);
        memcpy(f2, "PSID", 4); f2[5] = 2; f2[7] = 0x7C; f2[15] = 1; f2[17] = 1;
        f2[0x7C] = 0x34; f2[0x7D] = 0x12;
        psid_info i2;
        CHECK(psid_parse(f2, sizeof f2, &i2) == 0 && i2.load == 0x1234 && i2.init == 0x1234 && i2.data_len == 4,
              "load address from payload, init defaults to load");
    }

    /* ---- play-driven tune, 2 s PAL ---- */
    ngsid_config cfg;
    ngsid_config_default(&cfg);
    cfg.pal = 1;
    ngsid *s = ngsid_create(&cfg);
    c64 *m = c64_create(s, 1);
    build(0x1100, 0);
    psid_parse(file, sizeof file, &info);
    int err = 0;
    psid_player *pl = psid_player_create(m, s, &info, 2, cfg.sample_rate, &err);
    CHECK(pl && err == 0, "init returns");
    CHECK(c64_peek(m, 0xC000) == 0, "play not yet called");
    int ch = ngsid_channels(s);
    static uint8_t buf[44100 * 2 * 2];
    size_t n = ngsid_render(s, buf, 88200 / 2 * 1);
    n = ngsid_render(s, buf, 44100);
    (void)n; (void)ch;
    int calls = c64_peek(m, 0xC000);
    CHECK(calls >= 98 && calls <= 103, "play called %d times in 2 s (expect ~100; synth renders in 1024-sample blocks)", calls);
    int lo = 255, hi = 0;
    for (size_t i = 0; i < 44100 * (size_t)ch; i++) { if (buf[i] < lo) lo = buf[i]; if (buf[i] > hi) hi = buf[i]; }
    CHECK(hi - lo > 10, "tune is audible (range %d)", hi - lo);

    /* ---- IRQ-driven tune (play = 0): KERNAL default CIA timer, ~60 Hz ---- */
    build(0, 0);
    psid_parse(file, sizeof file, &info);
    CHECK(info.init == 0x1200, "irq tune init address");
    psid_info info2 = info;
    psid_player_destroy(pl);
    pl = psid_player_create(m, s, &info2, 1, cfg.sample_rate, &err);
    CHECK(pl && err == 0, "IRQ-tune init returns");
    ngsid_reset(s);
    ngsid_render(s, buf, 44100);
    ngsid_render(s, buf, 44100);
    calls = c64_peek(m, 0xC000);
    CHECK(calls >= 118 && calls <= 124, "IRQ routine ran %d times in 2 s (expect ~120 at 60 Hz)", calls);

    psid_player_destroy(pl);
    c64_destroy(m);
    ngsid_destroy(s);
    printf(failures ? "FAILED: %d\n" : "all checks passed\n", failures);
    return failures != 0;
}
