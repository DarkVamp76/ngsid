/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ngsid_wav - render SID register logs (or a built-in demo) to a 16-bit WAV file.
 *
 *   ngsid_wav --demo out.wav
 *   ngsid_wav [--ntsc] [--mono] [--rate N] [--exact-clock] [--no-limiter] log.txt out.wav
 *
 * Log format, one write per line ("#" starts a comment):
 *     <frame> <reg> <value>        e.g.   0 0x18 0x0f   (numbers may be decimal or 0x hex)
 * <frame> counts video frames (50/s PAL, 60/s NTSC), <reg> is 0..0x18.
 *
 * The original player outputs unsigned 8 bit data with a DC offset; the DC part is
 * removed here with a one-pole high-pass before conversion to signed 16 bit.
 */
#include "../ngsid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { long frame; unsigned reg; unsigned val; } event_t;

typedef struct {
    ngsid *s;
    event_t *ev;
    size_t n, pos;
    long frame;
    int demo;
} play_t;

static void demo_frame(play_t *p) {
    static const int notes[] = {0, 3, 7, 12, 7, 3}; /* C minor arpeggio */
    static const unsigned f16_c3 = 2278 * 2;        /* roughly C3 on a PAL SID */
    ngsid *s = p->s;
    long f = p->frame;
    if (f == 0) {
        ngsid_write(s, 0x18, 0x1F);
        /* voice 0: pulse lead, voice 1: saw through the filter, voice 2: triangle bass */
        ngsid_write(s, 2, 0x00); ngsid_write(s, 3, 0x08);
        ngsid_write(s, 5, 0x29); ngsid_write(s, 6, 0xA6);
        ngsid_write(s, 12, 0x08); ngsid_write(s, 13, 0x08);
        ngsid_write(s, 14 + 5, 0x1A); ngsid_write(s, 14 + 6, 0xE4);
        ngsid_write(s, 7 + 5, 0x08); ngsid_write(s, 7 + 6, 0xF8);
        ngsid_write(s, 0x17, 0x02); /* route voice 1 into the filter */
    }
    if (f % 25 == 0) { /* every half second */
        int step = (int)(f / 25), n = notes[step % 6] + ((step / 12) % 2 ? 5 : 0);
        double ratio = 1.0;
        for (int i = 0; i < n; i++) ratio *= 1.059463094359;
        unsigned lead = (unsigned)(f16_c3 * 2 * ratio);
        unsigned saw = (unsigned)(f16_c3 * ratio);
        unsigned bass = (unsigned)(f16_c3 / 2 * (step % 6 < 3 ? 1.0 : 1.122462));
        ngsid_write(s, 4, 0x40); ngsid_write(s, 0, lead & 0xFF); ngsid_write(s, 1, lead >> 8);
        ngsid_write(s, 4, 0x41);
        ngsid_write(s, 7 + 4, 0x20); ngsid_write(s, 7, saw & 0xFF); ngsid_write(s, 8, saw >> 8);
        ngsid_write(s, 7 + 4, 0x21);
        ngsid_write(s, 14 + 4, 0x10); ngsid_write(s, 14, bass & 0xFF); ngsid_write(s, 15, bass >> 8);
        ngsid_write(s, 14 + 4, 0x11);
    }
    if (f % 2 == 0) { /* slow cutoff sweep */
        unsigned c = (unsigned)((f * 9) % 1400);
        ngsid_write(s, 0x15, c & 7);
        ngsid_write(s, 0x16, (c >> 3) & 0xFF);
    }
    if (f % 25 == 18) { ngsid_write(s, 4, 0x40); ngsid_write(s, 7 + 4, 0x20); }
}

static void on_frame(void *user) {
    play_t *p = user;
    if (p->demo) {
        demo_frame(p);
    } else {
        while (p->pos < p->n && p->ev[p->pos].frame <= p->frame) {
            ngsid_write(p->s, p->ev[p->pos].reg, (uint8_t)p->ev[p->pos].val);
            p->pos++;
        }
    }
    p->frame++;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xFFFF)); put16(f, (unsigned)(v >> 16)); }

static int load_log(const char *path, play_t *p) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        long fr; unsigned reg, val;
        char a[64], b[64], c[64];
        if (sscanf(line, "%63s %63s %63s", a, b, c) != 3) continue;
        fr = strtol(a, NULL, 0); reg = (unsigned)strtoul(b, NULL, 0); val = (unsigned)strtoul(c, NULL, 0);
        if (p->n == cap) { cap = cap ? cap * 2 : 1024; p->ev = realloc(p->ev, cap * sizeof *p->ev); }
        p->ev[p->n++] = (event_t){fr, reg, val};
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    ngsid_config cfg;
    ngsid_config_default(&cfg);
    play_t p;
    memset(&p, 0, sizeof p);
    const char *in = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo")) p.demo = 1;
        else if (!strcmp(argv[i], "--ntsc")) cfg.pal = 0;
        else if (!strcmp(argv[i], "--mono")) cfg.stereo = 0;
        else if (!strcmp(argv[i], "--exact-clock")) cfg.exact_clock = 1;
        else if (!strcmp(argv[i], "--no-limiter")) cfg.limiter = 0;
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) cfg.sample_rate = atoi(argv[++i]);
        else if (!in && !p.demo) in = argv[i];
        else out = argv[i];
    }
    if (p.demo && !out && in) { out = in; in = NULL; }
    if (!out || (!p.demo && !in)) {
        fprintf(stderr, "usage: %s --demo out.wav\n       %s [--ntsc] [--mono] [--rate N] [--exact-clock] [--no-limiter] log.txt out.wav\n", argv[0], argv[0]);
        return 2;
    }
    if (!p.demo && load_log(in, &p)) return 1;

    p.s = ngsid_create(&cfg);
    if (!p.s) { fprintf(stderr, "cannot create synth (sample rate %d?)\n", cfg.sample_rate); return 1; }
    ngsid_set_frame_callback(p.s, on_frame, &p);

    long frames_total;
    int fps = cfg.pal ? 50 : 60;
    if (p.demo) frames_total = 50L * 24;
    else {
        long last = 0;
        for (size_t i = 0; i < p.n; i++) if (p.ev[i].frame > last) last = p.ev[i].frame;
        frames_total = last + fps; /* one more second for the release tail */
    }
    size_t nsamp = (size_t)((double)frames_total / fps * cfg.sample_rate);
    int ch = ngsid_channels(p.s);
    uint8_t *raw = malloc(nsamp * (size_t)ch);
    ngsid_render(p.s, raw, nsamp);

    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    unsigned long data = (unsigned long)nsamp * (unsigned long)ch * 2;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, (unsigned)ch); put32(f, (unsigned long)cfg.sample_rate);
    put32(f, (unsigned long)cfg.sample_rate * (unsigned long)ch * 2); put16(f, (unsigned)ch * 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data);
    double dc[2] = {0, 0};
    for (size_t i = 0; i < nsamp; i++)
        for (int c = 0; c < ch; c++) {
            double x = raw[i * (size_t)ch + (size_t)c];
            dc[c] += (x - dc[c]) * 0.0005; /* one-pole DC tracker (~14 Hz at 44.1 kHz) */
            int v = (int)((x - dc[c]) * 160.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            put16(f, (unsigned)(v & 0xFFFF));
        }
    fclose(f);
    printf("wrote %s: %.1f s, %d Hz, %s, %s\n", out, (double)nsamp / cfg.sample_rate, cfg.sample_rate,
           ch == 2 ? "stereo" : "mono", cfg.pal ? "PAL" : "NTSC");
    free(raw); free(p.ev);
    ngsid_destroy(p.s);
    return 0;
}
