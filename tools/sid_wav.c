/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sid_wav - render a PSID/RSID file to a 16 bit WAV with the ngsid synth.
 *
 *   sid_wav [-s song] [-t seconds] [--ntsc|--pal] [--mono] [--rate N]
 *           [--exact-clock] [--limiter|--no-limiter] [--digi-wrap] in.sid out.wav
 *
 * The synth core outputs unsigned 8 bit with a DC offset (like VacSid); a one-pole
 * high-pass removes it before the conversion to signed 16 bit.
 */
#include "../psid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xFFFF)); put16(f, (unsigned)(v >> 16 & 0xFFFF)); }

int main(int argc, char **argv) {
    int digi_lp = 0, no_burst = 0, mute_mask = 0, digi_gain = 100, filter_clamp = 0, digi_wrap = 0, song = 0, mono = 0, rate = 44100, exact = 0, limiter = 2, pal_override = -1;
    double seconds = 60;
    const char *in = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) song = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ntsc")) pal_override = 0;
        else if (!strcmp(argv[i], "--pal")) pal_override = 1;
        else if (!strcmp(argv[i], "--mono")) mono = 1;
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--exact-clock")) exact = 1;
        else if (!strcmp(argv[i], "--no-limiter")) limiter = 0;
        else if (!strcmp(argv[i], "--limiter")) limiter = 1;
        else if (!strcmp(argv[i], "--digi-wrap")) digi_wrap = 1;
        else if (!strcmp(argv[i], "--filter-clamp")) filter_clamp = 1;
        else if (!strcmp(argv[i], "--no-vol-burst")) no_burst = 1;
        else if (!strcmp(argv[i], "--digi-lp") && i + 1 < argc) digi_lp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mute") && i + 1 < argc) mute_mask = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--digi-gain") && i + 1 < argc) digi_gain = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vbi")) psid_force_vbi = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
    }
    if (!in || !out) {
        fprintf(stderr, "usage: %s [-s song] [-t seconds] [--ntsc|--pal] [--mono] [--rate N] "
                        "[--exact-clock] [--limiter|--no-limiter] [--digi-wrap] in.sid out.wav\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)flen > 0 ? (size_t)flen : 1);
    if (!buf || fread(buf, 1, (size_t)flen, f) != (size_t)flen) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);

    psid_info info;
    int rc = psid_parse(buf, (size_t)flen, &info);
    if (rc) { fprintf(stderr, "%s: %s\n", in, psid_strerror(rc)); return 1; }
    if (!song) song = info.start_song;
    int pal = pal_override >= 0 ? pal_override : (info.clock == 2 ? 0 : 1);

    printf("%s  \"%s\" by %s (%s)\n", info.rsid ? "RSID" : "PSID", info.name, info.author, info.released);
    printf("load $%04X init $%04X play $%04X, song %d/%d, %s\n", info.load, info.init, info.play,
           song, info.songs, pal ? "PAL" : "NTSC");
    if (info.sid2) printf("note: second SID at $%04X is not played yet\n", info.sid2);

    ngsid_config cfg;
    ngsid_config_default(&cfg);
    cfg.sample_rate = rate; cfg.pal = pal; cfg.stereo = !mono; cfg.exact_clock = exact; cfg.limiter = limiter; cfg.digi_wrap = digi_wrap; cfg.filter_clamp = filter_clamp; cfg.digi_gain = digi_gain; cfg.digi_lp = digi_lp;
    ngsid *s = ngsid_create(&cfg);
    c64 *m = c64_create(s, pal);
    if (m) m->no_vol_burst = no_burst;
    if (s) ngsid_set_voice_mute(s, (unsigned)mute_mask);
    if (!s || !m) { fprintf(stderr, "out of memory\n"); return 1; }

    psid_player *pl = psid_player_create(m, s, &info, song, rate, &rc);
    if (!pl) { fprintf(stderr, "init failed: %s\n", psid_strerror(rc)); return 1; }

    int ch = ngsid_channels(s);
    size_t total = (size_t)(seconds * rate);
    uint8_t *raw = (uint8_t *)malloc(total * (size_t)ch);
    if (!raw) { fprintf(stderr, "out of memory\n"); return 1; }
    size_t done = 0;
    while (done < total && !psid_player_error(pl)) {
        size_t n = total - done > 4096 ? 4096 : total - done;
        n = ngsid_render(s, raw + done * (size_t)ch, n);
        if (!n) break;
        done += n;
    }
    if (psid_player_error(pl)) fprintf(stderr, "warning: playback stopped: %s\n", psid_strerror(psid_player_error(pl)));

    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    unsigned long data = (unsigned long)done * (unsigned)ch * 2;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, (unsigned)ch); put32(f, (unsigned long)rate);
    put32(f, (unsigned long)rate * (unsigned)ch * 2); put16(f, (unsigned)ch * 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data);
    double dc[2] = { 128, 128 };
    for (size_t i = 0; i < done; i++)
        for (int c = 0; c < ch; c++) {
            double x = raw[i * (size_t)ch + (size_t)c];
            dc[c] += (x - dc[c]) * 0.0005;
            int v = (int)((x - dc[c]) * 128.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            put16(f, (unsigned)(v & 0xFFFF));
        }
    fclose(f);
    printf("wrote %s: %.1f s, %d Hz, %s, %llu SID writes\n", out, (double)done / rate, rate,
           ch == 2 ? "stereo" : "mono", (unsigned long long)m->sid_writes);

    free(raw); free(buf);
    psid_player_destroy(pl);
    c64_destroy(m);
    ngsid_destroy(s);
    return 0;
}
