/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sid_play - play a PSID/RSID file by writing raw signed 16 bit little-endian PCM to stdout.
 * Pipe it into any raw PCM player; the player's blocking write paces playback in real time.
 *
 *   sid_play [-o device] [-s song] [-t seconds] [--ntsc|--pal] [--mono] [--rate N]
 *            [--exact-clock] [--limiter|--no-limiter] [--digi-wrap] in.sid | aplay -f S16_LE -r 44100 -c 2 -t raw
 *   sid_play in.sid | pw-cat --playback --format s16 --rate 44100 --channels 2 -
 *
 * -t 0 (default) plays until interrupted. Status text goes to stderr.
 * If stdout is a terminal, the PCM is piped into pw-cat (or aplay/paplay) automatically.
 * -o selects the output device: a PipeWire node id or node.name (see `wpctl status`),
 *    or an ALSA/Pulse device name when aplay/paplay is used.
 */
#define _POSIX_C_SOURCE 200809L
#include "../psid.h"

#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int digi_lp = 0, no_burst = 0, digi_gain = 100, filter_clamp = 0, digi_wrap = 0, song = 0, mono = 0, rate = 44100, exact = 0, limiter = 2, pal_override = -1;
    double seconds = 0;
    const char *in = NULL, *dev = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) song = atoi(argv[++i]);
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
        else if (!strcmp(argv[i], "--digi-gain") && i + 1 < argc) digi_gain = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vbi")) psid_force_vbi = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else if (!in) in = argv[i];
    }
    if (!in) {
        fprintf(stderr, "usage: %s [-o device] [-s song] [-t seconds] [--ntsc|--pal] [--mono] [--rate N] "
                        "[--exact-clock] [--limiter|--no-limiter] [--digi-wrap] in.sid | aplay -f S16_LE -r 44100 -c 2 -t raw\n", argv[0]);
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

    fprintf(stderr, "%s  \"%s\" by %s (%s)\n", info.rsid ? "RSID" : "PSID", info.name, info.author, info.released);
    fprintf(stderr, "song %d/%d, %s, %d Hz %s\n", song, info.songs, pal ? "PAL" : "NTSC", rate, mono ? "mono" : "stereo");

    ngsid_config cfg;
    ngsid_config_default(&cfg);
    cfg.sample_rate = rate; cfg.pal = pal; cfg.stereo = !mono; cfg.exact_clock = exact; cfg.limiter = limiter; cfg.digi_wrap = digi_wrap; cfg.filter_clamp = filter_clamp; cfg.digi_gain = digi_gain; cfg.digi_lp = digi_lp;
    ngsid *s = ngsid_create(&cfg);
    c64 *m = c64_create(s, pal);
    if (m) m->no_vol_burst = no_burst;
    if (!s || !m) { fprintf(stderr, "out of memory\n"); return 1; }

    psid_player *pl = psid_player_create(m, s, &info, song, rate, &rc);
    if (!pl) { fprintf(stderr, "init failed: %s\n", psid_strerror(rc)); return 1; }

    int ch = ngsid_channels(s);
    FILE *outf = stdout;
    int piped = 0;
    if (isatty(fileno(stdout))) {
        char cmd[256];
        if (system("command -v pw-cat >/dev/null 2>&1") == 0) {
            /* newer pw-cat needs --raw for headerless samples on stdin; older versions treat "-" as raw */
            const char *raw = system("pw-cat --help 2>&1 | grep -q -- --raw") == 0 ? "--raw " : "";
            char tgt[160] = "";
            if (dev) snprintf(tgt, sizeof tgt, "--target '%s' ", dev);
            snprintf(cmd, sizeof cmd, "pw-cat --playback %s%s--format s16 --rate %d --channels %d -", tgt, raw, rate, ch);
        } else if (system("command -v aplay >/dev/null 2>&1") == 0)
            snprintf(cmd, sizeof cmd, "aplay -q %s%s%s-f S16_LE -r %d -c %d -t raw", dev ? "-D '" : "", dev ? dev : "", dev ? "' " : "", rate, ch);
        else if (system("command -v paplay >/dev/null 2>&1") == 0)
            snprintf(cmd, sizeof cmd, "paplay --raw %s%s%s--format=s16le --rate=%d --channels=%d", dev ? "--device='" : "", dev ? dev : "", dev ? "' " : "", rate, ch);
        else {
            fprintf(stderr, "stdout is a terminal and no pw-cat, aplay or paplay was found; pipe the output into a raw PCM player\n");
            return 1;
        }
        outf = popen(cmd, "w");
        if (!outf) { fprintf(stderr, "cannot start the audio player\n"); return 1; }
        piped = 1;
    }
    enum { CHUNK = 1024 };
    uint8_t raw[CHUNK * 2];
    int16_t pcm[CHUNK * 2];
    double dc[2] = { 128, 128 };
    size_t total = seconds > 0 ? (size_t)(seconds * rate) : 0, done = 0;

    while (!psid_player_error(pl) && (!total || done < total)) {
        size_t n = ngsid_render(s, raw, CHUNK);
        if (!n) break;
        if (total && done + n > total) n = total - done;
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < ch; c++) {
                double x = raw[i * (size_t)ch + (size_t)c];
                dc[c] += (x - dc[c]) * 0.0005;
                int v = (int)((x - dc[c]) * 128.0);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[i * (size_t)ch + (size_t)c] = (int16_t)v;     /* little-endian host assumed */
            }
        if (fwrite(pcm, sizeof(int16_t) * (size_t)ch, n, outf) != n) break;   /* player closed the pipe */
        done += n;
    }
    fflush(outf);
    if (piped) pclose(outf);
    if (psid_player_error(pl)) fprintf(stderr, "playback stopped: %s\n", psid_strerror(psid_player_error(pl)));

    free(buf);
    psid_player_destroy(pl);
    c64_destroy(m);
    ngsid_destroy(s);
    return 0;
}
