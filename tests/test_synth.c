/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-checks for ngsid. Each check verifies a rule read from VACSID.EXE. */
#include "../ngsid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) printf("  ok   ");                      \
        else { printf("  FAIL "); failures++; }           \
        printf(__VA_ARGS__);                              \
        printf("\n");                                     \
    } while (0)

static ngsid *make(int stereo, int limiter, int pal) {
    ngsid_config c;
    ngsid_config_default(&c);
    c.stereo = stereo;
    c.limiter = limiter;
    c.pal = pal;
    return ngsid_create(&c);
}

static void voice_setup(ngsid *s, int v, unsigned f16, uint8_t ctrl, uint8_t ad, uint8_t sr) {
    int b = v * 7;
    ngsid_write(s, b + 0, f16 & 0xFF);
    ngsid_write(s, b + 1, f16 >> 8);
    ngsid_write(s, b + 2, 0xFF);
    ngsid_write(s, b + 3, 0x0F); /* pulse width 0xFFF */
    ngsid_write(s, b + 5, ad);
    ngsid_write(s, b + 6, sr);
    ngsid_write(s, b + 4, ctrl);
}

/* 1. pitch: ((G>>12)*f16)>>4 / 256 Hz, measured by counting saw resets */
static void test_pitch(void) {
    printf("pitch\n");
    for (int pal = 0; pal <= 1; pal++) {
        ngsid *s = make(0, 0, pal);
        unsigned f16 = pal ? 7493u : 7032u; /* ~440 Hz */
        ngsid_write(s, 0x18, 0x1F);
        voice_setup(s, 0, f16, 0x21, 0x00, 0xF0);
        enum { N = 44100 };
        uint8_t *buf = malloc(N);
        ngsid_render(s, buf, N);
        int resets = 0;
        for (int i = 1; i < N; i++)
            if ((int)buf[i - 1] - (int)buf[i] > 8) resets++;
        uint32_t g = pal ? 982000u : 1021800u;
        double expect = (double)(((g >> 12) * f16) >> 4) / 256.0;
        CHECK(fabs(resets - expect) <= 2.0, "%s: %d saw periods/s, expected %.1f Hz",
              pal ? "PAL " : "NTSC", resets, expect);
        free(buf);
        ngsid_destroy(s);
    }
}

/* 2. attack: index 8 -> round(0.3*44100/1536)=9 samples per step, 127 steps */
static void test_attack_release(void) {
    printf("envelope\n");
    ngsid *s = make(0, 0, 1);
    ngsid_write(s, 0x18, 0x1F);
    voice_setup(s, 0, 1000, 0x41, 0x80, 0xF0); /* pulse, pw=max -> constant 0xFF, A=8 D=0 S=15 */
    enum { N = 4096 };
    uint8_t buf[N];
    ngsid_render(s, buf, N);
    int first_max = -1, mx = 0;
    for (int i = 0; i < N; i++) if (buf[i] > mx) mx = buf[i];
    for (int i = 0; i < N; i++) if (buf[i] >= mx) { first_max = i; break; }
    int expect = 127 * 9;
    CHECK(abs(first_max - expect) <= 12, "attack idx 8: peak reached at sample %d, expected ~%d", first_max, expect);

    /* release: idx 0 -> dec period round(0.006*44100*0.08)=21, 28 steps from 127 */
    ngsid_write(s, 0x06, 0xF0);
    ngsid_write(s, 0x04, 0x40); /* gate off */
    ngsid_render(s, buf, N);
    int last_nonzero = 0;
    for (int i = 0; i < N; i++) if (buf[i]) last_nonzero = i;
    /* 28 steps of 21 samples; the last two steps (level 1..3) are rounded to 0 by the volume tables */
    CHECK(abs(last_nonzero - 26 * 21) <= 25, "release idx 0: silent after sample %d, expected ~%d", last_nonzero, 26 * 21);
    ngsid_destroy(s);
}

/* 3. combined waveforms are OR-ed, noise alone only */
static void test_waveforms(void) {
    printf("waveforms\n");
    ngsid *s = make(0, 0, 1);
    ngsid_write(s, 0x18, 0x1F);
    voice_setup(s, 0, 3000, 0x81, 0x00, 0xF0); /* noise */
    uint8_t a[2048], b[2048];
    ngsid_render(s, a, 2048);
    int distinct = 0; unsigned seen[256] = {0};
    for (int i = 0; i < 2048; i++) if (!seen[a[i]]++) distinct++;
    /* a single voice at full level has only 65 possible output values (about 6 bit) */
    CHECK(distinct > 40 && distinct <= 65, "noise produces %d distinct values (max 65)", distinct);
    ngsid_write(s, 0x04, 0x91); /* noise + triangle -> silent */
    ngsid_render(s, b, 2048);
    int nz = 0; for (int i = 0; i < 2048; i++) nz += b[i] != 0;
    CHECK(nz == 0, "noise + other waveform is silent (%d non-zero samples)", nz);
    ngsid_destroy(s);
}

/* 4. low-pass reduces high-frequency content, 5. limiter pulls gain down */
static double roughness(const uint8_t *b, int n) {
    double sum = 0;
    for (int i = 1; i < n; i++) sum += fabs((double)b[i] - b[i - 1]);
    return sum / n;
}

static void test_filter_limiter(void) {
    printf("filter / limiter\n");
    /* The "filter" is a slew-rate limiter: it only bites on steep slopes. Use a fast saw
     * (about 1.5 kHz) and the lowest cutoff so the step per sample is small. */
    uint8_t *a = malloc(8192), *b = malloc(8192);
    for (int pass = 0; pass < 2; pass++) {
        ngsid *s = make(0, 0, 1);
        ngsid_write(s, 0x15, 0x00);
        ngsid_write(s, 0x16, 0x00); /* cutoff 0 -> 180 Hz */
        ngsid_write(s, 0x17, pass ? 0x01 : 0x00);
        ngsid_write(s, 0x18, 0x1F);
        voice_setup(s, 0, 25000, 0x21, 0x00, 0xF0);
        ngsid_render(s, pass ? b : a, 8192);
        ngsid_destroy(s);
    }
    double r0 = roughness(a, 8192), r1 = roughness(b, 8192);
    CHECK(r1 < r0 * 0.6, "LP filter (slew limiter): roughness %.2f -> %.2f", r0, r1);
    free(a); free(b);

    /* The "limiter" is an auto gain: below the 0xD8 threshold it raises the master gain by 1 per
     * block, above it lowers it by 2. Three full-level pulse voices sustain at peak 175. */
    enum { BLOCKS = 300 };
    uint8_t *x = malloc((size_t)2 * NGSID_BLOCK * BLOCKS);
    unsigned peak_on = 0, peak_off = 0;
    for (int lim = 1; lim >= 0; lim--) {
        ngsid *s = make(1, lim, 1);
        ngsid_write(s, 0x18, 0x0F);
        for (int v = 0; v < 3; v++) voice_setup(s, v, 3000 + v * 40, 0x41, 0x00, 0xF0);
        ngsid_render(s, x, (size_t)NGSID_BLOCK * BLOCKS);
        unsigned pk = 0;
        for (size_t i = (size_t)2 * NGSID_BLOCK * (BLOCKS - 100); i < (size_t)2 * NGSID_BLOCK * BLOCKS; i++)
            if (x[i] > pk) pk = x[i];
        if (lim) peak_on = pk; else peak_off = pk;
        ngsid_destroy(s);
    }
    CHECK(peak_off == 175, "limiter off: sustained peak stays at %u (level 119 after decay, expected 175)", peak_off);
    CHECK(peak_on >= 200 && peak_on <= 250, "limiter on: gain rises until the 0xD8 threshold, late peak %u", peak_on);
    free(x);
}

int main(void) {
    test_pitch();
    test_attack_release();
    test_waveforms();
    test_filter_limiter();
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures != 0;
}
