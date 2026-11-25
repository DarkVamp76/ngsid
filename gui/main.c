/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ngSid - OpenGL 3.3 SID player (NextGenSid).
 *
 *   ngSid [options] [file.sid|directory ...]     (no argument: opens the online HVSC browser)
 *
 * Window, input and audio come from SDL2; everything on screen is drawn through one small
 * batched OpenGL renderer (rounded rectangles as signed distance fields, soft glow lines,
 * a baked DejaVu Sans atlas). The graphics API sits behind gfx.h; the OpenGL backend (gfx_gl.c) is loaded at run time via
 * SDL_GL_GetProcAddress, so only libSDL2 is needed at link time.
 *
 * Keys: V orb view, B HVSC online browser (Backspace = folder up; needs curl), Space pause, Left/Right song, Up/Down file, 1-3 mute voice, 4 mute digi, +/- volume,
 *       R restart, F or F11 fullscreen, Esc quit. Mouse: click a voice card to mute it,
 *       click the playlist, drag the volume bar. Drop .sid files or folders onto the window.
 *
 * Options: --pal | --ntsc, --6581 | --8580, --rate N, -s song,
 *          --no-update-check, --version,
 *          --screenshot out.bmp [--frames N]   (render N frames, save, quit; for testing)
 */
#define _DEFAULT_SOURCE
#include "../psid.h"
#include "../lsp/engine.h"
#include "font_data.h"
#include "c64img_data.h"
#include "icon_data.h"
#include "hvsc.h"
#include "gfx.h"
#include "version.h"

#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define NG_GLES 1                      /* OpenGL ES 3: the browser and iOS */
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#endif


static int g_launch_bg;                    /* iOS: the app was started in the background: no tune starts by itself until it comes to the foreground or gets a play command */
#if defined(__APPLE__) && TARGET_OS_IPHONE
/* iOS app glue (ios_glue.m in the private app repo): SDL2 has no safe-area API */
int ios_safe_insets(SDL_Window *win, float *l, float *t, float *r, float *b);
void ios_media_setup(void);                                                       /* lock screen / CarPlay: Now Playing and the remote commands */
void ios_media_update(const char *title, const char *artist, const char *album, int playing, double pos, double duration);
int ios_media_poll(void);                                                         /* a remote command: 0 play, 1 pause, 2 next, 3 previous, -1 none */
int ios_hinge_status(SDL_Window *win);                                       /* ios_glue.m: -1 no hinge (iPhone, iPad), 1 closed, 2 partly open, 3 open (iPhone Duo) */
int ios_launched_in_background(void);                                         /* ios_glue.m: started by the system in the background (CarPlay / lock screen play command): do not play by itself */
int ios_is_testflight(void);                                                   /* ios_glue.m: a TestFlight build (not the App Store): the frame rate is shown */
int ios_app_active(void);                                                        /* ios_glue.m: the app is in the foreground */
void ios_media_set_cover(const unsigned char *px, int w, int h);                 /* the cover for the lock screen: RGBA, opaque, top row first; NULL = none */
void ios_audio_longform(void);                                                    /* AirPlay 2 (HomePod) needs the long-form route sharing policy */
void ios_airplay_place(SDL_Window *win, float x, float y, float size, int visible);   /* the native AirPlay button over the window (points) */
float ios_output_latency_ms(void);                                                /* latency of the output route in ms (AirPlay: ~2000) */
int ios_audio_event_poll(void);                                                   /* 1 = route changed / interruption ended (reopen the output), 2 = pause (fell back to the speaker) */
int ios_music_dir(char *out, int outlen);
static volatile int g_bg;                /* the app is in the background: nothing is drawn (iOS does not allow OpenGL there), the sound goes on */
static int ios_bg_watch(void *ud, SDL_Event *e) {      /* called by SDL on the main thread the moment iOS says so */
    (void)ud;
    if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_DIDENTERBACKGROUND) g_bg = 1;
    else if (e->type == SDL_APP_DIDENTERFOREGROUND) g_bg = 0;
    return 0;
}
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
/* macOS app glue (macos_glue.m in the private app repo): File menu, media keys, Now Playing */
void mac_setup(void);
void mac_media_update(const char *title, const char *artist, const char *album, int playing, double pos, double duration);
int mac_media_poll(void);                                                         /* a remote command: 0 play, 1 pause, 2 next, 3 previous, -1 none */
#endif

/* ================================================================== batched 2D renderer */

typedef struct { float r, g, b, a; } col;
typedef gfx_vtx vtx;

#define MAXV 200000
static vtx *vb;
static int nv;
static int add_blend;
static gfx_tex *fonttex, *c64tex[2];

static float view_w, view_h, view_scale = 1;
static int g_mobile;                  /* touch device (web, iOS): lean mode with fewer additive blend effects */
static int glyph_idx[256];


static void flush(void) {
    if (!nv) return;
    gfx_draw(vb, nv);
    nv = 0;
}

static void set_additive(int on) {
    if (on == add_blend) return;
    flush();
    add_blend = on;
    gfx_set_additive(on);
}

static int g_duo;                      /* iPhone Duo opened: two pages, the list on the left, the player on the right, one wide bar below */
static float g_ox, g_oy;               /* the right page is drawn with this offset in x (the left one at 0); Apple TV: everything is moved by the free margin; elsewhere they stay 0 */
static int tv_inset_px[4];                         /* Apple TV: the free margin in pixels (left, top, right, bottom) */
static float g_tvi_l, g_tvi_t, g_tvi_r, g_tvi_b;   /* Apple TV: the free margin in view units (the background goes to the edge, the cards keep this distance) */
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define GFX_OX g_ox
#define GFX_OY g_oy
#else
#define GFX_OX 0.0f
#define GFX_OY 0.0f
#endif
static void V(float x, float y, float u, float v, col c, float m, float p1, float p2, float p3) {
    if (nv >= MAXV) flush();
    vtx *t = &vb[nv++];
    t->x = x + GFX_OX; t->y = y + GFX_OY; t->u = u; t->v = v;
    t->r = c.r; t->g = c.g; t->b = c.b; t->a = c.a;
    t->m = m; t->p1 = p1; t->p2 = p2; t->p3 = p3;
}

static col C(float r, float g, float b, float a) { col c = { r, g, b, a }; return c; }
static col alpha(col c, float a) { c.a *= a; return c; }
static col mix(col a, col b, float t) {
    return C(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

static void rect_grad(float x, float y, float w, float h, col top, col bot) {
    V(x, y, 0, 0, top, 0, 0, 0, 0); V(x + w, y, 0, 0, top, 0, 0, 0, 0); V(x, y + h, 0, 0, bot, 0, 0, 0, 0);
    V(x + w, y, 0, 0, top, 0, 0, 0, 0); V(x + w, y + h, 0, 0, bot, 0, 0, 0, 0); V(x, y + h, 0, 0, bot, 0, 0, 0, 0);
}
static void rect(float x, float y, float w, float h, col c) { rect_grad(x, y, w, h, c, c); }

/* quad with SDF-local coordinates: uv is the offset from the centre */
static void sdf_quad(float x, float y, float w, float h, float pad, col c, float mode, float p1, float p2, float p3) {
    float hx = w / 2 + pad, hy = h / 2 + pad, cx = x + w / 2, cy = y + h / 2;
    V(cx - hx, cy - hy, -hx, -hy, c, mode, p1, p2, p3); V(cx + hx, cy - hy, hx, -hy, c, mode, p1, p2, p3);
    V(cx - hx, cy + hy, -hx, hy, c, mode, p1, p2, p3);  V(cx + hx, cy - hy, hx, -hy, c, mode, p1, p2, p3);
    V(cx + hx, cy + hy, hx, hy, c, mode, p1, p2, p3);   V(cx - hx, cy + hy, -hx, hy, c, mode, p1, p2, p3);
}
static void rrect(float x, float y, float w, float h, float r, col c) { sdf_quad(x, y, w, h, 1.0f, c, 2, w / 2, h / 2, r); }
static void shadow(float x, float y, float w, float h, float r, float soft, col c) {
    sdf_quad(x, y, w, h, soft, c, 5, w / 2, h / 2, soft + r * 0); (void)r;
}
static void blob(float cx, float cy, float sigma, col c) {
    float e = sigma * 3.2f;
    sdf_quad(cx - e, cy - e, e * 2, e * 2, 0, c, 6, sigma, 0, 0);
}
static void tri(float x0, float y0, float x1, float y1, float x2, float y2, col c) {
    V(x0, y0, 0, 0, c, 0, 0, 0, 0); V(x1, y1, 0, 0, c, 0, 0, 0, 0); V(x2, y2, 0, 0, c, 0, 0, 0, 0);
}

/* textured RGBA picture (premultiplied data, un-multiplied in the shader), centred at cx,cy and turned by ang radians clockwise;
   idx 0 = 6581 C64, 1 = 8580 C64 */
static void draw_c64(int idx, float cx, float cy, float w, float h, float ang, float a) {
    flush();
    gfx_set_texture(c64tex[idx]);
    col c = C(1, 1, 1, a);
    float co = cosf(ang), si = sinf(ang), px[4], py[4];
    static const float dx[4] = { -0.5f, 0.5f, -0.5f, 0.5f }, dy[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
    for (int i = 0; i < 4; i++) { float x = dx[i] * w, y = dy[i] * h; px[i] = cx + x * co - y * si; py[i] = cy + x * si + y * co; }
    V(px[0], py[0], 0, 0, c, 7, 0, 0, 0); V(px[1], py[1], 1, 0, c, 7, 0, 0, 0); V(px[2], py[2], 0, 1, c, 7, 0, 0, 0);
    V(px[1], py[1], 1, 0, c, 7, 0, 0, 0); V(px[3], py[3], 1, 1, c, 7, 0, 0, 0); V(px[2], py[2], 0, 1, c, 7, 0, 0, 0);
    flush();
    gfx_set_texture(fonttex);
}
/* is the point inside the picture turned by ang around cx,cy? */
static int c64_hit(float px, float py, float cx, float cy, float w, float h, float ang) {
    float co = cosf(ang), si = sinf(ang), x = px - cx, y = py - cy;
    float u = x * co + y * si, v = -x * si + y * co;
    return fabsf(u) <= w / 2 && fabsf(v) <= h / 2;
}

/* soft anti-aliased polyline, xy = n points; w half width in px, k = falloff */
static void polyline(const float *xy, int n, float w, float k, col c) {
    if (n < 2) return;
    float px[2] = { 0, 0 };
    float nx[1100], ny[1100];
    if (n > 1100) n = 1100;
    for (int i = 0; i < n; i++) {
        float dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
        if (i > 0) { dx0 = xy[2 * i] - xy[2 * i - 2]; dy0 = xy[2 * i + 1] - xy[2 * i - 1]; }
        if (i < n - 1) { dx1 = xy[2 * i + 2] - xy[2 * i]; dy1 = xy[2 * i + 3] - xy[2 * i + 1]; }
        float l0 = sqrtf(dx0 * dx0 + dy0 * dy0), l1 = sqrtf(dx1 * dx1 + dy1 * dy1);
        float ax = l0 > 1e-6f ? -dy0 / l0 : 0, ay = l0 > 1e-6f ? dx0 / l0 : 0;
        float bx = l1 > 1e-6f ? -dy1 / l1 : 0, by = l1 > 1e-6f ? dx1 / l1 : 0;
        float sx = ax + bx, sy = ay + by, sl = sqrtf(sx * sx + sy * sy);
        if (sl < 1e-4f) { sx = ax + bx; sy = ay + by; if (l1 > 0) { sx = bx; sy = by; } else { sx = ax; sy = ay; } sl = 1; }
        sx /= sl; sy /= sl;
        float d = sx * (l0 > 0 ? ax : bx) + sy * (l0 > 0 ? ay : by);   /* miter length compensation */
        float m = d > 0.5f ? 1.0f / d : 2.0f;
        nx[i] = sx * m; ny[i] = sy * m;
    }
    (void)px;
    for (int i = 0; i < n - 1; i++) {
        float x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * i + 2], y1 = xy[2 * i + 3];
        float a0x = x0 + nx[i] * w, a0y = y0 + ny[i] * w, a1x = x0 - nx[i] * w, a1y = y0 - ny[i] * w;
        float b0x = x1 + nx[i + 1] * w, b0y = y1 + ny[i + 1] * w, b1x = x1 - nx[i + 1] * w, b1y = y1 - ny[i + 1] * w;
        V(a0x, a0y, 0, 1, c, 3, k, 0, 0); V(a1x, a1y, 0, -1, c, 3, k, 0, 0); V(b0x, b0y, 0, 1, c, 3, k, 0, 0);
        V(a1x, a1y, 0, -1, c, 3, k, 0, 0); V(b1x, b1y, 0, -1, c, 3, k, 0, 0); V(b0x, b0y, 0, 1, c, 3, k, 0, 0);
    }
}

/* ---- text (Latin-1 codes) */
static float text_width(const char *s, float size) {
    float w = 0, sc = size / FONT_PX;
    for (; *s; s++) {
        int gi = glyph_idx[(unsigned char)*s];
        if (gi >= 0) w += font_glyphs[gi].adv * sc;
    }
    return w;
}

static float text(float x, float y, float size, col c, const char *s) {
    float sc = size / FONT_PX, x0 = x;
    for (; *s; s++) {
        int gi = glyph_idx[(unsigned char)*s];
        if (gi < 0) continue;
        const font_glyph *g = &font_glyphs[gi];
        if (g->w && g->h) {
            float gx = x + g->xoff * sc, gy = y + g->yoff * sc, gw = g->w * sc, gh = g->h * sc;
            float u0 = (float)g->x / FONT_ATLAS_W, v0 = (float)g->y / FONT_ATLAS_H;
            float u1 = (float)(g->x + g->w) / FONT_ATLAS_W, v1 = (float)(g->y + g->h) / FONT_ATLAS_H;
            V(gx, gy, u0, v0, c, 1, 0, 0, 0); V(gx + gw, gy, u1, v0, c, 1, 0, 0, 0); V(gx, gy + gh, u0, v1, c, 1, 0, 0, 0);
            V(gx + gw, gy, u1, v0, c, 1, 0, 0, 0); V(gx + gw, gy + gh, u1, v1, c, 1, 0, 0, 0); V(gx, gy + gh, u0, v1, c, 1, 0, 0, 0);
        }
        x += g->adv * sc;
    }
    return x - x0;
}

static float text_right(float xr, float y, float size, col c, const char *s) {
    float w = text_width(s, size);
    text(xr - w, y, size, c, s);
    return w;
}

/* UTF-8 -> Latin-1 where possible (file names) */
static void utf8_to_latin1(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        if (*p < 0x80) out[o++] = (char)*p;
        else if ((*p == 0xC2 || *p == 0xC3) && (p[1] & 0xC0) == 0x80) { out[o++] = (char)(((*p & 3) << 6) | (p[1] & 0x3F)); p++; }
        else if (*p >= 0xC0) { out[o++] = '?'; while ((p[1] & 0xC0) == 0x80) p++; }
    }
    out[o] = 0;
}

static void fit_text(char *s, float maxw, float size) {
    if (text_width(s, size) <= maxw) return;
    size_t n = strlen(s);
    while (n > 1) {
        s[--n] = 0;
        char tmp[300];
        snprintf(tmp, sizeof tmp, "%s...", s);
        if (text_width(tmp, size) <= maxw) { strcpy(s, tmp); return; }
    }
}

/* ================================================================== palette */

static const col BG0 = { 0.035f, 0.043f, 0.078f, 1 };
static const col BG1 = { 0.070f, 0.090f, 0.150f, 1 };
static const col CARD = { 0.082f, 0.102f, 0.168f, 1 };
static const col CARDHI = { 0.105f, 0.130f, 0.208f, 1 };
static const col EDGE = { 0.22f, 0.27f, 0.42f, 1 };
static const col TXT = { 0.92f, 0.94f, 1.0f, 1 };
static const col DIM = { 0.52f, 0.58f, 0.74f, 1 };
static const col FAINT = { 0.30f, 0.35f, 0.50f, 1 };
static const col VC[3] = { { 0.24f, 0.88f, 1.0f, 1 }, { 1.0f, 0.36f, 0.80f, 1 }, { 1.0f, 0.76f, 0.28f, 1 } };
static const col OKC = { 0.35f, 0.95f, 0.55f, 1 };
static const col WARN = { 1.0f, 0.45f, 0.35f, 1 };

/* ================================================================== tune + audio */

typedef struct {
    uint8_t *buf;
    size_t len;             /* bytes in buf */
    psid_info info;
    engine *e;
    ngsid *s;               /* synth of SID 1, valid as long as e */
    ngsid *syn[3];          /* synth of each chip (display only) */
    int song, pal;
    int nsid, model[3];     /* chips in use and the model each runs as (6581/8580) */
} tune_t;

typedef struct {
    uint8_t wave[3][3][NGSID_BLOCK];       /* [chip][voice] */
    ngsid_voice_info vi[3][3];
    uint8_t digi[NGSID_BLOCK];             /* digi channel of SID 1 */
    int hold[3];               /* samples of the last block a voice of SID 1 was held (oscillator digi) */
    uint8_t reg[0x20];         /* registers of SID 1 */
    float ring[2048];          /* mono, newest sample last */
    float peak[2];             /* max |sample| per channel since the UI last looked */
    uint64_t frames;
    int running;
} vis_t;

static ngsid_config g_cfg;
static int g_rate = 44100;
static int g_pal_override = -1;
static int g_sid_model = -1;      /* -1 = from the tune */
static unsigned g_mute;
static float g_vol = 0.8f;
static int g_paused;
static tune_t *g_tune;      /* swapped under SDL_LockAudioDevice */
static vis_t g_vis;
static SDL_AudioDeviceID g_dev;
static char g_error[160];

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    static int16_t stage[NGSID_BLOCK * 2];
    static int pos = NGSID_BLOCK, have = NGSID_BLOCK;
    static double dc[2] = { 0, 0 };
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    while (frames > 0) {
        tune_t *t = g_tune;
        if (!t || !t->e || g_paused || engine_error(t->e)) {
            memset(out, 0, (size_t)frames * 4);
            g_vis.running = 0;
            return;
        }
        if (pos >= have) {
            int16_t raw[NGSID_BLOCK * 2];
            size_t n = engine_render(t->e, raw, NGSID_BLOCK);
            have = (int)n;
            pos = 0;
            const float gain = g_vol * 1.6f;
            float pk[2] = { 0, 0 };
            for (int i = 0; i < have; i++) {
                for (int c = 0; c < 2; c++) {
                    double x = raw[i * 2 + c];
                    dc[c] += (x - dc[c]) * 0.0005;      /* tiny offsets of the emulated chip */
                    float a = fabsf((float)((x - dc[c]) * gain / 32768.0));
                    if (a > pk[c]) pk[c] = a;
                    int v = (int)((x - dc[c]) * gain);
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    stage[i * 2 + c] = (int16_t)v;
                }
            }
            /* shift ring and append */
            memmove(g_vis.ring, g_vis.ring + have, (size_t)(2048 - have) * sizeof(float));
            for (int i = 0; i < have; i++) g_vis.ring[2048 - have + i] = ((float)(stage[i * 2] + stage[i * 2 + 1]) / 65536.0f) / (g_vol * 1.6f > 0.01f ? g_vol * 1.6f : 0.01f);
            for (int c = 0; c < 2; c++) if (pk[c] > g_vis.peak[c]) g_vis.peak[c] = pk[c];
            for (int c = 0; c < t->nsid && c < 3; c++) {
                if (!t->syn[c]) continue;
                for (int v = 0; v < 3; v++) {
                    memcpy(g_vis.wave[c][v], ngsid_voice_wave(t->syn[c], v), NGSID_BLOCK);
                    ngsid_get_voice_info(t->syn[c], v, &g_vis.vi[c][v]);
                }
            }
            memcpy(g_vis.digi, ngsid_digi_wave(t->s), NGSID_BLOCK);
            for (int v = 0; v < 3; v++) g_vis.hold[v] = ngsid_voice_hold(t->s, v);
            for (unsigned r = 0; r < 0x20; r++) g_vis.reg[r] = ngsid_shadow_reg(t->s, r);
            g_vis.frames += (uint64_t)have;
            g_vis.running = 1;
            if (!have) { memset(out, 0, (size_t)frames * 4); return; }
        }
        int n = have - pos < frames ? have - pos : frames;
        memcpy(out, stage + pos * 2, (size_t)n * 4);
        out += n * 2; pos += n; frames -= n;
    }
}

/* Mute mask: voice N of every SID, and the digi channel (bit 3). */
static void apply_mute(void) {
    if (!g_tune || !g_tune->e) return;
    for (int i = 0; i < g_tune->nsid && i < 3; i++) if (g_tune->syn[i]) ngsid_set_voice_mute(g_tune->syn[i], g_mute);
    engine_mute(g_tune->e, g_mute);
}

/* ---- NOTES view: the notes run down like the pattern of ngPlayer. A SID tune has no pattern in its file, so a second, silent engine of the same tune runs LA_ROWS rows ahead
 * of what is played and its voice registers are read NT_RATE times a second: the rows below the bar are the notes that are about to sound, the rows above it those that have.
 * A note is shown where a voice starts one (gate on, or another pitch), "===" where the gate goes off, "|" while it is held, "NOI" for noise. */
#define NT_ROWS 64
#define NT_RATE 12.0f
#define LA_ROWS 16
typedef struct { signed char note; unsigned char kind; } nt_cell;      /* kind: 0 empty, 1 note, 2 gate off, 3 noise, 4 held */
static nt_cell nt_cells[9][NT_ROWS];
static int64_t nt_rowid[NT_ROWS];                                      /* the absolute row that a slot holds, -1 = none */
static int nt_pnote[9], nt_pgate[9], nt_pnoise[9];
static engine *la_e; static tune_t *la_tune; static int la_ncols; static uint64_t la_frames; static int64_t la_next_row;
static void la_destroy(void) { if (la_e) engine_destroy(la_e); la_e = NULL; la_tune = NULL; la_frames = 0; la_next_row = 0; }
static int la_create(tune_t *t) {
    la_destroy();
    if (!t || !t->e || !t->buf) return -1;
    ngsid_config cfg = g_cfg; cfg.pal = t->pal; cfg.stereo = 1; cfg.sample_rate = g_rate;
    engine_opts eo; eo.pal_override = g_pal_override; eo.sid_model = g_sid_model;
    char emsg[160] = "";
    la_e = engine_create(t->buf, t->len, t->song, g_rate, &cfg, &eo, emsg, sizeof emsg);
    if (!la_e) return -1;
    la_tune = t; la_ncols = 3 * (int)(engine_sid_count(la_e) > 3 ? 3 : engine_sid_count(la_e)); la_frames = 0; la_next_row = 0;
    memset(nt_cells, 0, sizeof nt_cells); memset(nt_pnote, 0, sizeof nt_pnote); memset(nt_pgate, 0, sizeof nt_pgate); memset(nt_pnoise, 0, sizeof nt_pnoise);
    for (int i = 0; i < NT_ROWS; i++) nt_rowid[i] = -1;
    return 0;
}
static void la_sample(int64_t row) {                                    /* the voices of the look-ahead engine now are row `row` */
    const double clk = la_tune && la_tune->pal ? 985248.0 : 1022727.0;
    const int slot = (int)(row % NT_ROWS);
    for (int k = 0; k < 9; k++) {
        nt_cell cell = { -1, 0 };
        ngsid *syn = k < la_ncols ? engine_synth_at(la_e, (unsigned)(k / 3)) : NULL;
        if (syn) {
            ngsid_voice_info vi; ngsid_get_voice_info(syn, k % 3, &vi);
            int gate = vi.ctrl & 1, noise = (vi.ctrl & 0x80) != 0, note = -1;
            double hz = vi.freq * clk / 16777216.0;
            if (vi.freq && hz > 8) { int mi = (int)floor(69 + 12 * log2(hz / 440.0) + 0.5); if (mi >= 0 && mi < 128) note = mi; }
            if (gate && note >= 0 && (!nt_pgate[k] || note != nt_pnote[k] || noise != nt_pnoise[k])) { cell.note = (signed char)note; cell.kind = noise ? 3 : 1; }
            else if (!gate && nt_pgate[k]) cell.kind = 2;
            else if (gate) cell.kind = 4;
            nt_pgate[k] = gate; nt_pnote[k] = note; nt_pnoise[k] = noise;
        }
        nt_cells[k][slot] = cell;
    }
    nt_rowid[slot] = row;
}
/* render the look-ahead engine up to `target` frames, but not longer than budget_ms (catching up after the view was switched on in the middle of a tune takes a while) */
static void la_advance(uint64_t target, double budget_ms) {
    static int16_t raw[NGSID_BLOCK * 2];
    const double rowf = (double)g_rate / NT_RATE;
    const Uint64 t0 = SDL_GetPerformanceCounter();
    while (la_e && la_frames < target) {
        while ((double)la_next_row * rowf <= (double)la_frames) { la_sample(la_next_row); la_next_row++; }
        size_t n = engine_render(la_e, raw, NGSID_BLOCK);
        if (!n || engine_error(la_e)) { la_destroy(); return; }
        la_frames += n;
        if ((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency() > budget_ms) break;
    }
}

static void tune_free(tune_t *t) {
    if (!t) return;
    if (t == la_tune) la_destroy();                    /* the look-ahead engine of the notes view belongs to this tune */
    if (t->e) engine_destroy(t->e);
    free(t->buf);
    free(t);
}

/* Parse + create a player outside the audio lock, swap it in under the lock. */
static int tune_load(const char *path, int song, tune_t **out_info) {
    g_error[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(g_error, sizeof g_error, "%s", strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    tune_t *t = (tune_t *)calloc(1, sizeof *t);
    t->buf = (uint8_t *)malloc(len > 0 ? (size_t)len : 1);
    if (!t->buf || fread(t->buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); snprintf(g_error, sizeof g_error, "read error"); tune_free(t); return -1; }
    fclose(f);
    t->len = (size_t)len;
    int rc = psid_parse(t->buf, (size_t)len, &t->info);
    if (rc) { snprintf(g_error, sizeof g_error, "%s", psid_strerror(rc)); tune_free(t); return -1; }
    if (song < 1 || song > t->info.songs) song = t->info.start_song;
    t->song = song;
    t->pal = g_pal_override >= 0 ? g_pal_override : (t->info.clock == 2 ? 0 : 1);
    ngsid_config cfg = g_cfg;
    cfg.pal = t->pal; cfg.stereo = 1; cfg.sample_rate = g_rate;
    engine_opts eo;
    eo.pal_override = g_pal_override;
    eo.sid_model = g_sid_model;
    char emsg[160] = "";
    /* RSID with the C64 BASIC flag (header flags bit 1): the tune is a BASIC program, which needs the BASIC ROM. ngSid has no ROMs
     * (Commodore's are not ours to ship), so nothing would play; say so instead of staying silent. */
    int basic_tune = t->info.rsid && (t->info.flags & 2);
    t->e = basic_tune ? NULL : engine_create(t->buf, (size_t)len, song, g_rate, &cfg, &eo, emsg, sizeof emsg);
    if (t->e) {
        t->s = engine_synth(t->e);
        t->nsid = (int)engine_sid_count(t->e);
        if (t->nsid > 3) t->nsid = 3;
        for (int i = 0; i < t->nsid; i++) t->model[i] = engine_sid_model(t->e, (unsigned)i);
        for (int i = 0; i < t->nsid; i++) t->syn[i] = engine_synth_at(t->e, (unsigned)i);
        for (int i = 0; i < t->nsid; i++) if (t->syn[i]) ngsid_set_voice_mute(t->syn[i], g_mute);
        engine_mute(t->e, g_mute);
    } else {
        if (basic_tune) snprintf(g_error, sizeof g_error, "Cannot play: a C64 BASIC program, it needs the BASIC ROM (not included)");
        else snprintf(g_error, sizeof g_error, "init: %s", emsg);
        /* keep the header data for display, but nothing to play */
    }
    if (g_dev) SDL_LockAudioDevice(g_dev);
    tune_t *old = g_tune;
    g_tune = t;
    memset(&g_vis, 0, sizeof g_vis);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    tune_free(old);
    (void)out_info;
    return t->e ? 0 : -1;
}

/* ================================================================== playlist */

typedef struct { char *path; char label[128]; char *rel; } entry;   /* rel: HVSC path if fetched online */
static entry *pl;
static int npl, cur = -1, cur_song = 1;
static float list_scroll;
static int list_follow = 1;

static int is_sid_name(const char *n) {
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".sid");
}

static void add_file(const char *path) {
    pl = (entry *)realloc(pl, (size_t)(npl + 1) * sizeof *pl);
    pl[npl].path = strdup(path);
    pl[npl].rel = getenv("NGSID_TEST_REL") ? strdup(getenv("NGSID_TEST_REL")) : NULL;      /* test hook: a fake HVSC path (the link in the Info view) */
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char tmp[200];
    utf8_to_latin1(b, tmp, sizeof tmp);
    size_t l = strlen(tmp);
    if (l > 4 && !strcasecmp(tmp + l - 4, ".sid")) tmp[l - 4] = 0;
    for (char *q = tmp; *q; q++) if (*q == '_') *q = ' ';
    snprintf(pl[npl].label, sizeof pl[npl].label, "%s", tmp);
    npl++;
}

/* ---- favorites: tunes of the online HVSC (their path in the HVSC), kept in a text file with one path per line */
typedef struct { char *rel; char label[128]; } fav_t;
static fav_t *fav;
static void fav_remember(const char *rel);
static int nfav, fav_mode;                               /* fav_mode: the sidebar shows the favorites (then browse is 0) */
static float fav_scroll;
static int fav_find(const char *rel) {
    for (int i = 0; i < nfav; i++) if (!strcmp(fav[i].rel, rel)) return i;
    return -1;
}
static int fav_cur(void) { return cur >= 0 && cur < npl && pl[cur].rel ? fav_find(pl[cur].rel) : -1; }
static void fav_label(const char *rel, char *out, size_t cap) {
    const char *b = strrchr(rel, '/');
    b = b ? b + 1 : rel;
    char tmp[200];
    utf8_to_latin1(b, tmp, sizeof tmp);
    size_t l = strlen(tmp);
    if (l > 4 && !strcasecmp(tmp + l - 4, ".sid")) tmp[l - 4] = 0;
    for (char *q = tmp; *q; q++) if (*q == '_') *q = ' ';
    snprintf(out, cap, "%s", tmp);
}
static void fav_add_raw(const char *rel) {
    if (!*rel || fav_find(rel) >= 0) return;
    fav = (fav_t *)realloc(fav, (size_t)(nfav + 1) * sizeof *fav);
    fav[nfav].rel = strdup(rel);
    fav_label(rel, fav[nfav].label, sizeof fav[nfav].label);
    nfav++;
}

static int cmp_entry(const void *a, const void *b) { return strcasecmp(((const entry *)a)->path, ((const entry *)b)->path); }

static void add_path(const char *path, int depth) {
    struct stat st;
    if (stat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        if (depth > 3) return;
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *e;
        int first = npl;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            add_path(full, depth + 1);
        }
        closedir(d);
        qsort(pl + first, (size_t)(npl - first), sizeof *pl, cmp_entry);
    } else if (is_sid_name(path))
        add_file(path);
}

#ifdef __EMSCRIPTEN__
/* address bar = shareable link of the tune that plays (HVSC path), or no hash for local files.
 * replaceState does not fire hashchange, so this never reloads anything. */
static void web_set_url(const char *rel) {
    EM_ASM({
        var r = '';
        var p = $0;
        var c;
        if (p) { while ((c = HEAPU8[p++])) r += String.fromCharCode(c); }
        try {
            history.replaceState(null, '', r ? '#/' + encodeURI(decodeURIComponent(escape(r))) : location.pathname + location.search);
        } catch (e) {}
    }, rel);
}
#endif

static void play_index(int idx, int song) {
    if (idx < 0 || idx >= npl) return;
    cur = idx;
    list_follow = 1;
    if (!pl[idx].path[0] && pl[idx].rel) {          /* entry of a folder link: fetched when it is played */
        char file[512];
        if (hvsc_fetch(pl[idx].rel, file, sizeof file) != 0) { snprintf(g_error, sizeof g_error, "%s: HVSC unreachable", pl[idx].label); return; }
        free(pl[idx].path);
        pl[idx].path = strdup(file);
    }
    tune_load(pl[idx].path, song, NULL);
    cur_song = g_tune ? g_tune->song : 1;
#ifdef __EMSCRIPTEN__
    web_set_url(pl[idx].rel);
#endif
}

/* ---- online HVSC browser: folder listings and tunes are fetched by a worker thread */

/* row of the playing tune in the current HVSC folder (-1 if it is elsewhere) */
static int browse_cur_row(void);
static int browse;                       /* sidebar shows the HVSC browser instead of the playlist */
static char rpath[768];                  /* current folder, "" = root, else "MUSICIANS/H/" */
#if defined(__APPLE__) && TARGET_OS_TV
#define NG_TV 1                                  /* Apple TV: the browser is fixed at MUSICIANS (nothing above it), no tabs, the remote steers */
#define BR_ROOT "MUSICIANS/"
#else
#define NG_TV 0
#define BR_ROOT ""
#endif
static int br_at_root(void) { return !strcmp(rpath, BR_ROOT) || (NG_TV && !rpath[0]); }          /* the top of the browser: the HVSC root, on the Apple TV MUSICIANS/ */
static hvsc_entry *be;
static int nbe;
static float br_scroll;
static int br_sel, br_follow;            /* keyboard cursor row, scroll-into-view request */
static char br_focus[160];               /* folder name to put the cursor on after the next listing */
static char br_msg[96];
static struct {
    SDL_atomic_t state;                  /* 0 idle, 1 running, 2 done */
    int kind, rc, nres;                  /* kind 0 = list folder, 1 = fetch file */
    char path[768], file[1024];
    hvsc_entry *res;
} job;

/* ---- update check: asks the GitHub API once at startup (curl, worker thread) */
#define NGSID_REPO "DarkVamp76/ngsid"
static SDL_atomic_t upd_state;          /* 0 idle/none, 1 running, 2 update available */
static char upd_tag[48], upd_url[160];
static int g_check_updates = 1;

static const char *ver_plain(const char *v) { return (*v == 'v' || *v == 'V') ? v + 1 : v; }

/* parse "x.y.z" (leading numbers only); returns the number of parts found */
static int ver_parse(const char *v, int out[3]) {
    int n = 0;
    out[0] = out[1] = out[2] = 0;
    v = ver_plain(v);
    while (n < 3 && *v >= '0' && *v <= '9') {
        out[n++] = atoi(v);
        while (*v >= '0' && *v <= '9') v++;
        if (*v == '.') v++; else break;
    }
    return n;
}

static int update_fn(void *ud) {
    (void)ud;
    char *buf = NULL;
    size_t len = 0;
    const char *url = getenv("NGSID_UPDATE_URL");   /* testing aid */
    if (!url || !*url) url = "https://api.github.com/repos/" NGSID_REPO "/releases/latest";
    if (hvsc_http_get(url, &buf, &len) == 0 && buf) {
        buf = (char *)realloc(buf, len + 1);
        buf[len] = 0;
        const char *k = strstr(buf, "\"tag_name\"");
        if (k && (k = strchr(k + 10, ':')) && (k = strchr(k, '"'))) {
            k++;
            size_t n = strcspn(k, "\"");
            if (n > 0 && n < sizeof upd_tag) {
                char tag[48];
                memcpy(tag, k, n); tag[n] = 0;
                int a[3], b[3];
                if (ver_parse(tag, a) && ver_parse(NGSID_VERSION, b)) {
                    int newer = 0;
                    for (int i = 0; i < 3; i++) { if (a[i] != b[i]) { newer = a[i] > b[i]; break; } }
                    if (newer) {
                        snprintf(upd_tag, sizeof upd_tag, "%s", tag);
                        snprintf(upd_url, sizeof upd_url, "https://github.com/" NGSID_REPO "/releases/tag/%s", tag);
                        SDL_AtomicSet(&upd_state, 2);
                    }
                }
            }
        }
    }
    free(buf);
    if (SDL_AtomicGet(&upd_state) == 1) SDL_AtomicSet(&upd_state, 0);
    return 0;
}

static void update_check_start(void) {
    if (!g_check_updates) return;
    SDL_AtomicSet(&upd_state, 1);
    SDL_Thread *t = SDL_CreateThread(update_fn, "update", NULL);
    if (t) SDL_DetachThread(t); else SDL_AtomicSet(&upd_state, 0);
}

static int job_fn(void *ud) {
    (void)ud;
    if (job.kind == 0) job.rc = hvsc_list(job.path, &job.res, &job.nres);
    else job.rc = hvsc_fetch(job.path, job.file, sizeof job.file);
    SDL_AtomicSet(&job.state, 2);
    return 0;
}

static void job_start(int kind, const char *path) {
    if (SDL_AtomicGet(&job.state)) return;
    if (kind == 1) fav_remember(path);                 /* a favorite that starts is remembered for the next launch */
#ifdef __EMSCRIPTEN__
    /* no threads in the browser: the job runs right here (synchronous request to the proxy) */
    job.kind = kind;
    snprintf(job.path, sizeof job.path, "%s", path);
    job.res = NULL; job.nres = 0; job.rc = 0;
    br_msg[0] = 0;
    SDL_AtomicSet(&job.state, 1);
    job_fn(NULL);
    return;
#endif
    job.kind = kind;
    snprintf(job.path, sizeof job.path, "%s", path);
    job.res = NULL; job.nres = 0; job.rc = 0;
    br_msg[0] = 0;
    SDL_AtomicSet(&job.state, 1);
    SDL_Thread *t = SDL_CreateThread(job_fn, "hvsc", NULL);
    if (t) SDL_DetachThread(t);
    else { SDL_AtomicSet(&job.state, 0); snprintf(br_msg, sizeof br_msg, "Thread error"); }
}

static int browse_cur_row(void) {
    if (cur < 0 || cur >= npl || !pl[cur].rel) return -1;
    size_t rl = strlen(rpath);
    if (strncmp(pl[cur].rel, rpath, rl)) return -1;
    const char *rest = pl[cur].rel + rl;
    int off = br_at_root() ? 0 : 1;
    for (int i = 0; i < nbe; i++)
        if (!be[i].dir && !strcmp(be[i].name, rest)) return i + off;
    return -1;
}

static void job_poll(void) {
    if (SDL_AtomicGet(&job.state) != 2) return;
    if (job.rc == 0) {
        if (job.kind == 0) {
            free(be);
            be = job.res;
            nbe = job.nres;
            snprintf(rpath, sizeof rpath, "%s", job.path);
            br_scroll = 0;
            int off = job.path[0] ? 1 : 0, cr = browse_cur_row();
            br_sel = off < nbe + off ? off : 0;
            if (br_focus[0]) {
                for (int i = 0; i < nbe; i++) if (be[i].dir && !strcmp(be[i].name, br_focus)) { br_sel = i + off; break; }
            } else if (cr >= 0)
                br_sel = cr;
            br_focus[0] = 0;
            br_follow = 1;
        } else {
            int idx = -1;
            for (int i = 0; i < npl; i++) if (!strcmp(pl[i].path, job.file)) { idx = i; break; }
            if (idx < 0) { add_file(job.file); idx = npl - 1; }
            if (!pl[idx].rel) pl[idx].rel = strdup(job.path);
            play_index(idx, 0);
        }
    } else {
        free(job.res);
        snprintf(br_msg, sizeof br_msg, "%s", job.rc == HVSC_ERR_NOCURL ? "curl not found" : "HVSC unreachable");
    }
    SDL_AtomicSet(&job.state, 0);
}

static void browse_open(void) {
    browse = 1; fav_mode = 0;
    if (!nbe && br_at_root() && !SDL_AtomicGet(&job.state)) job_start(0, BR_ROOT);
}

static void browse_up(void) {
    if (br_at_root()) return;
    char up[768];
    snprintf(up, sizeof up, "%s", rpath);
    size_t l = strlen(up);
    if (l) up[--l] = 0;                  /* drop trailing '/' */
    char *sl = strrchr(up, '/');
    snprintf(br_focus, sizeof br_focus, "%s", sl ? sl + 1 : up);   /* cursor lands on the folder we left */
    if (sl) sl[1] = 0; else up[0] = 0;
    job_start(0, up);
}

/* row 0 is ".." below the root, then the entries */
static void browse_activate(int row) {
    int off = br_at_root() ? 0 : 1;
    if (off && row == 0) { browse_up(); return; }
    int i = row - off;
    if (i < 0 || i >= nbe) return;
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", rpath, be[i].name, be[i].dir ? "/" : "");
    job_start(be[i].dir ? 0 : 1, path);
}

static void browse_move(int delta) {
    int count = nbe + (br_at_root() ? 0 : 1);
    if (count <= 0) return;
    br_sel += delta;
    if (br_sel < 0) br_sel = 0;
    if (br_sel > count - 1) br_sel = count - 1;
    br_follow = 1;
}

static void change_song(int delta) {
    if (!g_tune || g_tune->info.songs < 1) return;
    int n = g_tune->info.songs;
    int s = ((cur_song - 1 + delta) % n + n) % n + 1;
    play_index(cur, s);
}

static void change_file(int delta) {
    if (fav_mode && nfav) {                    /* FAV is shown: step through the favorites */
        if (SDL_AtomicGet(&job.state)) return;
        int c = fav_cur(), start = c >= 0 ? c : (delta > 0 ? -1 : nfav);
        job_start(1, fav[((start + delta) % nfav + nfav) % nfav].rel);
        return;
    }
    if (browse && nbe) {                       /* HVSC Online is shown: step through the tunes of that folder, not through the playlist */
        int off = br_at_root() ? 0 : 1, cr = browse_cur_row(), start = cr >= 0 ? cr - off : (delta > 0 ? -1 : nbe), found = -1;
        for (int k = 1; k <= nbe; k++) {
            int i = ((start + delta * k) % nbe + nbe) % nbe;
            if (!be[i].dir) { found = i; break; }
        }
        if (found >= 0) {
            if (SDL_AtomicGet(&job.state)) return;           /* still loading the previous one */
            char rel[800];
            snprintf(rel, sizeof rel, "%s%s", rpath, be[found].name);
            br_sel = found + off; br_follow = 1;
            job_start(1, rel);
            return;
        }
    }
    if (!npl) return;
    play_index(((cur + delta) % npl + npl) % npl, 0);
}

/* transport buttons: next/previous sub tune, and when there is none left (or the file has only one) the next/previous file of the list */
static void step_tune(int delta) {
    int n = g_tune ? g_tune->info.songs : 1;
    if (delta > 0 && cur_song < n) change_song(1);
    else if (delta < 0 && cur_song > 1) change_song(-1);
    else change_file(delta);
}

/* ================================================================== analysis helpers */

static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len, wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

#define NBANDS 64
static float band[NBANDS], bandpk[NBANDS], bandhold[NBANDS];
static float vu[2], vupk[2], vuhold[2];
static float scope_amp[9] = { 60, 60, 60, 60, 60, 60, 60, 60, 60 };
static float glow[3];
static float g_beat;    /* 1 at a detected beat, decays quickly: drives the pulse of the SID graphic */

static void analyse(const vis_t *v, float dt) {
    static double re[2048], im[2048], win[2048];
    static int init;
    if (!init) { for (int i = 0; i < 2048; i++) win[i] = 0.5 - 0.5 * cos(2 * M_PI * i / 2047.0); init = 1; }
    for (int i = 0; i < 2048; i++) { re[i] = v->ring[i] * win[i]; im[i] = 0; }
    fft(re, im, 2048);
    float mag[1025];
    for (int i = 0; i <= 1024; i++) mag[i] = (float)(sqrt(re[i % 2048] * re[i % 2048] + im[i % 2048] * im[i % 2048]) / (2048.0 / 4.0));
    double fmin = 40.0, fmax = 17000.0, binhz = (double)g_rate / 2048.0;
    for (int b = 0; b < NBANDS; b++) {
        double f0 = fmin * pow(fmax / fmin, (double)b / NBANDS), f1 = fmin * pow(fmax / fmin, (double)(b + 1) / NBANDS);
        double b0 = f0 / binhz, b1 = f1 / binhz;
        float m = 0;
        if (b1 - b0 < 1.0) {
            double c = (b0 + b1) / 2;
            int i0 = (int)c;
            double fr = c - i0;
            m = (float)(mag[i0] * (1 - fr) + mag[i0 + 1] * fr);
        } else {
            for (int i = (int)ceil(b0); i <= (int)b1 && i <= 1024; i++) if (mag[i] > m) m = mag[i];
        }
        float db = 20.0f * log10f(m + 1e-7f);
        float val = (db + 66.0f) / 66.0f + (float)b / NBANDS * 0.10f;   /* slight tilt to compensate the 1/f slope */
        if (val < 0) val = 0;
        if (val > 1) val = 1;
        if (val > band[b]) band[b] += (val - band[b]) * 0.65f;
        else { band[b] -= dt * 1.3f; if (band[b] < val) band[b] = val; }
        if (band[b] >= bandpk[b]) { bandpk[b] = band[b]; bandhold[b] = 0.45f; }
        else if (bandhold[b] > 0) bandhold[b] -= dt;
        else { bandpk[b] -= dt * 0.55f; if (bandpk[b] < band[b]) bandpk[b] = band[b]; }
    }
    {   /* beat: the bass or the hi-hat/noise range jumps above its own running average (drum hits) */
        static float slow[2], last[2], since = 1;
        float e[2] = { 0, 0 };
        int lo1 = (int)(220.0 / binhz) + 1, h0 = (int)(3500.0 / binhz), h1 = (int)(12000.0 / binhz);
        for (int i = 1; i <= lo1; i++) e[0] += mag[i] * mag[i];
        for (int i = h0; i <= h1; i++) e[1] += mag[i] * mag[i];
        e[0] = sqrtf(e[0]); e[1] = sqrtf(e[1]);
        since += dt;
        int hit = 0;
        for (int k = 0; k < 2; k++) {
            if (e[k] > 1.5f * slow[k] + 0.006f && e[k] > 1.10f * last[k]) hit = 1;
            slow[k] += (e[k] - slow[k]) * (1.0f - expf(-dt / 0.40f));
            last[k] = e[k];
        }
        if (hit && since > 0.11f) { g_beat = 1; since = 0; }
        g_beat *= expf(-dt * 7.0f);
    }
    for (int c = 0; c < 2; c++) {
        float db = 20.0f * log10f(v->peak[c] + 1e-6f);
        float val = (db + 48.0f) / 48.0f;
        if (val < 0) val = 0;
        if (val > 1) val = 1;
        if (val > vu[c]) vu[c] = val; else { vu[c] -= dt * 1.6f; if (vu[c] < val) vu[c] = val; }
        if (vu[c] >= vupk[c]) { vupk[c] = vu[c]; vuhold[c] = 0.8f; }
        else if (vuhold[c] > 0) vuhold[c] -= dt;
        else { vupk[c] -= dt * 0.5f; if (vupk[c] < vu[c]) vupk[c] = vu[c]; }
    }
}

static const char *NOTE[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* ================================================================== UI */

static float mx, my;
static int click, clickx, clicky, mouse_down, vol_drag;
static int rclick, rclickx, rclicky;                       /* right mouse button */
static int ctx_on; static float ctx_x, ctx_y; static char ctx_rel[800];   /* context menu (Share / Info) of an HVSC tune */
static float toast_t; static char toast_txt[48];
#define CTX_W 150.0f
#define CTX_ROW 30.0f
static int ctx_n(void) { size_t l = strlen(ctx_rel); return l && ctx_rel[l - 1] == '/' ? 2 : 3; }   /* a folder has no Favorite item */
static float ctx_h(void) { return 2 + ctx_n() * CTX_ROW; }

/* favorites file: ~/.config/ngsid/favorites.txt (on iOS and macOS the home is the app container), in the browser the local storage */
static void fav_parse(char *txt) {
    for (char *line = strtok(txt, "\r\n"); line; line = strtok(NULL, "\r\n")) fav_add_raw(line);
}
static void fav_text(char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < nfav; i++) {
        size_t l = strlen(fav[i].rel);
        if (o + l + 2 >= cap) break;
        memcpy(out + o, fav[i].rel, l); o += l; out[o++] = '\n'; out[o] = 0;
    }
}
#ifdef __EMSCRIPTEN__
static char fav_buf[1 << 16];
static void fav_save(void) {
    fav_text(fav_buf, sizeof fav_buf);
    EM_ASM({ try { localStorage.setItem('ngsid_favorites', UTF8ToString($0)); } catch (e) {} }, fav_buf);
}
static void fav_load(void) {
    EM_ASM({
        var v = '';
        try { v = localStorage.getItem('ngsid_favorites') || ''; } catch (e) {}
        var b = new TextEncoder().encode(v);
        var n = Math.min(b.length, $1 - 1);
        HEAPU8.set(b.subarray(0, n), $0); HEAPU8[$0 + n] = 0;
    }, fav_buf, (int)sizeof fav_buf);
    fav_parse(fav_buf);
}
#else
static int fav_file(char *out, size_t cap, int mk) {
    const char *xc = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    char dir[900];
    if (xc && *xc) snprintf(dir, sizeof dir, "%s/ngsid", xc);
    else if (h && *h) snprintf(dir, sizeof dir, "%s/.config/ngsid", h);
    else return -1;
    if (mk) {
        char p[900];
        snprintf(p, sizeof p, "%s", dir);
        for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
        mkdir(p, 0755);
    }
    snprintf(out, cap, "%s/favorites.txt", dir);
    return 0;
}
static void fav_save(void) {
    char path[1000], *buf = (char *)malloc(1 << 20);
    if (!buf) return;
    if (fav_file(path, sizeof path, 1) == 0) {
        fav_text(buf, 1 << 20);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(buf, 1, strlen(buf), f); fclose(f); }
    }
    free(buf);
}
static void fav_load(void) {
    char path[1000];
    if (fav_file(path, sizeof path, 0)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = (char *)calloc(1, (1 << 20) + 1);
    if (buf) { size_t n = fread(buf, 1, 1 << 20, f); buf[n] = 0; fav_parse(buf); free(buf); }
    fclose(f);
}
#endif

/* the last favorite that was played: ~/.config/ngsid/lastfav.txt, in the browser the local storage key ngsid_lastfav */
static char fav_last[800];
#ifdef __EMSCRIPTEN__
static void fav_last_save(void) {
    EM_ASM({ try { localStorage.setItem('ngsid_lastfav', UTF8ToString($0)); } catch (e) {} }, fav_last);
}
static void fav_last_load(void) {
    EM_ASM({
        var v = '';
        try { v = localStorage.getItem('ngsid_lastfav') || ''; } catch (e) {}
        var b = new TextEncoder().encode(v);
        var n = Math.min(b.length, $1 - 1);
        HEAPU8.set(b.subarray(0, n), $0); HEAPU8[$0 + n] = 0;
    }, fav_last, (int)sizeof fav_last);
}
#else
static int fav_last_file(char *out, size_t cap, int mk) {
    char p[1000];
    if (fav_file(p, sizeof p, mk)) return -1;
    char *sl = strrchr(p, '/');
    if (!sl) return -1;
    snprintf(out, cap, "%.*s/lastfav.txt", (int)(sl - p), p);
    return 0;
}
static void fav_last_save(void) {
    char path[1100];
    if (fav_last_file(path, sizeof path, 1)) return;
    FILE *f = fopen(path, "wb");
    if (f) { fputs(fav_last, f); fclose(f); }
}
static void fav_last_load(void) {
    char path[1100];
    fav_last[0] = 0;
    if (fav_last_file(path, sizeof path, 0)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(fav_last, 1, sizeof fav_last - 1, f);
    fav_last[n] = 0;
    fclose(f);
    fav_last[strcspn(fav_last, "\r\n")] = 0;
}
#endif
static void fav_remember(const char *rel) {
    if (fav_find(rel) < 0 || !strcmp(fav_last, rel)) return;
    snprintf(fav_last, sizeof fav_last, "%s", rel);
    fav_last_save();
}
static void fav_toggle(const char *rel) {
    int i = fav_find(rel);
    if (i >= 0) {
        free(fav[i].rel);
        memmove(fav + i, fav + i + 1, (size_t)(nfav - i - 1) * sizeof *fav);
        nfav--;
        snprintf(toast_txt, sizeof toast_txt, "Removed from favorites");
    } else {
        fav_add_raw(rel);
        snprintf(toast_txt, sizeof toast_txt, "Added to favorites");
    }
    toast_t = 2.0f;
    fav_save();
}
#if defined(__APPLE__) && TARGET_OS_TV
/* ---- Apple TV: the context menu (Select held) and the favorites list ---------------------------------------------------------------------------------------------
 * Select held (0.5 s) opens a menu: add / remove the favorite (the tune in the browser row, or the playing one), show the favorites / the HVSC browser, cancel.
 * Up / Down choose, Select acts, Menu closes. The favorites list: Up / Down choose a tune, Select plays it, Menu goes back to the browser. */
static int tv_fav_sel;                                                   /* the marked row of the favorites list */
static Uint32 tv_dn_t; static int tv_dn_armed;                          /* Select is down: acts on release, held 0.5 s = the menu */
static int tv_ctx_open, tv_ctx_sel, tv_ctx_n, tv_ctx_it[4]; static Uint32 tv_ctx_t;
static char tv_ctx_rel[768];                                             /* the tune the favorite entry of the menu is about */
enum { TVC_FAV = 1, TVC_SHOWFAV, TVC_SHOWBROWSER, TVC_CANCEL };
static void tv_ctx_show(void) {
    tv_ctx_rel[0] = 0;
    if (fav_mode) { if (tv_fav_sel >= 0 && tv_fav_sel < nfav) snprintf(tv_ctx_rel, sizeof tv_ctx_rel, "%s", fav[tv_fav_sel].rel); }
    else {
        int off = br_at_root() ? 0 : 1, i = br_sel - off;
        if (browse && i >= 0 && i < nbe && !be[i].dir) snprintf(tv_ctx_rel, sizeof tv_ctx_rel, "%s%s", rpath, be[i].name);
        else if (cur >= 0 && cur < npl && pl[cur].rel) snprintf(tv_ctx_rel, sizeof tv_ctx_rel, "%s", pl[cur].rel);
    }
    tv_ctx_n = 0;
    if (tv_ctx_rel[0]) tv_ctx_it[tv_ctx_n++] = TVC_FAV;
    tv_ctx_it[tv_ctx_n++] = fav_mode ? TVC_SHOWBROWSER : TVC_SHOWFAV;
    tv_ctx_it[tv_ctx_n++] = TVC_CANCEL;
    tv_ctx_open = 1; tv_ctx_sel = 0; tv_ctx_t = SDL_GetTicks();
}
static void tv_ctx_act(void) {
    int a = tv_ctx_it[tv_ctx_sel]; tv_ctx_open = 0; tv_ctx_t = SDL_GetTicks();
    switch (a) {
    case TVC_FAV: fav_toggle(tv_ctx_rel); if (tv_fav_sel >= nfav) tv_fav_sel = nfav - 1; if (tv_fav_sel < 0) tv_fav_sel = 0; break;
    case TVC_SHOWFAV: fav_mode = 1; browse = 0; if (tv_fav_sel >= nfav) tv_fav_sel = 0; break;
    case TVC_SHOWBROWSER: fav_mode = 0; browse = 1; break;
    default: break;
    }
}
#endif
static int hit(float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; }
static int clicked(float x, float y, float w, float h) { return click && clickx >= x && clickx < x + w && clicky >= y && clicky < y + h; }

static float chip(float x, float y, const char *label, col c, int lit) {
    float w = text_width(label, 12) + 14;
    if (lit) {
        rrect(x, y, w, 20, 6, alpha(c, 0.22f));
        text(x + 7, y + 3, 12, c, label);
    } else {
        rrect(x, y, w, 20, 6, alpha(FAINT, 0.18f));
        text(x + 7, y + 3, 12, alpha(FAINT, 0.9f), label);
    }
    return w + 5;
}

/* ---- the SID chip graphic (DIP-28) for the header: variant per model, glow follows the level */
static float chip_w(const char *label) { return text_width(label, 12) + 14 + 5; }

static void draw_sid(float x, float y, float w, int model, float lvl, float beat) {
    {   /* grow a little on the beat, around the centre */
        float k = 1.0f + 0.07f * beat, nw = w * k;
        x -= (nw - w) / 2; y -= (nw - w) * 0.4f / 2; w = nw;
    }
    const int m8580 = model == 8580;
    const float h = w * 0.40f, pitch = w * 0.94f / 14.0f, pw = pitch * 0.42f;
    const float bx = x, by = y + h * 0.13f, bw = w, bh = h * 0.74f;
    const col accent = m8580 ? C(0.35f, 0.82f, 1.0f, 1) : C(1.0f, 0.62f, 0.28f, 1);
    const col body = m8580 ? C(0.075f, 0.085f, 0.105f, 1) : C(0.105f, 0.092f, 0.088f, 1);
    const col label = m8580 ? C(0.80f, 0.89f, 1.0f, 1) : C(0.93f, 0.83f, 0.64f, 1);

    set_additive(1);
    blob(x + w / 2, y + h / 2, w * (0.30f + 0.12f * beat), alpha(accent, 0.10f + 0.30f * lvl + 0.55f * beat));
    set_additive(0);

    shadow(bx, by + 3, bw, bh, 4, 9, C(0, 0, 0, 0.65f));
    for (int i = 0; i < 14; i++) {                       /* legs: tin plated, both long sides */
        float px = x + pitch * 0.55f + pitch * (float)i;
        col pa = mix(C(0.86f, 0.87f, 0.90f, 1), accent, 0.55f * beat), pb = mix(C(0.50f, 0.51f, 0.55f, 1), accent, 0.35f * beat);
        rect_grad(px, y, pw, h * 0.22f, pa, pb);
        rect_grad(px, y + h * 0.78f, pw, h * 0.22f, pb, pa);
    }
    rrect(bx, by, bw, bh, 3.5f, C(0.30f, 0.30f, 0.34f, 1));            /* bevelled edge */
    rrect(bx + 1, by + 1, bw - 2, bh - 2, 3, body);
    rect_grad(bx + 4, by + 2, bw - 8, bh * 0.40f, C(1, 1, 1, 0.075f), C(1, 1, 1, 0));
    rect_grad(bx + 4, by + bh * 0.70f, bw - 8, bh * 0.28f, C(0, 0, 0, 0), C(0, 0, 0, 0.22f));

    float r = bh * 0.15f, dx = bx + bw * 0.075f, dy = by + bh * 0.5f;    /* moulded dimple */
    rrect(dx - r, dy - r, r * 2, r * 2, r, C(0, 0, 0, 0.45f));
    rrect(dx - r * 0.8f, dy - r * 0.8f, r * 1.6f, r * 1.6f, r * 0.8f, alpha(body, 1));
    float pr = bh * 0.045f;                                              /* pin 1 marker */
    rrect(bx + bw * 0.045f, by + bh - bh * 0.16f, pr * 2, pr * 2, pr, C(0, 0, 0, 0.5f));

    char l1[24];
    snprintf(l1, sizeof l1, m8580 ? "CSG 8580R5" : "MOS 6581R4");
    float fs = bh * 0.27f, avail = bw * 0.74f;
    while (fs > 7 && text_width(l1, fs) > avail) fs -= 0.5f;
    float tx = bx + bw * 0.17f;
    int sub = w >= 150;
    float ty = by + bh * (sub ? 0.20f : 0.34f);
    text(tx, ty, fs, alpha(mix(label, C(1, 1, 1, 1), 0.5f * beat), 0.93f), l1);
    if (sub) {
        float f2 = bh * 0.125f;
        text(tx, ty + fs * 1.28f, f2, alpha(label, 0.55f), "SOUND INTERFACE DEVICE");
    }
}

/* Where libsidplayfp's mixer puts chip i of n in the stereo field: with two chips SID 1 sits at
 * left 1.0 / right 0.5 and SID 2 mirrored, with three SID 2 is centred. -1 = left, 0 = centre, +1 = right. */
static float sid_pan(int n, int i) {
    if (n < 2) return 0;
    if (i == 0) return -1.0f / 3.0f;
    if (i == n - 1) return 1.0f / 3.0f;
    return 0;
}

static void draw_pan(float x, float y, float w, float pan, col accent) {
    text(x, y - 3, 9, alpha(DIM, 0.8f), "L");
    text_right(x + w, y - 3, 9, alpha(DIM, 0.8f), "R");
    float bx = x + 12, bw = w - 24, cxm = bx + bw / 2;
    rrect(bx, y, bw, 4, 2, alpha(FAINT, 0.40f));
    rect(cxm - 0.5f, y - 2, 1, 8, alpha(DIM, 0.55f));
    float mx = cxm + pan * bw / 2;
    set_additive(1); blob(mx, y + 2, 7, alpha(accent, 0.45f)); set_additive(0);
    rrect(mx - 3.5f, y - 1.5f, 7, 7, 3.5f, accent);
}

static void card(float x, float y, float w, float h, int hover) {
    shadow(x, y + 6, w, h, 14, 22, C(0, 0, 0, 0.5f));
    rrect(x, y, w, h, 14, alpha(EDGE, hover ? 0.75f : 0.45f));
    rrect(x + 1, y + 1, w - 2, h - 2, 13, hover ? CARDHI : CARD);
}

static void mmss(char *buf, size_t cap, double sec) {
    int s = (int)sec;
    snprintf(buf, cap, "%d:%02d", s / 60, s % 60);
}

static void draw_voice(int v, float x, float y, float w, float h, const vis_t *vs, double t) {
    const ngsid_voice_info *vi = &vs->vi[0][v];
    col vc = VC[v];
    int muted = (g_mute >> v) & 1;
    int active = ((vi->ctrl & 1) || vi->level > 0) && vs->running;
    card(x, y, w, h, hit(x, y, w, h));
    if (!g_mobile && glow[v] > 0.01f) {
        set_additive(1);
        blob(x + w / 2, y + 90, w * 0.45f, alpha(vc, 0.10f * glow[v]));
        set_additive(0);
    }
    char buf[64];
    snprintf(buf, sizeof buf, "VOICE %d", v + 1);
    text(x + 16, y + 12, 13, alpha(DIM, 1), buf);
    /* gate LED */
    float lx = x + w - 26, ly = y + 21;
    rrect(lx - 6, ly - 6, 12, 12, 6, alpha(FAINT, 0.5f));
    if (vi->ctrl & 1 && vs->running) {
        set_additive(1); blob(lx, ly, 10, alpha(vc, 0.55f)); set_additive(0);
        rrect(lx - 5, ly - 5, 10, 10, 5, vc);
    }
    /* note + Hz */
    double clk = g_tune && g_tune->pal ? 985248.0 : 1022727.0;
    double hz = vi->freq * clk / 16777216.0;
    char note[16] = "--", hzs[48] = "";
    if (vi->freq && hz > 8) {
        double m = 69 + 12 * log2(hz / 440.0);
        int mi = (int)floor(m + 0.5);
        if (mi >= 0 && mi < 128) snprintf(note, sizeof note, "%s%d", NOTE[mi % 12], mi / 12 - 1);
        snprintf(hzs, sizeof hzs, "%.1f Hz", hz);
        double cents = (m - mi) * 100;
        if (fabs(cents) >= 5) {
            char cs[24];
            snprintf(cs, sizeof cs, "  %+.0f ct", cents);
            strncat(hzs, cs, sizeof hzs - strlen(hzs) - 1);
        }
    }
    col nc = active && !muted ? mix(TXT, vc, 0.35f) : alpha(FAINT, 1);
    text(x + 16, y + 30, 44, nc, note);
    text(x + 16, y + 82, 13, alpha(DIM, active ? 1.0f : 0.5f), hzs);
    /* chips */
    float cx = x + 16, cy = y + 106;
    cx += chip(cx, cy, "TRI", vc, (vi->ctrl & 0x10) != 0);
    cx += chip(cx, cy, "SAW", vc, (vi->ctrl & 0x20) != 0);
    cx += chip(cx, cy, "PUL", vc, (vi->ctrl & 0x40) != 0);
    cx += chip(cx, cy, "NOI", vc, (vi->ctrl & 0x80) != 0);
    if (vi->ctrl & 0x02) cx += chip(cx, cy, "SYNC", OKC, 1);
    if (vi->ctrl & 0x04) cx += chip(cx, cy, "RING", OKC, 1);
    if (vi->ctrl & 0x08) cx += chip(cx, cy, "TEST", WARN, 1);
    if (vi->ctrl & 0x40) {
        snprintf(buf, sizeof buf, "PW %d%%", (int)(vi->pw * 100 / 4095));
        text_right(x + w - 16, y + 82, 13, alpha(DIM, active ? 1.0f : 0.5f), buf);
    }
    /* scope */
    float sx = x + 14, sy = y + 136, sw = w - 28, sh = 56;
    rrect(sx, sy, sw, sh, 8, C(0.03f, 0.04f, 0.07f, 1));
    rect(sx + 6, sy + sh / 2, sw - 12, 1, alpha(FAINT, 0.25f));
    const uint8_t *wv = vs->wave[0][v];
    double mean = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) mean += wv[i];
    mean /= NGSID_BLOCK;
    int trig = 0;
    for (int i = 0; i < 380; i++) if (wv[i] <= mean && wv[i + 1] > mean) { trig = i; break; }
    float pk = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) { float d = (float)fabs(wv[i] - mean); if (d > pk) pk = d; }
    float target = pk < 20 ? 20 : pk;
    if (target > scope_amp[v]) scope_amp[v] = target; else scope_amp[v] += (target - scope_amp[v]) * 0.08f;
    enum { NP = 280 };
    float pts[NP * 2];
    for (int i = 0; i < NP; i++) {
        float val = (float)(wv[trig + i * 2] - mean) / scope_amp[v];
        if (muted) val = 0;
        pts[2 * i] = sx + 6 + (sw - 12) * (float)i / (NP - 1);
        pts[2 * i + 1] = sy + sh / 2 - val * (sh / 2 - 6);
    }
    col lc = muted ? alpha(FAINT, 1) : vc;
    set_additive(1);
    polyline(pts, NP, 5.0f, 3.5f, alpha(lc, 0.30f));
    set_additive(0);
    polyline(pts, NP, 1.5f, 3.0f, mix(lc, C(1, 1, 1, 1), 0.35f));
    /* envelope */
    float ey = y + h - 22, ew = w - 32 - 70;
    rrect(x + 16, ey, ew, 6, 3, alpha(FAINT, 0.35f));
    float lvl = vs->running ? vi->level / 127.0f : 0;
    if (lvl > 0.005f) rrect(x + 16, ey, ew * lvl < 6 ? 6 : ew * lvl, 6, 3, alpha(muted ? FAINT : vc, 0.95f));
    static const char *ST[4] = { "ATTACK", "DECAY", "HOLD", "RELEASE" };
    const char *st = (vi->level == 0 && !(vi->ctrl & 1)) ? "OFF" : ST[vi->state & 3];
    text_right(x + w - 16, ey - 5, 11, alpha(DIM, 0.9f), st);
    /* mute overlay */
    if (muted) {
        rrect(x, y, w, h, 14, C(0.03f, 0.04f, 0.07f, 0.55f));
        text(x + w / 2 - text_width("MUTED", 22) / 2, y + 62, 22, VC[2], "MUTED");
    }
    (void)t;
}


/* Compact voice card for multi-SID tunes: one row per chip, three cards per row. */
static void draw_voice_compact(int c, int v, float x, float y, float w, float h, const vis_t *vs) {
    const ngsid_voice_info *vi = &vs->vi[c][v];
    const col vc = VC[v];
    const int muted = (g_mute >> v) & 1, tall = h >= 96;
    const int active = ((vi->ctrl & 1) || vi->level > 0) && vs->running;
    card(x, y, w, h, hit(x, y, w, h));
    char buf[48];
    snprintf(buf, sizeof buf, "SID %d  VOICE %d", c + 1, v + 1);
    text(x + 14, y + 8, 11, DIM, buf);
    float lx = x + w - 18, ly = y + 14;
    rrect(lx - 4, ly - 4, 8, 8, 4, alpha(FAINT, 0.5f));
    if ((vi->ctrl & 1) && vs->running) {
        set_additive(1); blob(lx, ly, 8, alpha(vc, 0.55f)); set_additive(0);
        rrect(lx - 3.5f, ly - 3.5f, 7, 7, 3.5f, vc);
    }
    double clk = g_tune && g_tune->pal ? 985248.0 : 1022727.0;
    double hz = vi->freq * clk / 16777216.0;
    char note[16] = "--", hzs[48] = "";
    if (vi->freq && hz > 8) {
        double m = 69 + 12 * log2(hz / 440.0);
        int mi = (int)floor(m + 0.5);
        if (mi >= 0 && mi < 128) snprintf(note, sizeof note, "%s%d", NOTE[mi % 12], mi / 12 - 1);
        snprintf(hzs, sizeof hzs, "%.1f Hz", hz);
    }
    col nc = active && !muted ? mix(TXT, vc, 0.35f) : alpha(FAINT, 1);
    const int narrow = w < 200;           /* three cards across: smaller note, scope starts further right */
    float ns = narrow ? 20 : (tall ? 30 : 24);
    text(x + 14, y + 21 + (narrow ? 4 : 0), ns, nc, note);
    text(x + 14, y + 21 + ns + 3 + (narrow ? 4 : 0), 11, alpha(DIM, active ? 1.0f : 0.5f), hzs);
    /* mini scope, right of the note */
    float sx = x + w * (narrow ? 0.54f : 0.45f), sy = y + 24, sw = x + w - 14 - sx, sh = tall ? 40 : 30;
    rrect(sx, sy, sw, sh, 6, C(0.03f, 0.04f, 0.07f, 1));
    rect(sx + 4, sy + sh / 2, sw - 8, 1, alpha(FAINT, 0.25f));
    const uint8_t *wv = vs->wave[c][v];
    double mean = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) mean += wv[i];
    mean /= NGSID_BLOCK;
    int trig = 0;
    for (int i = 0; i < 380; i++) if (wv[i] <= mean && wv[i + 1] > mean) { trig = i; break; }
    float pk = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) { float d = (float)fabs(wv[i] - mean); if (d > pk) pk = d; }
    float target = pk < 20 ? 20 : pk;
    float *amp = &scope_amp[c * 3 + v];
    if (target > *amp) *amp = target; else *amp += (target - *amp) * 0.08f;
    enum { NP = 120 };
    float pts[NP * 2];
    for (int i = 0; i < NP; i++) {
        float val = (float)(wv[trig + i * 3] - mean) / *amp;
        if (muted) val = 0;
        pts[2 * i] = sx + 4 + (sw - 8) * (float)i / (NP - 1);
        pts[2 * i + 1] = sy + sh / 2 - val * (sh / 2 - 4);
    }
    col lc = muted ? alpha(FAINT, 1) : vc;
    set_additive(1);
    polyline(pts, NP, 4.0f, 3.0f, alpha(lc, 0.30f));
    set_additive(0);
    polyline(pts, NP, 1.3f, 3.0f, mix(lc, C(1, 1, 1, 1), 0.35f));
    if (tall) {
        float cx = x + 14, cy = y + h - 36;
        cx += chip(cx, cy, "TRI", vc, (vi->ctrl & 0x10) != 0);
        cx += chip(cx, cy, "SAW", vc, (vi->ctrl & 0x20) != 0);
        cx += chip(cx, cy, "PUL", vc, (vi->ctrl & 0x40) != 0);
        cx += chip(cx, cy, "NOI", vc, (vi->ctrl & 0x80) != 0);
    }
    /* envelope */
    float ey = y + h - 12, ew = w - 28;
    rrect(x + 14, ey, ew, 4, 2, alpha(FAINT, 0.35f));
    float lvl = vs->running ? vi->level / 127.0f : 0;
    if (lvl > 0.005f) rrect(x + 14, ey, ew * lvl < 4 ? 4 : ew * lvl, 4, 2, alpha(muted ? FAINT : vc, 0.95f));
    if (muted) {
        rrect(x, y, w, h, 14, C(0.03f, 0.04f, 0.07f, 0.55f));
        text(x + w / 2 - text_width("MUTED", 16) / 2, y + h / 2 - 9, 16, VC[2], "MUTED");
    }
}

static void draw_notes(float x, float y, float w, float h, const vis_t *vs, float dtf) {
    (void)dtf;
    card(x, y, w, h, 0);
    text(x + 18, y + 10, 12, DIM, "NOTES");
    tune_t *tn = g_tune;
    if (!tn || !tn->e) return;
    static uint64_t prev_frames;
    const double rowf = (double)g_rate / NT_RATE;
    if (la_tune != tn || vs->frames < prev_frames) la_create(tn);              /* another tune, or the time went back (restart): a new look-ahead engine */
    prev_frames = vs->frames;
    const double now = (double)vs->frames / rowf;                               /* the row that sounds now, with the part of it that has passed */
    const uint64_t target = (uint64_t)((now + LA_ROWS) * rowf);
    la_advance(target, 10.0);
    if (la_e && la_frames + (uint64_t)(2 * rowf) < target) text_right(x + w - 18, y + 10, 11, DIM, "reading ahead ...");
    const int nsid = tn->nsid > 1 ? (tn->nsid > 3 ? 3 : tn->nsid) : 1, ncols = 3 * nsid;
    const float x0 = x + 14, cw = (w - 28) / (float)ncols, hy = y + 30, ry0 = hy + 26, rh = 24, bottom = y + h - 10;
    for (int k = 0; k < ncols; k++) {                                     /* column heads: a click mutes the voice like on its card */
        int c = k / 3, v = k % 3, muted = (g_mute >> v) & 1;
        float hx = x0 + k * cw + 2, hw = cw - 4;
        int hv = hit(hx, hy, hw, 20);
        rrect(hx, hy, hw, 20, 6, alpha(muted ? WARN : VC[v], muted ? 0.22f : (hv ? 0.28f : 0.16f)));
        char lb[24]; if (nsid == 1) snprintf(lb, sizeof lb, "VOICE %d", v + 1); else snprintf(lb, sizeof lb, "SID %d  V%d", c + 1, v + 1);
        text(hx + (hw - text_width(lb, 11)) / 2, hy + 4, 11, muted ? WARN : alpha(DIM, 1.0f), lb);
        if (clicked(hx, hy, hw, 20)) { g_mute ^= 1u << v; apply_mute(); click = 0; }
    }
    int nvis = (int)((bottom - ry0) / rh);
    if (nvis < 3) return;
    const int half = nvis / 2;
    const float ybar = ry0 + (float)half * rh;                              /* the bar: the row that sounds now, in the middle like in ngPlayer */
    rrect(x + 8, ybar - 2, w - 16, rh, 6, alpha(VC[0], 0.14f));
    const int64_t r0 = (int64_t)floor(now);
    for (int64_t r = r0 - half - 1; r <= r0 + half + 2; r++) {
        if (r < 0) continue;
        float yy = ybar + (float)((double)r - now) * rh;
        if (yy < ry0 - 2 || yy + rh > bottom) continue;
        int slot = (int)(r % NT_ROWS), valid = nt_rowid[slot] == r;
        float fade = 1.0f - 0.6f * fabsf(yy - ybar) / ((float)half * rh + 1);
        for (int k = 0; k < ncols; k++) {
            nt_cell cc = valid ? nt_cells[k][slot] : (nt_cell){ -1, 0 };
            int v = k % 3, muted = (g_mute >> v) & 1;
            float tx = x0 + k * cw + 8, al = (muted ? 0.35f : 1.0f) * fade;
            char b[16];
            if (cc.kind == 1) { snprintf(b, sizeof b, "%s%d", NOTE[cc.note % 12], cc.note / 12 - 1); text(tx, yy, 16, alpha(mix(TXT, VC[v], 0.35f), al), b); }
            else if (cc.kind == 3) text(tx, yy, 16, alpha(VC[v], al), "NOI");
            else if (cc.kind == 2) text(tx, yy, 16, alpha(WARN, al * 0.85f), "===");
            else if (cc.kind == 4) text(tx + 4, yy, 16, alpha(VC[v], al * 0.35f), "|");
            else text(tx, yy, 16, alpha(FAINT, al * 0.5f), "...");
        }
    }
}

static float digi_dmax = 32;

/* The digi display normally shows the $D418 volume-register channel. Tunes that play samples
 * through an oscillator (waveform switching, test bit) are shown there too: -1 = $D418 digi,
 * 0..2 = that voice. */
static int digi_source(const vis_t *vs) {
    int act = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) if (vs->digi[i]) { act = 1; break; }
    if (act || !vs->running) return -1;
    int best = -1, bn = NGSID_BLOCK / 4;
    for (int v = 0; v < 3; v++) if (vs->hold[v] > bn) { bn = vs->hold[v]; best = v; }
    return best;
}

static void draw_digi(float x, float y, float w, float h, const vis_t *vs) {
    col dc = C(0.30f, 0.95f, 0.62f, 1);
    const int src = digi_source(vs);
    int muted = src >= 0 ? (g_mute >> src) & 1 : (g_mute >> 3) & 1;
    card(x, y, w, h, hit(x, y, w, h));
    const uint8_t *d = src >= 0 ? vs->wave[0][src] : vs->digi;
    int changes = 0, active = 0, last = d[0];
    for (int i = 0; i < NGSID_BLOCK; i++) {
        if (d[i] != last) { changes++; last = d[i]; }
        if (d[i]) active = 1;
    }
    if (!vs->running) { changes = 0; active = 0; }
    float rate = (float)changes * (float)g_rate / NGSID_BLOCK;
    const int compact = h < 80;           /* portrait: everything has to fit into a 46..64 px card */
    text(x + 16, y + (compact ? 8 : 12), 13, DIM, "DIGI CHANNEL");
    {
        char sb[40];
        snprintf(sb, sizeof sb, "voice %d oscillator", src + 1);
        if (!compact) text(x + 16, y + 30, 11, FAINT, "Samples via the");
        if (!compact || !active) text(x + 16, y + (compact ? 26 : 44), 11, FAINT, src >= 0 ? sb : (compact ? "via volume register" : "volume register"));
    }
    char b[48];
    snprintf(b, sizeof b, "%.1f kHz", rate / 1000.0f);
    if (compact) { if (active) text(x + 16, y + 26 + 0, 11, alpha(dc, muted ? 0.4f : 1.0f), b); }   /* replaces the description while samples play */
    else text(x + 16, y + 66, 13, alpha(active && !muted ? dc : DIM, active ? 1.0f : 0.5f), active ? b : "--");
    float lx = compact ? x + 150 : x + 130, ly = y + (compact ? 14 : 21);
    rrect(lx - 6, ly - 6, 12, 12, 6, alpha(FAINT, 0.5f));
    if (active && !muted) { set_additive(1); blob(lx, ly, 10, alpha(dc, 0.55f)); set_additive(0); rrect(lx - 5, ly - 5, 10, 10, 5, dc); }

    float sx = x + 160, sy = y + 12, sw = w - 176, sh = h - 24;
    rrect(sx, sy, sw, sh, 8, C(0.03f, 0.04f, 0.07f, 1));
    for (int g = 1; g < 4; g++) rect(sx + 6, sy + sh * g / 4.0f, sw - 12, 1, alpha(FAINT, 0.18f));
    double dmean = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) dmean += d[i];
    dmean /= NGSID_BLOCK;
    float dpk = 0;
    for (int i = 0; i < NGSID_BLOCK; i++) { float a = (float)fabs(d[i] - dmean); if (a > dpk) dpk = a; }
    float target = dpk < 3 ? 3.0f : dpk;       /* AC coupled: the sample steps ride on a DC level */
    if (target > digi_dmax) digi_dmax = target; else digi_dmax += (target - digi_dmax) * 0.06f;
    enum { NS = 520 };
    static float pts[NS * 4 + 4];
    int np = 0;
    float x0 = sx + 6, wid = sw - 12, top = sy + 8, bot = sy + sh - 8;
    for (int i = 0; i < NS; i++) {
        float v = muted ? 0 : (float)(d[i] - dmean) / digi_dmax;
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        float yy = (top + bot) / 2 - v * (bot - top) / 2;
        float xa = x0 + wid * (float)i / NS, xb = x0 + wid * (float)(i + 1) / NS;
        pts[np * 2] = xa; pts[np * 2 + 1] = yy; np++;
        pts[np * 2] = xb; pts[np * 2 + 1] = yy; np++;
    }
    col lc = muted ? alpha(FAINT, 1) : dc;
    set_additive(1);
    polyline(pts, np, 4.5f, 3.5f, alpha(lc, 0.26f));
    set_additive(0);
    polyline(pts, np, 1.2f, 3.0f, mix(lc, C(1, 1, 1, 1), 0.3f));
    if (!muted)   /* sample points: a small square wherever the level changes */
        for (int i = 1; i < NS; i++)
            if (d[i] != d[i - 1]) {
                float v = (float)(d[i] - dmean) / digi_dmax; if (v > 1) v = 1; if (v < -1) v = -1;
                float xx = x0 + wid * (float)i / NS, yy = (top + bot) / 2 - v * (bot - top) / 2;
                rect(xx - 1.5f, yy - 1.5f, 3, 3, mix(dc, C(1, 1, 1, 1), 0.6f));
            }
    if (muted) {
        rrect(x, y, w, h, 14, C(0.03f, 0.04f, 0.07f, 0.55f));
        text(x + w / 2 - text_width("MUTED", 22) / 2, y + h / 2 - 14, 22, VC[2], "MUTED");
    }
}

static int g_portrait, g_plopen;      /* portrait (phones): stacked layout, playlist/browser as overlay */
static int g_orb;                       /* 0 = voice cards, 1 = orb, 2 = notes, 3 = info (key V / switch in the filter strip) */
static float orb_bs[NBANDS];

/* ================================================================== Orb: geodesic net, deformed by the spectrum
 * Icosahedron subdivided twice (162 vertices, 480 edges, 320 triangles) with a little jitter. Vertices and faces are pushed
 * outwards by the spectrum and light up; equator = bass, poles = treble. */
#define OB_V 162
#define OB_F 320
#define OB_E 480
static float ob_u[OB_V][3];
static int ob_f[OB_F][3], ob_e[OB_E][2], ob_nv, ob_nf, ob_ne;

static int ob_mid(int a, int b) {       /* midpoint of two vertices (looked up so vertices are not duplicated) */
    float m[3];
    for (int k = 0; k < 3; k++) m[k] = 0.5f * (ob_u[a][k] + ob_u[b][k]);
    float l = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    for (int k = 0; k < 3; k++) m[k] /= l;
    for (int i = 0; i < ob_nv; i++)
        if (fabsf(ob_u[i][0] - m[0]) + fabsf(ob_u[i][1] - m[1]) + fabsf(ob_u[i][2] - m[2]) < 1e-4f) return i;
    for (int k = 0; k < 3; k++) ob_u[ob_nv][k] = m[k];
    return ob_nv++;
}
static void ob_init(void) {
    const float g = 1.61803399f;
    static const float iv[12][3] = { {-1, 1, 0}, {1, 1, 0}, {-1, -1, 0}, {1, -1, 0}, {0, -1, 1}, {0, 1, 1}, {0, -1, -1}, {0, 1, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, -1}, {-1, 0, 1} };
    static const int itri[20][3] = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
                                     {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
    (void)g;
    ob_nv = 12; ob_nf = 20;
    for (int i = 0; i < 12; i++) {
        float l = sqrtf(iv[i][0] * iv[i][0] + iv[i][1] * iv[i][1] + iv[i][2] * iv[i][2]);
        for (int k = 0; k < 3; k++) ob_u[i][k] = iv[i][k] / l;
    }
    for (int i = 0; i < 20; i++) for (int k = 0; k < 3; k++) ob_f[i][k] = itri[i][k];
    for (int pass = 0; pass < 2; pass++) {
        static int nf2[OB_F][3];
        int n2 = 0;
        for (int i = 0; i < ob_nf; i++) {
            int a = ob_f[i][0], b = ob_f[i][1], c = ob_f[i][2];
            int ab = ob_mid(a, b), bc = ob_mid(b, c), ca = ob_mid(c, a);
            int t[4][3] = { {a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca} };
            for (int k = 0; k < 4; k++) { nf2[n2][0] = t[k][0]; nf2[n2][1] = t[k][1]; nf2[n2][2] = t[k][2]; n2++; }
        }
        for (int i = 0; i < n2; i++) for (int k = 0; k < 3; k++) ob_f[i][k] = nf2[i][k];
        ob_nf = n2;
    }
    unsigned seed = 12345u;                              /* slight jitter: looks more organic than the smooth grid */
    for (int i = 0; i < ob_nv; i++) {
        for (int k = 0; k < 3; k++) { seed = seed * 1664525u + 1013904223u; ob_u[i][k] += ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.10f; }
        float l = sqrtf(ob_u[i][0] * ob_u[i][0] + ob_u[i][1] * ob_u[i][1] + ob_u[i][2] * ob_u[i][2]);
        for (int k = 0; k < 3; k++) ob_u[i][k] /= l;
    }
    ob_ne = 0;                                           /* edges: each triangle side once */
    for (int i = 0; i < ob_nf; i++)
        for (int k = 0; k < 3; k++) {
            int a = ob_f[i][k], b = ob_f[i][(k + 1) % 3], dup = 0;
            if (a > b) { int tmp = a; a = b; b = tmp; }
            for (int j = 0; j < ob_ne; j++) if (ob_e[j][0] == a && ob_e[j][1] == b) { dup = 1; break; }
            if (!dup && ob_ne < OB_E) { ob_e[ob_ne][0] = a; ob_e[ob_ne][1] = b; ob_ne++; }
        }
}

static void open_deepsid(const char *rel);
#include "c64.inc"
static void draw_orb(float x, float y, float w, float h, float dtf) {
    static int init;
    static float t, spin;
    if (!init) { ob_init(); init = 1; }
    float dt = dtf > 0.1f ? 0.1f : dtf;
    float beat = g_paused ? 0.0f : g_beat;
    if (!g_paused) { t += dt; spin += dt * (0.16f + 0.35f * beat); }
    card(x, y, w, h, 0);
    text(x + 18, y + 10, 12, DIM, "ORB");
    for (int b = 0; b < NBANDS; b++) {      /* mix neighbouring bands: softer waves */
        float l = band[b > 0 ? b - 1 : b], r = band[b < NBANDS - 1 ? b + 1 : b];
        float v = 0.5f * band[b] + 0.25f * (l + r);
        orb_bs[b] += (v - orb_bs[b]) * (v > orb_bs[b] ? 0.7f : 0.25f);
    }
    float cx = x + w / 2, cy = y + h / 2 + 6, R = 0.34f * (w < h ? w : h);
    float ca = cosf(spin), sa = sinf(spin), ct = cosf(0.42f), st = sinf(0.42f), fz = 8.0f * R;
    static float sx[OB_V], sy[OB_V], dn[OB_V], ev[OB_V];
    for (int i = 0; i < ob_nv; i++) {
        float ux = ob_u[i][0], uy = ob_u[i][1], uz = ob_u[i][2];
        float lat = fabsf(uy), bf = lat * (NBANDS - 1);      /* equator = bass, poles = treble */
        int b0 = (int)bf;
        if (b0 > NBANDS - 2) b0 = NBANDS - 2;
        float fr = bf - b0, lon = atan2f(uz, ux);
        float e = (orb_bs[b0] * (1 - fr) + orb_bs[b0 + 1] * fr) * (0.65f + 0.35f * sinf(lon * 3.0f + t * 1.3f + lat * 5.0f));
        if (e > 1) e = 1;
        ev[i] = e;
        float rad = R * (1.0f + 0.06f * beat + 0.36f * e);
        float px = ux * rad, py = uy * rad, pz = uz * rad;
        float rx = px * ca + pz * sa, rz = -px * sa + pz * ca;
        float ry = py * ct - rz * st, rz2 = py * st + rz * ct;
        float sc = fz / (fz + rz2);
        sx[i] = cx + rx * sc; sy[i] = cy + ry * sc;
        float d = 0.5f - 0.5f * rz2 / (1.5f * R);             /* 1 = front, 0 = back */
        dn[i] = d < 0 ? 0 : (d > 1 ? 1 : d);
    }
    set_additive(1);
    blob(cx, cy, R * (0.60f + 0.12f * beat), alpha(VC[0], 0.05f + 0.08f * beat));
    for (int f = 0; f < ob_nf; f++) {                        /* translucent faces, by the energy of their three vertices */
        int a = ob_f[f][0], b = ob_f[f][1], c = ob_f[f][2];
        float e = (ev[a] + ev[b] + ev[c]) * (1.0f / 3.0f), d = (dn[a] + dn[b] + dn[c]) * (1.0f / 3.0f);
        float al = (0.012f + 0.30f * e * e) * (0.25f + 0.75f * d);
        if (al < 0.01f) continue;
        col col0 = e < 0.5f ? mix(VC[0], VC[1], e * 2.0f) : mix(VC[1], VC[2], (e - 0.5f) * 2.0f);
        tri(sx[a], sy[a], sx[b], sy[b], sx[c], sy[c], alpha(mix(col0, C(1, 1, 1, 1), 0.25f), al));
    }
    for (int k = 0; k < ob_ne; k++) {                        /* edges */
        int a = ob_e[k][0], b = ob_e[k][1];
        float e = 0.5f * (ev[a] + ev[b]), d = 0.5f * (dn[a] + dn[b]);
        col c = e < 0.5f ? mix(VC[0], VC[1], e * 2.0f) : mix(VC[1], VC[2], (e - 0.5f) * 2.0f);
        c = mix(c, C(1, 1, 1, 1), 0.45f);
        float xy[4] = { sx[a], sy[a], sx[b], sy[b] };
        polyline(xy, 2, 0.55f + 0.5f * d, 3.0f, alpha(c, (0.10f + 0.50f * d) * (0.55f + 0.9f * e)));
    }
    for (int i = 0; i < ob_nv; i++) {                        /* vertices: bright core plus glow */
        float e = ev[i], d = dn[i];
        col c = e < 0.5f ? mix(VC[0], VC[1], e * 2.0f) : mix(VC[1], VC[2], (e - 0.5f) * 2.0f);
        c = mix(c, C(1, 1, 1, 1), 0.55f);
        float rr = 0.9f + 1.5f * d + 1.6f * e;
        rrect(sx[i] - rr, sy[i] - rr, rr * 2, rr * 2, rr, alpha(c, 0.25f + 0.70f * d));
        if (e > 0.12f) blob(sx[i], sy[i], 3.0f + 5.0f * e, alpha(c, 0.30f * e * (0.3f + 0.7f * d)));
    }
    set_additive(0);
}

/* ---- playlist / HVSC browser panel (sidebar in landscape, full overlay in portrait: tap selects, drag scrolls) */
static int sb_pd, sb_moved, sb_tap, sb_pressed;
static int ctx_swallow;                                  /* a tap that hit the context menu (or closed it) must not select the row below when the finger lifts */
static int lp_on, lp_fired, lp_swallow, lp_x, lp_y;     /* long press on a touch screen = right click */
static Uint32 lp_t;
static float sb_py0, sb_px0, sb_sc0;
static int sb_click(float x, float y, float w, float h) {
    if (!g_portrait && !g_mobile) return clicked(x, y, w, h);
    return sb_tap && mx >= x && mx < x + w && my >= y && my < y + h;
}
/* ---- Share / Info: right click on an HVSC tune (playlist entry fetched online, or a file in the HVSC browser) */
static void url_enc_path(const char *in, char *out, size_t cap) {    /* percent-encode everything except unreserved characters and '/' */
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') out[o++] = (char)c;
        else { snprintf(out + o, 4, "%%%02X", c); o += 3; }
    }
    out[o] = 0;
}
static void open_url(const char *url) {
#ifdef __EMSCRIPTEN__
    EM_ASM({ window.open(UTF8ToString($0), '_blank'); }, url);
#else
    SDL_OpenURL(url);
#endif
}
static void ctx_share(void) {
    char enc[1024], url[1100];
    url_enc_path(ctx_rel, enc, sizeof enc);
    snprintf(url, sizeof url, "https://ngsid.org/player/#/%s", enc);
#ifdef __EMSCRIPTEN__
    /* SDL's clipboard does not reach the browser clipboard from the main loop: use the async API, with an execCommand fallback */
    EM_ASM({
        var t = UTF8ToString($0);
        var fb = function () {
            var a = document.createElement('textarea');
            a.value = t; a.style.position = 'fixed'; a.style.opacity = '0';
            document.body.appendChild(a); a.focus(); a.select();
            try { document.execCommand('copy'); } catch (e) {}
            document.body.removeChild(a);
        };
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(t).catch(fb); else fb();
    }, url);
#else
    SDL_SetClipboardText(url);
#endif
    snprintf(toast_txt, sizeof toast_txt, "Link copied"); toast_t = 2.0f;
}
static void open_deepsid(const char *rel) {                       /* the tune on DeepSID (composer, STIL info, song list) */
    char enc[1024], url[1100];
    url_enc_path(rel, enc, sizeof enc);
    snprintf(url, sizeof url, "https://deepsid.chordian.net/?file=/%s", enc);
    open_url(url);
}
static void ctx_info(void) { open_deepsid(ctx_rel); }
static void ctx_open(const char *rel) {
    snprintf(ctx_rel, sizeof ctx_rel, "%s", rel);
    ctx_on = 1; rclick = 0;
    ctx_x = (float)rclickx; ctx_y = (float)rclicky;
    if (g_mobile || g_portrait) ctx_y -= ctx_h() + 18;          /* touch: the menu opens above the finger */
    if (ctx_y < 4) ctx_y = 4;
    if (ctx_x + CTX_W > view_w - 4) ctx_x = view_w - 4 - CTX_W;
    if (ctx_y + ctx_h() > view_h - 4) ctx_y = view_h - 4 - ctx_h();
}
#if defined(__APPLE__) && TARGET_OS_TV
static void tv_ctx_draw(float W, float H) {                              /* the menu (Select held): a panel in the middle, the chosen row marked */
    if (!tv_ctx_open) return;
    const char *fav_txt = fav_find(tv_ctx_rel) >= 0 ? "Remove from favorites" : "Add to favorites";
    const char *nm[5] = { "", fav_txt, "Show favorites", "Show the HVSC browser", "Cancel" };
    float rh = 54, pw = 560, ph = 24 + (float)tv_ctx_n * rh + 24, px = (W - pw) / 2, py = (H - ph) / 2;
    rect(0, 0, W, H, alpha(BG0, 0.55f));
    rrect(px - 1, py - 1, pw + 2, ph + 2, 15, alpha(EDGE, 0.9f)); rrect(px, py, pw, ph, 14, CARDHI);
    for (int j = 0; j < tv_ctx_n; j++) {
        float yy = py + 24 + (float)j * rh;
        if (j == tv_ctx_sel) rrect(px + 10, yy, pw - 20, rh - 6, 9, alpha(VC[0], 0.30f));
        text(px + 34, yy + (rh - 6 - 22) / 2, 22, j == tv_ctx_sel ? TXT : DIM, nm[tv_ctx_it[j]]);
    }
}
#endif
static void draw_ctx_menu(double dt) {
    if (ctx_on) {
        const char *const item[3] = { "Share", "Info", fav_find(ctx_rel) >= 0 ? "Remove Favorite" : "Add Favorite" };
        rrect(ctx_x - 1, ctx_y - 1, CTX_W + 2, ctx_h() + 2, 9, alpha(EDGE, 0.9f));
        rrect(ctx_x, ctx_y, CTX_W, ctx_h(), 8, CARDHI);
        for (int j = 0; j < ctx_n(); j++) {
            float iy = ctx_y + 1 + j * CTX_ROW;
            int hv = mx >= ctx_x && mx < ctx_x + CTX_W && my >= iy && my < iy + CTX_ROW;
            if (hv) rrect(ctx_x + 3, iy + 2, CTX_W - 6, CTX_ROW - 4, 6, alpha(VC[0], 0.22f));
            text(ctx_x + 14, iy + 7, 14, hv ? TXT : DIM, item[j]);
        }
    }
    if (toast_t > 0) {
        toast_t -= (float)dt;
        float w = text_width(toast_txt, 14) + 32, x = (view_w - w) / 2, y = view_h - 70;
        rrect(x, y, w, 32, 10, alpha(CARDHI, 0.96f));
        text(x + 16, y + 8, 14, OKC, toast_txt);
    }
}
static void draw_sidebar(float x, float y, float w, float h) {
    sb_tap = 0; sb_pressed = 0;
    const int touch = g_portrait || g_mobile;        /* touch: drag scrolls, a tap (no drag) selects */
    if (touch) {
        if (mouse_down) {
            if (!sb_pd && mx >= x && mx < x + w && my >= y && my < y + h) { sb_pd = 1; sb_py0 = my; sb_px0 = mx; sb_moved = 0; sb_pressed = 1; }
            if (sb_pd && (fabsf(my - sb_py0) > 8 || fabsf(mx - sb_px0) > 8)) sb_moved = 1;
        } else if (sb_pd) {
            sb_pd = 0;
            if (browse && !fav_mode && !br_at_root() && !lp_swallow && !ctx_swallow && mx - sb_px0 > 70 && mx - sb_px0 > 1.6f * fabsf(my - sb_py0)) browse_up();     /* a swipe to the right: one level up (like the back gesture of iOS) */
            else if (!sb_moved && !lp_swallow && !ctx_swallow) sb_tap = 1;
            lp_swallow = 0; ctx_swallow = 0;
        }
        else ctx_swallow = 0;
    }
        card(x, y, w, h, 0);
        float tw1 = text_width("PLAYLIST", 13), tw2 = text_width("HVSC ONLINE", 13), tw3 = text_width("FAV", 13);
        float t2 = x + 18 + tw1 + 22, t3 = t2 + tw2 + 22;
        int in_pl = !browse && !fav_mode;
        if (NG_TV) {
#if defined(__APPLE__) && TARGET_OS_TV
            if (fav_mode) { browse = 0; text(x + 18, y + 12, 20, TXT, "FAVORITES"); }
            else
#endif
            {
            browse = 1; fav_mode = 0;
            text(x + 18, y + 12, 20, TXT, "HVSC");
            text(x + 18 + text_width("HVSC", 20) + 12, y + 14, 14, VC[2], "MUSICIANS");
            }
        } else
        if (g_portrait) {                 /* touch: real tab buttons instead of plain text */
            rrect(x + 8, y + 6, tw1 + 20, 30, 8, in_pl ? alpha(VC[2], 0.25f) : alpha(FAINT, 0.16f));
            rrect(t2 - 10, y + 6, tw2 + 20, 30, 8, browse ? alpha(VC[2], 0.25f) : alpha(FAINT, 0.16f));
            rrect(t3 - 10, y + 6, tw3 + 20, 30, 8, fav_mode ? alpha(VC[2], 0.25f) : alpha(FAINT, 0.16f));
        }
        if (!NG_TV) {
        text(x + 18, y + 14, 13, in_pl ? TXT : FAINT, "PLAYLIST");
        text(t2, y + 14, 13, browse ? TXT : FAINT, "HVSC ONLINE");
        text(t3, y + 14, 13, fav_mode ? TXT : FAINT, "FAV");
        if (sb_click(x + 8, y + 6, tw1 + 20, 30)) { browse = 0; fav_mode = 0; click = 0; }
        else if (sb_click(t2 - 10, y + 6, tw2 + 20, 30)) { browse_open(); click = 0; }
        else if (sb_click(t3 - 10, y + 6, tw3 + 20, 30)) { browse = 0; fav_mode = 1; click = 0; }
        }
        if (g_portrait && !g_duo) {       /* second way out of the overlay: close button (the Duo list stays open) */
            float bx = x + w - 42, by2 = y + 6;
            int hv = hit(bx, by2, 34, 30);
            rrect(bx, by2, 34, 30, 8, alpha(FAINT, hv ? 0.30f : 0.16f));
            text(bx + 11, by2 + 7, 14, TXT, "X");
            if (sb_click(bx - 6, by2, 46, 30)) { g_plopen = 0; click = 0; }
        }
        int boff = br_at_root() ? 0 : 1;
        int count = fav_mode ? nfav : browse ? nbe + boff : npl;
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%d", fav_mode ? nfav : browse ? nbe : npl);
        text_right(x + w - (g_portrait && !g_duo ? 54 : 18), y + 14, 13, DIM, cnt);
        float ry = y + 44, rh = touch ? 36 : 28, area = h - 44 - 40;
        if (browse) {
            char crumb[800];
            snprintf(crumb, sizeof crumb, "/%s", rpath);
            if (!NG_TV && !br_at_root()) {    /* a fixed bar above the list (it does not scroll away): a tap goes one level up, the path of the folder is in it */
                const float bh = 30;
                int hv = hit(x + 10, ry - 4, w - 20, bh);
                rrect(x + 10, ry - 4, w - 20, bh, 8, alpha(VC[2], hv ? 0.28f : 0.16f));
                text(x + 20, ry + 2, 14, TXT, "<");
                fit_text(crumb, w - 36 - 22, 12);
                text(x + 38, ry + 3, 12, DIM, crumb);
                if (sb_click(x + 10, ry - 4, w - 20, bh)) { browse_up(); click = 0; }
                ry += bh + 6; area -= bh + 6 + 18;
            } else {
                fit_text(crumb, w - 36, 12);
                text(x + 18, ry, 12, FAINT, crumb);
                ry += 22; area -= 22 + 18;
            }
        }
        float *sc = fav_mode ? &fav_scroll : browse ? &br_scroll : &list_scroll;
        int fc = fav_mode ? fav_cur() : -1;
        if (sb_pressed) sb_sc0 = *sc;
        if (touch && sb_pd && sb_moved) *sc = sb_sc0 + (sb_py0 - my) / rh;
        int vis_rows = (int)(area / rh);
        int maxscroll = count > vis_rows ? count - vis_rows : 0;
        if (!browse && !fav_mode && list_follow && cur >= 0) {
            if (cur < *sc) *sc = (float)cur;
            if (cur >= *sc + vis_rows) *sc = (float)(cur - vis_rows + 1);
            list_follow = 0;
        }
        if (browse && br_follow) {
            if (br_sel < *sc) *sc = (float)br_sel;
            if (br_sel >= *sc + vis_rows) *sc = (float)(br_sel - vis_rows + 1);
            br_follow = 0;
        }
        if (*sc > maxscroll) *sc = (float)maxscroll;
        if (*sc < 0) *sc = 0;
        int first = (int)*sc;
        for (int r = 0; r < vis_rows + 1 && first + r < count; r++) {
            int i = first + r;
            float yy = ry + r * rh;
            if (yy + rh > y + h - 36) break;
            int isc = !browse && !fav_mode && i == cur, anc = 0;
            if (fav_mode) isc = i == fc;
#if defined(__APPLE__) && TARGET_OS_TV
            if (fav_mode) isc = i == tv_fav_sel;
#endif
            if (browse) {
                isc = i == browse_cur_row();
                if (!isc && i >= boff && be[i - boff].dir && cur >= 0 && pl[cur].rel) {
                    size_t rl = strlen(rpath), nl = strlen(be[i - boff].name);
                    anc = !strncmp(pl[cur].rel, rpath, rl) && !strncmp(pl[cur].rel + rl, be[i - boff].name, nl) && pl[cur].rel[rl + nl] == '/';
                }
            }
            int hv = hit(x + 8, yy, w - 16, rh);
            if (browse && i == br_sel) rrect(x + 8, yy, w - 16, rh - 2, 8, alpha(EDGE, 0.55f));
            if (isc) {
                rrect(x + 8, yy, w - 16, rh - 2, 8, alpha(VC[0], 0.16f));
                rrect(x + 8, yy + 5, 3, rh - 12, 1.5f, VC[0]);
            } else if (anc) {
                rrect(x + 8, yy, w - 16, rh - 2, 8, alpha(VC[0], 0.07f));
                rrect(x + 8, yy + 5, 3, rh - 12, 1.5f, alpha(VC[0], 0.45f));
            } else if (hv)
                rrect(x + 8, yy, w - 16, rh - 2, 8, alpha(EDGE, 0.25f));
            char lab[260];
            int isdir = 0;
            if (fav_mode)
                snprintf(lab, sizeof lab, "%s", fav[i].label);
            else if (!browse)
                snprintf(lab, sizeof lab, "%s", pl[i].label);
            else if (boff && i == 0) { snprintf(lab, sizeof lab, ".."); isdir = 1; }
            else {
                const hvsc_entry *en = &be[i - boff];
                char tmp[200];
                utf8_to_latin1(en->name, tmp, sizeof tmp);
                size_t tl = strlen(tmp);
                if (!en->dir && tl > 4) tmp[tl - 4] = 0;
                for (char *q = tmp; *q; q++) if (*q == '_') *q = ' ';
                snprintf(lab, sizeof lab, "%s%s", tmp, en->dir ? "/" : "");
                isdir = en->dir;
            }
            float lx = x + 20, lw = w - 52;
            if (fav_mode || (!browse && pl[i].rel)) {          /* tune that came from the online HVSC */
                rrect(lx, yy + 6, 26, 17, 4, alpha(VC[2], isc ? 0.32f : 0.20f));
                text(lx + 4, yy + 8, 11, VC[2], "HV");
                lx += 33; lw -= 33;
            }
            fit_text(lab, lw, 14);
            text(lx, yy + 5, 14, isc || isdir ? TXT : DIM, lab);
            if (rclick && rclickx >= x + 8 && rclickx < x + w - 8 && rclicky >= yy && rclicky < yy + rh) {
                if (fav_mode) ctx_open(fav[i].rel);
                else if (!browse && pl[i].rel) ctx_open(pl[i].rel);
                else if (browse && i >= boff) { char rel[800]; snprintf(rel, sizeof rel, "%s%s%s", rpath, be[i - boff].name, be[i - boff].dir ? "/" : ""); ctx_open(rel); }   /* a folder shares as #/PATH/ */
            }
            if (sb_click(x + 8, yy, w - 16, rh)) {
                if (fav_mode) { if (!SDL_AtomicGet(&job.state)) job_start(1, fav[i].rel); }
                else if (browse) { br_sel = i; browse_activate(i); } else play_index(i, 0);
                click = 0;
                if (g_portrait && (!browse || !isdir)) g_plopen = 0;
            }
        }
        if (fav_mode && !nfav) {
            text(x + 18, y + 60, 14, DIM, "No favorites yet.");
            text(x + 18, y + 84, 13, FAINT, "Right click a tune in");
            text(x + 18, y + 102, 13, FAINT, "HVSC ONLINE: Add Favorite.");
        }
        if (!browse && !fav_mode && !npl) {
            text(x + 18, y + 60, 14, DIM, "No files.");
            text(x + 18, y + 84, 13, FAINT, "Drag SID files or a");
            text(x + 18, y + 102, 13, FAINT, "folder into the window.");
        }
        if (browse) {
            const char *st = SDL_AtomicGet(&job.state) ? "Loading ..." : br_msg;
            if (*st) text(x + 18, y + h - 50, 12, SDL_AtomicGet(&job.state) ? DIM : VC[1], st);
        }
        if (count > vis_rows) {   /* scrollbar */
            float th = area * vis_rows / count;
            if (th < 24) th = 24;
            float ty = ry + (area - th) * (maxscroll ? *sc / maxscroll : 0);
            rrect(x + w - 8, ty, 3, th, 1.5f, alpha(DIM, 0.5f));
        }
        {
            char vb[96];
            snprintf(vb, sizeof vb, "ngSid %s  \xb7  NextGenSid", NGSID_VERSION);
            text(x + 18, y + h - 28, 11, FAINT, vb);
            if (SDL_AtomicGet(&upd_state) == 2 && !browse) {
                char ub[96];
                snprintf(ub, sizeof ub, "Update available: %s", upd_tag);
                text(x + 18, y + h - 48, 12, VC[2], ub);
                if (sb_click(x + 8, y + h - 54, w - 16, 20)) { SDL_OpenURL(upd_url); click = 0; }
            }
        }
}

static void draw_spectrum_card(float ax, float sy, float spw, float sh) {
    card(ax, sy, spw, sh, 0);
    text(ax + 18, sy + 12, 12, DIM, "SPECTRUM");
    {
        float bx = ax + 18, bw = spw - 36, by0 = sy + 34, bh = (sh - 34 - 36) * 0.74f, base = by0 + bh;
        for (int g = 0; g <= 4; g++) {
            float gy = by0 + bh * g / 4.0f;
            rect(bx, gy, bw, 1, alpha(FAINT, 0.22f));
        }
        float step = bw / NBANDS, barw = step * 0.68f;
        float refl = (sh - 34 - 36) * 0.26f;
        for (int b = 0; b < NBANDS; b++) {
            float xx = bx + b * step + (step - barw) / 2;
            float hh2 = band[b] * bh;
            float u = (float)b / (NBANDS - 1);
            col lo = u < 0.5f ? mix(VC[0], VC[1], u * 2) : mix(VC[1], VC[2], (u - 0.5f) * 2);
            col top = mix(lo, C(1, 1, 1, 1), 0.35f);
            if (hh2 > 1) {
                rect_grad(xx, base - hh2, barw, hh2, top, alpha(lo, 0.35f));
                float rh2 = hh2 * 0.35f < refl ? hh2 * 0.35f : refl;
                rect_grad(xx, base + 2, barw, rh2, alpha(lo, 0.22f), alpha(lo, 0.0f));
            }
            float py = base - bandpk[b] * bh;
            if (bandpk[b] > 0.02f) rect(xx, py - 3, barw, 2, alpha(top, 0.95f));
        }
        if (!g_mobile) set_additive(1);
        for (int b = 0; !g_mobile && b < NBANDS; b += 2) {
            float u = (float)b / (NBANDS - 1);
            col lo = u < 0.5f ? mix(VC[0], VC[1], u * 2) : mix(VC[1], VC[2], (u - 0.5f) * 2);
            blob(bx + (b + 0.5f) * step, base - band[b] * bh, 14 + band[b] * 10, alpha(lo, 0.10f * band[b]));
        }
        set_additive(0);
        static const struct { double f; const char *l; } AX[] = { { 100, "100" }, { 1000, "1k" }, { 10000, "10k" } };
        for (int i = 0; i < 3; i++) {
            float xx = bx + bw * (float)(log(AX[i].f / 40.0) / log(17000.0 / 40.0));
            text(xx - text_width(AX[i].l, 11) / 2, sy + sh - 24, 11, alpha(DIM, 0.8f), AX[i].l);
        }
    }
}

static void draw_level_card(float x, float sy, float w, float sh) {
        card(x, sy, w, sh, 0);
        text(x + 18, sy + 12, 12, DIM, "LEVEL");
        float my0 = sy + 34, mh = sh - 34 - 38, mw = 26;
        if (mh < 48) {   /* a short card (portrait layout): two horizontal meters, the vertical ones would have no height */
            enum { HS = 14 };
            float bx = x + 34, bw = w - 34 - 14, sgw = bw / HS, rowh = (sh - 34 - 12) / 2, bh = rowh - 6;
            if (bh > 18) bh = 18;
            for (int c = 0; c < 2; c++) {
                float by = sy + 34 + c * rowh;
                for (int i = 0; i < HS; i++) {
                    float lvl = (float)(i + 1) / HS;
                    col base = lvl > 0.9f ? WARN : (lvl > 0.72f ? VC[2] : OKC);
                    int on = vu[c] >= lvl - 0.5f / HS;
                    rrect(bx + i * sgw + 0.5f, by, sgw - 1.5f, bh, 2, alpha(base, on ? 0.95f : 0.12f));
                }
                int pi = (int)(vupk[c] * HS);
                if (pi > 0 && pi <= HS) {
                    float lvl = (float)pi / HS;
                    col base = lvl > 0.9f ? WARN : (lvl > 0.72f ? VC[2] : OKC);
                    rrect(bx + (pi - 1) * sgw + 0.5f, by, sgw - 1.5f, bh, 2, mix(base, C(1, 1, 1, 1), 0.4f));
                }
                text(x + 16, by + bh / 2 - 7, 13, DIM, c ? "R" : "L");
            }
            return;
        }
        enum { SEG = 24 };
        for (int c = 0; c < 2; c++) {
            float mxx = x + 22 + c * (mw + 28);
            float sg = mh / SEG;
            for (int i = 0; i < SEG; i++) {
                float lvl = (float)(i + 1) / SEG;
                float yy = my0 + mh - (i + 1) * sg;
                col base = lvl > 0.9f ? WARN : (lvl > 0.72f ? VC[2] : OKC);
                int on = vu[c] >= lvl - 0.5f / SEG;
                rrect(mxx, yy + 1, mw, sg - 2, 2, alpha(base, on ? 0.95f : 0.12f));
            }
            int pi = (int)(vupk[c] * SEG);
            if (pi > 0 && pi <= SEG) {
                float lvl = (float)pi / SEG;
                float yy = my0 + mh - pi * sg;
                col base = lvl > 0.9f ? WARN : (lvl > 0.72f ? VC[2] : OKC);
                rrect(mxx, yy + 1, mw, sg - 2, 2, mix(base, C(1, 1, 1, 1), 0.4f));
            }
            text(mxx + mw / 2 - 4, sy + sh - 28, 13, DIM, c ? "R" : "L");
        }
}

/* ================================================================== portrait layout (phones)
 * Virtual width 440: header, voices (or orb) + digi, filter strip, spectrum + level, transport with a LIST button. */
#if defined(__APPLE__) && TARGET_OS_IPHONE
/* the AirPlay button: a native view over the window (AVRoutePickerView, ios_glue.m); here only its round background is drawn and the place is remembered for the frame */
static float g_ap_x, g_ap_y, g_ap_r; static int g_ap_seen;
static void draw_airplay_corner(float cx, float cy, float r) {
    rrect(cx - r, cy - r, 2 * r, 2 * r, r, C(0.13f, 0.18f, 0.28f, 0.92f));
    g_ap_x = cx; g_ap_y = cy; g_ap_r = r; g_ap_seen = 1;
}
#endif
static void draw_ui_portrait(const vis_t *vs, double t, double dt) {

    const float M = 10, BOT = g_duo ? 0.0f : 64.0f, gap = 8;        /* the Duo has the wide bar (draw_duo_bar) */
    float W = view_w, H = view_h, aw = W - 2 * M;
    static const tune_t *last_tune;
    static int auto_done;
    if (g_tune != last_tune) { last_tune = g_tune; if (g_tune) g_plopen = 0; }
    if (!auto_done) { auto_done = 1; if (browse && !g_tune) g_plopen = 1; }
    if (g_duo) g_plopen = 0;                                       /* the list is always on the left page */
    const tune_t *tn = g_tune;
    const int nsid = tn && tn->nsid > 1 ? (tn->nsid > 3 ? 3 : tn->nsid) : 1;

    /* ---------------- header */
    float hy = M, hh = g_duo ? 84.0f : 100.0f;
    const float rpad = g_duo && !NG_TV ? 84.0f : 0.0f;                       /* the Duo: time and Wi-Fi of the system lie over the top right corner */
    card(M, hy, aw, hh, 0);
    {
        char title[160], sub[200];
        if (tn) {
            snprintf(title, sizeof title, "%s", tn->info.name[0] ? tn->info.name : (cur >= 0 ? pl[cur].label : ""));
            snprintf(sub, sizeof sub, "%s%s%s", tn->info.author, tn->info.released[0] ? "  \xb7  " : "", tn->info.released);
        } else {
            snprintf(title, sizeof title, "ngSid");
            snprintf(sub, sizeof sub, "SID player");
        }
        float tmax = aw - 32 - 96 - rpad, tsize = 22;
        while (tsize > 15 && text_width(title, tsize) > tmax) tsize -= 1;
        fit_text(title, tmax, tsize);
        text(M + 16, hy + 10, tsize, TXT, title);
        fit_text(sub, tmax, 13);
        if (g_error[0]) text(M + 16, hy + 14 + tsize, 12, WARN, g_error);
        else text(M + 16, hy + 14 + tsize, 13, DIM, sub);
        char tb[32];
        mmss(tb, sizeof tb, (double)vs->frames / g_rate);
        text_right(M + aw - 16 - rpad, hy + 10, 28, TXT, tb);
        if (tn) {
            char b[48];
            snprintf(b, sizeof b, "%d / %d", tn->song, tn->info.songs);
            text_right(M + aw - 16 - rpad, hy + 44, 13, VC[0], b);
            if (g_paused) text_right(M + aw - 16 - rpad, hy + 62, 12, VC[2], "PAUSED");
#if defined(__APPLE__) && TARGET_OS_IPHONE
            if (!g_duo) draw_airplay_corner(M + aw - 30, hy + hh - 24, 16);     /* (the Duo has it in the wide bar) */
#endif
            float cx = M + 16, cy = hy + hh - 30;
            cx += chip(cx, cy, tn->info.rsid ? "RSID" : "PSID", VC[0], 1);
            cx += chip(cx, cy, tn->pal ? "PAL" : "NTSC", VC[2], 1);
            if (tn->nsid == 1 && tn->model[0]) { snprintf(b, sizeof b, "MOS %d", tn->model[0]); cx += chip(cx, cy, b, DIM, 1); }
            else if (tn->nsid > 1) { snprintf(b, sizeof b, "%d SIDs", tn->nsid); cx += chip(cx, cy, b, DIM, 1); }
        }
    }

    /* ---------------- vertical budget */
    float by = H - M - BOT, sh = g_duo ? 88.0f : (H < 720 ? 80 : 96), sy = by - M - sh, fh = 46, fy = sy - M - fh;   /* low windows: flatter spectrum card */
    float cy0 = hy + hh + M, midh = fy - M - cy0;
    const float tv_side = NG_TV && nsid == 1 ? (aw - 2 * gap) / 3 : 0;   /* Apple TV, one SID: three square voice cards in one row */
    if (NG_TV) {
        const float tvdh = 52;                                               /* the digi strip is flatter here, the spectrum gets the room */
        if (nsid == 1) { sh = by - (cy0 + tv_side + M + tvdh + M + fh + M); sy = by - sh; fy = sy - M - fh; midh = fy - M - cy0; }
        else { sh = 120; sy = by - sh; fy = sy - M - fh; midh = fy - M - cy0; }
    }

    if (g_plopen) {
        draw_sidebar(M, cy0, aw, sy + sh - cy0);      /* the transport bar below (LIST!) still gets its click */
    } else {
        /* ---------------- voices + digi, or the orb */
        if (g_orb == 1) draw_orb(M, cy0, aw, midh, (float)dt);
        else if (g_orb == 2) draw_notes(M, cy0, aw, midh, vs, (float)dt);
        else if (g_orb == 3) draw_info(M, cy0, aw, midh, (float)dt);
        else {
            float dh = NG_TV ? 52.0f : 64.0f;
            int rows = nsid == 1 && !NG_TV ? 3 : nsid;
            float rhv = (midh - dh - M - gap * (float)(rows - 1)) / (float)rows;
            if (rhv < 64) { dh = 46; rhv = (midh - dh - M - gap * (float)(rows - 1)) / (float)rows; }   /* tight: smaller digi card */
            float rmax = NG_TV ? (nsid == 1 ? tv_side : 170.0f) : (nsid == 1 ? 120.0f : 90.0f);
            if (rhv > rmax) rhv = rmax;
            if (rhv < 60) rhv = 60;
            if (NG_TV && nsid == 1) rhv = tv_side;
            float vw3 = (aw - 2 * gap) / 3;
            for (int c = 0; c < rows; c++) {
                if (NG_TV && nsid == 1) {                                     /* three squares in one row: the large voice card */
                    for (int v = 0; v < 3; v++) {
                        float vx = M + v * (vw3 + gap);
                        draw_voice(v, vx, cy0, vw3, rhv, vs, t);
                        if (clicked(vx, cy0, vw3, rhv)) { g_mute ^= 1u << v; apply_mute(); click = 0; }
                    }
                } else if (nsid == 1) {
                    draw_voice_compact(0, c, M, cy0 + (float)c * (rhv + gap), aw, rhv, vs);
                    if (clicked(M, cy0 + (float)c * (rhv + gap), aw, rhv)) { g_mute ^= 1u << c; apply_mute(); click = 0; }
                } else {
                    for (int v = 0; v < 3; v++) {
                        float vx = M + v * (vw3 + gap), vyy = cy0 + (float)c * (rhv + gap);
                        draw_voice_compact(c, v, vx, vyy, vw3, rhv, vs);
                        if (clicked(vx, vyy, vw3, rhv)) { g_mute ^= 1u << v; apply_mute(); click = 0; }
                    }
                }
            }
            float dy = cy0 + (float)rows * rhv + gap * (float)(rows - 1) + M;
            draw_digi(M, dy, aw, dh, vs);
            if (clicked(M, dy, aw, dh)) { int ds = digi_source(vs); g_mute ^= ds >= 0 ? 1u << ds : 8u; apply_mute(); click = 0; }
        }

        /* ---------------- filter strip + VOICES | ORB switch */
        card(M, fy, aw, fh, 0);
        {
            int mode = vs->reg[0x18];
            float cx = M + 16, cy = fy + 13;
            text(cx, cy + 1, 12, DIM, "FILTER");
            cx += 56;
            cx += chip(cx, cy, "LP", VC[0], (mode & 0x10) != 0);
            cx += chip(cx, cy, "BP", VC[0], (mode & 0x20) != 0);
            cx += chip(cx, cy, "HP", VC[0], (mode & 0x40) != 0);
            float sw = 52, sx0 = M + aw - 14 - 4 * sw;
            static const char *const SL[4] = { "VOICES", "NOTES", "ORB", "INFO" }; static const int SV[4] = { 0, 2, 1, 3 };
            for (int i = 0; i < 4; i++) {
                float xx = sx0 + i * sw;
                int on = g_orb == SV[i], hv = hit(xx, cy - 2, sw - 4, 20);
                rrect(xx, cy - 2, sw - 4, 20, 6, on ? alpha(VC[2], 0.25f) : alpha(FAINT, hv ? 0.30f : 0.16f));
                text(xx + (sw - 4 - text_width(SL[i], 11)) / 2, cy + 2, 11, on ? VC[2] : alpha(DIM, 0.9f), SL[i]);
                if (clicked(xx, cy - 2, sw - 4, 20)) { g_orb = SV[i]; click = 0; }
            }
        }
        float vuw = 130, spw = aw - vuw - M;
        draw_spectrum_card(M, sy, spw, sh);
        draw_level_card(M + spw + M, sy, vuw, sh);
    }

    /* ---------------- transport */
    if (!g_duo) {
        float bx = M, bw = aw, cy = by + BOT / 2, r = 20;
        card(bx, by, bw, BOT, 0);
        float bxs[3] = { bx + 12, bx + 62, bx + 112 };
        for (int i = 0; i < 3; i++) {
            float x = bxs[i];
            int hv = hit(x, cy - r, 2 * r, 2 * r);
            rrect(x, cy - r, 2 * r, 2 * r, r, alpha(hv ? VC[0] : EDGE, hv ? 0.30f : 0.35f));
            col ic = hv ? TXT : DIM;
            float ccx = x + r, ccy = cy;
            if (i == 0) { tri(ccx + 6, ccy - 8, ccx + 6, ccy + 8, ccx - 6, ccy, ic); rect(ccx - 9, ccy - 8, 3, 16, ic); }
            else if (i == 2) { tri(ccx - 6, ccy - 8, ccx - 6, ccy + 8, ccx + 6, ccy, ic); rect(ccx + 6, ccy - 8, 3, 16, ic); }
            else if (g_paused) tri(ccx - 5, ccy - 9, ccx - 5, ccy + 9, ccx + 9, ccy, ic);
            else { rect(ccx - 7, ccy - 8, 5, 16, ic); rect(ccx + 2, ccy - 8, 5, 16, ic); }
            if (clicked(x, cy - r, 2 * r, 2 * r)) {
                if (i == 0) step_tune(-1);
                else if (i == 2) step_tune(1);
                else g_paused = !g_paused;
                click = 0;
            }
        }
        {   /* playlist / HVSC overlay on/off */
            float lx = bx + 166, lw = 58;
            int hv = hit(lx, cy - 16, lw, 32);
            rrect(lx, cy - 16, lw, 32, 9, alpha(g_plopen ? VC[2] : (hv ? VC[0] : EDGE), g_plopen ? 0.28f : 0.35f));
            text(lx + (lw - text_width("LIST", 12)) / 2, cy - 8, 12, g_plopen ? VC[2] : TXT, "LIST");
            if (clicked(lx, cy - 16, lw, 32)) { g_plopen = !g_plopen; if (g_plopen) list_follow = 1; click = 0; }
        }
        float vx = bx + 250, vw = bw - 250 - 22, vyb = cy - 3;
        rrect(vx, vyb, vw, 6, 3, alpha(FAINT, 0.4f));
        rrect(vx, vyb, 6 + (vw - 6) * g_vol, 6, 3, VC[0]);
        rrect(vx + (vw - 6) * g_vol - 3, vyb - 6, 18, 18, 9, TXT);
        if (clicked(vx - 12, cy - 18, vw + 24, 36)) vol_drag = 1;
        if (vol_drag) {
            if (!mouse_down) vol_drag = 0;
            else { g_vol = (mx - vx) / vw; if (g_vol < 0) g_vol = 0; if (g_vol > 1) g_vol = 1; }
        }
    }
}

static Uint64 g_draw_t0; static float g_draw_ms;                          /* CPU time of the frame (building the picture), shown by draw_fps */
#if defined(__APPLE__) && TARGET_OS_TV
/* Apple TV: locked 60 frames a second by an adaptive render size. The picture is rendered at the native size of the screen (4K when the Apple TV is set to 4K), at 0.75 of it
 * or at 1920 wide, whichever the GPU keeps at 60: the GPU time of the frames is averaged over a second; more than 12 ms steps down at once, less than 8.5 ms (predicted for the next
 * step up from the pixel count) four seconds in a row steps up. Only the GPU decides (when the CPU is the slow one, a smaller picture does not help). Starts at the lowest step. */
static float tv_fps, tv_slowest, tv_cpu, tv_gpu; static int tv_w, tv_h, tv_hz;       /* the last second, for the counter */
static void tv_adapt(SDL_Window *wn, int fw, int fh) {
    static Uint64 last; static double acc, worst, cpu, gpu; static int n, good, lvl = -1, nlv, fixed, refresh; static float lv[3];
    if (lvl < 0) {
        int nw = 0, nh = 0; gfx_native_size(wn, &nw, &nh);
        float lo = nw > 1920 ? 1920.0f / (float)nw : 1.0f;
        lv[0] = lo; nlv = 1; if (lo < 1.0f) { lv[1] = (lo + 1.0f) / 2; lv[2] = 1.0f; nlv = 3; }
        lvl = 0; gfx_set_render_scale(lv[0]); refresh = gfx_refresh_hz(wn);
        const char *fx = getenv("NGSID_TEST_SCALE");                     /* test aid: a fixed render scale (1 = native), no adaptation */
        if (fx) { fixed = 1; gfx_set_render_scale((float)atof(fx)); }
    }
    (void)fh;
    Uint64 t = SDL_GetPerformanceCounter();
    if (last) {
        double dt = (double)(t - last) / (double)SDL_GetPerformanceFrequency();
        acc += dt; n++; cpu += g_draw_ms; gpu += gfx_last_gpu_ms(); if (dt > worst) worst = dt;
        if (acc >= 1.0) {
            double g = gpu / n; tv_fps = (float)(n / acc); tv_slowest = (float)(worst * 1000.0); tv_cpu = (float)(cpu / n); tv_gpu = (float)g; tv_w = fw; tv_h = fh;
            refresh = gfx_refresh_hz(wn); tv_hz = refresh;                              /* the screen can be switched while the app runs (50 / 60 Hz) */
            const double budget = 1000.0 / (double)(refresh > 0 ? refresh : 60);        /* ms a frame may take: one refresh of the screen */
            if (fixed) { /* no adaptation */ }
            else if (g > 0.80 * budget && lvl > 0) { lvl--; gfx_set_render_scale(lv[lvl]); good = 0; }
            else if (lvl + 1 < nlv && tv_fps > (float)refresh - 1.5f && g * (double)(lv[lvl + 1] / lv[lvl]) * (double)(lv[lvl + 1] / lv[lvl]) < 0.70 * budget) { if (++good >= 4) { lvl++; gfx_set_render_scale(lv[lvl]); good = 0; } }
            else good = 0;
            acc = 0; n = 0; worst = 0; cpu = 0; gpu = 0;
        }
    }
    last = t;
}
/* the counter: TestFlight builds only (or NGSID_TEST_FPS=1): frame rate, slowest frame, CPU and GPU time of a frame, render size */
static void draw_fps(float W, float H) {
    static int on = -1;
    if (on < 0) {
        on = 0;
#ifdef NG_FPS_COUNTER                                                      /* compiled in only for TestFlight test builds (FPS=1 for tvos/scripts/archive.sh): a build for the App Store has no counter at all */
        on = getenv("NGSID_TEST_FPS") != NULL || ios_is_testflight();
#endif
    }
    if (!on || tv_w <= 0) return;
    char txt[96];
    snprintf(txt, sizeof txt, "%.0f/%d fps  slowest %.0f ms  cpu %.1f  gpu %.1f  %dx%d", tv_fps, tv_hz, tv_slowest, tv_cpu, tv_gpu, tv_w, tv_h);
    text_right(W - 12, H - 13, 10, alpha(DIM, 0.85f), txt);          /* in the free margin below the cards */
}
#endif
static void draw_duo_bar(float W, float H) {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_TV
    const float M = 10, BH = 64;
    float by = H - M - BH, cy = by + BH / 2, r = 20, fold = W / 2;
    card(M, by, W - 2 * M, BH, 0);
    /* left half: previous, play / pause, next, the volume up to the fold */
    float bxs[3] = { M + 12, M + 62, M + 112 };
    for (int i = 0; i < 3; i++) {
        float x = bxs[i];
        int hv = hit(x, cy - r, 2 * r, 2 * r);
        rrect(x, cy - r, 2 * r, 2 * r, r, alpha(hv ? VC[0] : EDGE, hv ? 0.30f : 0.35f));
        col ic = hv ? TXT : DIM;
        float ccx = x + r, ccy = cy;
        if (i == 0) { tri(ccx + 6, ccy - 8, ccx + 6, ccy + 8, ccx - 6, ccy, ic); rect(ccx - 9, ccy - 8, 3, 16, ic); }
        else if (i == 2) { tri(ccx - 6, ccy - 8, ccx - 6, ccy + 8, ccx + 6, ccy, ic); rect(ccx + 6, ccy - 8, 3, 16, ic); }
        else if (g_paused) tri(ccx - 5, ccy - 9, ccx - 5, ccy + 9, ccx + 9, ccy, ic);
        else { rect(ccx - 7, ccy - 8, 5, 16, ic); rect(ccx + 2, ccy - 8, 5, 16, ic); }
        if (clicked(x, cy - r, 2 * r, 2 * r)) {
            if (i == 0) step_tune(-1);
            else if (i == 2) step_tune(1);
            else g_paused = !g_paused;
            click = 0;
        }
    }
    float vx = M + 172, vw = (fold - 8) - vx;
    rrect(vx, cy - 3, vw, 6, 3, alpha(FAINT, 0.4f));
    rrect(vx, cy - 3, 6 + (vw - 6) * g_vol, 6, 3, VC[0]);
    rrect(vx + (vw - 6) * g_vol - 3, cy - 9, 18, 18, 9, TXT);
    if (clicked(vx - 12, cy - 18, vw + 24, 36)) vol_drag = 1;
    if (vol_drag) {
        if (!mouse_down) vol_drag = 0;
        else { g_vol = (mx - vx) / vw; if (g_vol < 0) g_vol = 0; if (g_vol > 1) g_vol = 1; }
    }
    /* right half: the tune of a multi-tune file (previous / number / next), AirPlay at the right edge */
    const tune_t *tn = g_tune;
    if (tn && tn->info.songs > 1) {
        float sx = fold + 8, sw = 34;
        for (int i = 0; i < 2; i++) {
            float x = sx + i * (sw + 86);
            int hv = hit(x, cy - 16, sw, 32);
            rrect(x, cy - 16, sw, 32, 9, alpha(hv ? VC[0] : EDGE, hv ? 0.30f : 0.35f));
            if (i == 0) tri(x + 21, cy - 7, x + 21, cy + 7, x + 12, cy, hv ? TXT : DIM);
            else tri(x + 13, cy - 7, x + 13, cy + 7, x + 22, cy, hv ? TXT : DIM);
            if (clicked(x, cy - 16, sw, 32)) { change_song(i ? 1 : -1); click = 0; }
        }
        char sb[40]; snprintf(sb, sizeof sb, "SONG %d / %d", tn->song, tn->info.songs);
        text(sx + sw + (86 - text_width(sb, 12)) / 2, cy - 8, 12, VC[0], sb);
    }
#if TARGET_OS_TV
    text_right(W - M - 20, cy - 16, 12, DIM, "UP / DOWN  choose");
    text_right(W - M - 20, cy + 2, 12, DIM, "SELECT  play / open      MENU  back      PLAY / PAUSE");
#else
    draw_airplay_corner(W - M - 16 - 18, cy, 18);
#endif
#else
    (void)W; (void)H;
#endif
}
static void draw_ui_duo(const vis_t *vs, double t, double dt) {
#if defined(__APPLE__) && TARGET_OS_TV
    {   /* Apple TV: the narrow list on the left (the focus never leaves it), the player on the rest of the width and the whole height, no bar below: the remote does everything.
         * The background fills the whole screen (the viewport is the screen); the cards keep the small free margin (g_tvi_*), everything is drawn moved by it. */
        const float M = 10;
        const float W = view_w, H = view_h, aw = W - g_tvi_l - g_tvi_r, ah = H - g_tvi_t - g_tvi_b, lw = floorf(aw * 0.30f);
        g_ox = g_tvi_l; g_oy = g_tvi_t;
        draw_sidebar(M, M, lw - M, ah - M);
        float mx0 = mx, vw0 = view_w, vh0 = view_h;
        int cl0 = clickx, rcl0 = rclickx;
        g_ox = g_tvi_l + lw; mx -= lw; clickx -= (int)lw; rclickx -= (int)lw;
        view_w = aw - lw; view_h = ah + M;                          /* (the phone layout keeps one margin above the end of its height) */
        draw_ui_portrait(vs, t, dt);
        g_ox = 0; g_oy = 0; mx = mx0; clickx = cl0; rclickx = rcl0; view_w = vw0; view_h = vh0;
        draw_fps(W, H);
        return;
    }
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    const float M = 10, BAR = 64;
    float W = view_w, H = view_h, pw = W / 2, Hc = H - BAR - M;     /* the two pages touch (the fold lies between them); the wide bar below them */
    g_ox = 0;
    draw_sidebar(M, M, pw - M, Hc - 2 * M);                          /* left page: the list (PLAYLIST / HVSC ONLINE / FAV), always visible */
    float mx0 = mx, vw0 = view_w, vh0 = view_h;
    int cl0 = clickx, rcl0 = rclickx;
    g_ox = pw; mx -= pw; clickx -= (int)pw; rclickx -= (int)pw;      /* the right page works in its own coordinates */
    view_w = pw; view_h = Hc + M;                                    /* (the phone layout keeps one margin above the end of its height) */
    draw_ui_portrait(vs, t, dt);                                     /* right page: the player (header, voices, digi, filter, spectrum, level) */
    g_ox = 0; mx = mx0; clickx = cl0; rclickx = rcl0; view_w = vw0; view_h = vh0;
    draw_duo_bar(W, H);
#else
    (void)vs; (void)t; (void)dt;
#endif
}
static void draw_ui(const vis_t *vs, double t, double dt) {
    float W = view_w, H = view_h;
    int vol_changed = 0;
    (void)dt;

    if (g_orb != 2 && la_e) la_destroy();                       /* the notes view is off: the look-ahead engine is not needed */

    /* background */
    rect_grad(0, 0, W, H, BG1, BG0);
    float act = 0;
    const int nsid = g_tune && g_tune->nsid > 1 ? (g_tune->nsid > 3 ? 3 : g_tune->nsid) : 1;
    for (int v = 0; v < 3; v++) {
        float target = 0;
        for (int c = 0; c < nsid; c++) {
            float tg = (vs->running && (vs->vi[c][v].ctrl & 1)) ? 1.0f : (vs->running ? vs->vi[c][v].level / 127.0f : 0);
            if (tg > target) target = tg;
        }
        if (g_mute >> v & 1) target = 0;
        glow[v] += (target - glow[v]) * 0.12f;
        act += glow[v];
    }
    if (!g_mobile) {                      /* big glow blobs cost a lot of fill rate: skipped on phones */
        set_additive(1);
        blob(W * 0.25f, -30, 380, alpha(VC[0], 0.06f + 0.05f * glow[0]));
        blob(W * 0.60f, -60, 420, alpha(VC[1], 0.05f + 0.05f * glow[1]));
        blob(W * 0.92f, H * 0.9f, 380, alpha(VC[2], 0.04f + 0.05f * glow[2]));
        set_additive(0);
    }

    if (g_duo) { draw_ui_duo(vs, t, dt); return; }
    if (g_portrait) { draw_ui_portrait(vs, t, dt); return; }
    const float M = 16, SB = 288;
    /* ---------------- playlist sidebar */
    draw_sidebar(M, M, SB, H - 2 * M);

    float ax = M + SB + M, aw = W - ax - M;

    /* ---------------- header */
    const tune_t *tn = g_tune;
    float hy = M, hh = 112;
    card(ax, hy, aw, hh, 0);
    {
        char title[160], sub[200];
        if (tn) {
            snprintf(title, sizeof title, "%s", tn->info.name[0] ? tn->info.name : (cur >= 0 ? pl[cur].label : ""));
            snprintf(sub, sizeof sub, "%s%s%s", tn->info.author, tn->info.released[0] ? "  \xb7  " : "", tn->info.released);
        } else {
            snprintf(title, sizeof title, "ngSid");
            snprintf(sub, sizeof sub, "SID player  \xb7  based on VacSid by Oliver Blasnik");
        }
        /* tags under the title; the SID graphic sits to the right of them */
        char tag[8][40]; col tagc[8]; int taglit[8], ntag = 0;
        #define TAG(str, c, lit) do { snprintf(tag[ntag], sizeof tag[0], "%s", str); tagc[ntag] = c; taglit[ntag] = lit; ntag++; } while (0)
        const int show_chips = tn && tn->nsid > 0 && tn->model[0] && aw >= 640;
        if (tn) {
            char b[40];
            TAG(tn->info.rsid ? "RSID" : "PSID", VC[0], 1);
            TAG(tn->pal ? "PAL" : "NTSC", VC[2], 1);
            if (!show_chips && tn->nsid == 1 && tn->model[0]) { snprintf(b, sizeof b, "MOS %d", tn->model[0]); TAG(b, DIM, 1); }
            else if (!show_chips && tn->nsid > 1) {
                int o = snprintf(b, sizeof b, "%d SIDs:", tn->nsid);
                for (int i = 0; i < tn->nsid && o < (int)sizeof b - 8; i++)
                    o += snprintf(b + o, sizeof b - (size_t)o, "%s%d", i ? " + " : " ", tn->model[i]);
                TAG(b, DIM, 1);
            }
            snprintf(b, sizeof b, "LOAD $%04X", tn->info.load); TAG(b, DIM, 0);
            snprintf(b, sizeof b, "INIT $%04X", tn->info.init); TAG(b, DIM, 0);
            snprintf(b, sizeof b, "PLAY $%04X", tn->info.play); TAG(b, DIM, 0);
            if (tn->info.sid2) { snprintf(b, sizeof b, "2. SID $%04X", tn->info.sid2); TAG(b, DIM, 0); }
        }
        #undef TAG
        float tagsw = 0;
        for (int i = 0; i < ntag; i++) tagsw += chip_w(tag[i]);

        /* SID graphic: one chip large, several smaller; tall only if it does not run into the tags */
        float cwid = 0, cgap = 12, chipL = ax + aw - 22 - 150, chipR = chipL;
        int nch = show_chips ? tn->nsid : 0;
        if (nch) {
            cwid = nch == 1 ? 200 : nch == 2 ? 128 : 94;
            float tot = cwid * (float)nch + cgap * (float)(nch - 1);
            if (chipR - tot < ax + 22 + tagsw + 16 && cwid > 150) cwid = 128;
            tot = cwid * (float)nch + cgap * (float)(nch - 1);
            float cap = (aw - 220) * 0.45f;
            if (tot > cap) { cwid = (cap - cgap * (float)(nch - 1)) / (float)nch; tot = cap; }
            chipL = chipR - tot;
        }

        float tmax = (nch ? chipL - 16 : ax + aw - 220 + 0) - (ax + 22);
        if (!nch) tmax = aw - 220;
        /* the C64 that goes with the chip model (6581: transparent purple, 8580: the one with the lit keys): turned 22.5 degrees,
           right behind the title; a click opens commodore.net */
        const float c64ang = 0.392699f;                    /* 22.5 degrees */
        int c64i = tn && tn->model[0] ? (tn->model[0] == 8580 ? 1 : 0) : -1;
        float c64h = 48, c64w = 0, c64bb = 0;
        if (c64i >= 0) {
            c64w = c64h * (c64i ? (float)C64_8580_W / (float)C64_8580_H : (float)C64_6581_W / (float)C64_6581_H);
            c64bb = c64w * cosf(c64ang) + c64h * sinf(c64ang);      /* width of the turned picture */
            if (tmax - c64bb < 220) c64i = -1;                   /* no room: keep the title wide */
            else tmax -= c64bb + 8;
        }
        float tsize = 32;
        while (tsize > 18 && text_width(title, tsize) > tmax) tsize -= 2;
        if (c64i >= 0) {          /* drawn first: the title, subtitle and tags stay readable on top of it */
            float tw = text_width(title, tsize), sw = text_width(sub, 16);      /* right behind the longer of title and subtitle line */
            if (sw > tw) tw = sw;
            if (tw > tmax) tw = tmax;
            float ccx = ax + 22 + tw + 12 + c64bb / 2, ccy = hy + hh / 2 - 1;
            int hv = c64_hit(mx, my, ccx, ccy, c64w, c64h, c64ang);
            float grow = hv ? 1.08f : 1.0f;
            if (!c64d_header(c64i, ccx, ccy, c64w * grow * 1.5f, c64h * grow * 1.5f, (float)dt)) draw_c64(c64i, ccx, ccy, c64w * grow, c64h * grow, c64ang, 1.0f);      /* the 3D one turns all the way round */
            if (click && c64_hit((float)clickx, (float)clicky, ccx, ccy, c64w, c64h, c64ang)) { open_url("https://commodore.net/computer/"); click = 0; }
        }
        text(ax + 22, hy + 14, tsize, TXT, title);
        if (g_error[0]) {       /* in place of the subtitle (author, year), so it never overlaps the line */
            char em[160]; snprintf(em, sizeof em, "%s", g_error);
            fit_text(em, tmax, 14);
            text(ax + 22, hy + 14 + tsize + 6, 14, WARN, em);
        } else {
            fit_text(sub, tmax, 16);
            text(ax + 22, hy + 14 + tsize + 6, 16, DIM, sub);
        }

        float cx = ax + 22, cy = hy + hh - 30;
        for (int i = 0; i < ntag; i++) cx += chip(cx, cy, tag[i], tagc[i], taglit[i]);
        if (nch) {
            /* every chip animates on its own: level from its own voices, pulse on its own note onsets */
            static float clvl[3], cbeat[3], pgate[3][3], plvl[3][3];
            static Uint32 ct;
            Uint32 now_ms = SDL_GetTicks();
            float cdt = ct ? (float)(now_ms - ct) / 1000.0f : 0.016f;
            ct = now_ms;
            if (cdt > 0.1f) cdt = 0.1f;
            for (int i = 0; i < nch && i < 3; i++) {
                float tgt = 0, kick = 0;
                for (int v = 0; v < 3; v++) {
                    const ngsid_voice_info *vv = &vs->vi[i][v];
                    float l = vs->running && !g_paused ? vv->level / 127.0f : 0;
                    float gt = vs->running && !g_paused && (vv->ctrl & 1) ? 1.0f : 0.0f;
                    if (l > tgt) tgt = l;
                    if (gt > pgate[i][v] || l > plvl[i][v] + 0.25f) kick = 1;   /* gate on or sudden level jump */
                    pgate[i][v] = gt; plvl[i][v] = l;
                }
                if (nch == 1) {                          /* single chip: the audio beat as before */
                    float a = vs->peak[0] > vs->peak[1] ? vs->peak[0] : vs->peak[1];
                    tgt = g_paused ? 0 : (a > 1 ? 1 : a);
                    kick = g_paused ? 0 : g_beat;
                }
                clvl[i] += (tgt - clvl[i]) * 0.2f;
                if (kick > cbeat[i]) cbeat[i] = kick;
                cbeat[i] *= expf(-cdt * 7.0f);
            }
            float ch = cwid * 0.40f, y0 = hy + (cwid > 150 ? 12 : 16);   /* small chips end above the tag row */
            if (cwid > 150 && ch > hh - 26) ch = hh - 26;
            for (int i = 0; i < nch; i++) {
                float cxx = chipL + (cwid + cgap) * (float)i;
                draw_sid(cxx, y0, cwid, tn->model[i], clvl[i < 3 ? i : 2], g_paused ? 0.0f : cbeat[i < 3 ? i : 2]);
                if (nch > 1)
                    draw_pan(cxx + 4, y0 + cwid * 0.40f + 9, cwid - 8, sid_pan(nch, i),
                             tn->model[i] == 8580 ? C(0.35f, 0.82f, 1.0f, 1) : C(1.0f, 0.62f, 0.28f, 1));
            }
        }
        /* time + song */
        char tb[32];
        double sec = (double)vs->frames / g_rate;
        mmss(tb, sizeof tb, sec);
        text_right(ax + aw - 22, hy + 12, 44, TXT, tb);
        if (tn) {
            char sb[48];
            snprintf(sb, sizeof sb, "SONG %d / %d", tn->song, tn->info.songs);
            text_right(ax + aw - 22, hy + 66, 16, VC[0], sb);
            if (g_paused) text_right(ax + aw - 22, hy + 88, 13, VC[2], "PAUSED");
        }
#if defined(__APPLE__) && TARGET_OS_IPHONE
        draw_airplay_corner(ax + aw - 160, hy + hh - 30, 16);      /* landscape: left of the SONG / PAUSED lines, bottom right of the header card */
#endif
    }

    /* ---------------- voice cards */
    float vy = hy + hh + M, vh = 214, gap = M;
    float vw = (aw - 2 * gap) / 3;
    float rhc = nsid == 2 ? 101 : 76;
    if (nsid > 1) vh = rhc * (float)nsid + gap * (float)(nsid - 1);
    if (g_orb) {
        /* orb covers the voice cards and the digi channel */
    } else if (nsid == 1) {
        for (int v = 0; v < 3; v++) {
            float vx = ax + v * (vw + gap);
            draw_voice(v, vx, vy, vw, vh, vs, t);
            if (clicked(vx, vy, vw, vh)) { g_mute ^= 1u << v; apply_mute(); click = 0; }
        }
    } else {   /* several SIDs: one compact row per chip; a click mutes that voice on every chip */
        float rh = rhc;
        for (int c = 0; c < nsid; c++)
            for (int v = 0; v < 3; v++) {
                float vx = ax + v * (vw + gap), cyy = vy + (float)c * (rh + gap);
                draw_voice_compact(c, v, vx, cyy, vw, rh, vs);
                if (clicked(vx, cyy, vw, rh)) { g_mute ^= 1u << v; apply_mute(); click = 0; }
            }
    }

    /* ---------------- digi channel */
    float dy = vy + vh + M, dh = 96;
    if (g_orb == 1) draw_orb(ax, vy, aw, dy + dh - vy, (float)dt);
    else if (g_orb == 2) draw_notes(ax, vy, aw, dy + dh - vy, vs, (float)dt);
    else if (g_orb == 3) draw_info(ax, vy, aw, dy + dh - vy, (float)dt);
    else draw_digi(ax, dy, aw, dh, vs);
    if (!g_orb && clicked(ax, dy, aw, dh)) { int ds = digi_source(vs); g_mute ^= ds >= 0 ? 1u << ds : 8u; apply_mute(); click = 0; }

    /* ---------------- SID strip (filter, volume) */
    float fy = dy + dh + M, fh = 46;
    card(ax, fy, aw, fh, 0);
    {
        const uint8_t *r = vs->reg;
        int cutoff = (r[0x16] << 3) | (r[0x15] & 7), res = r[0x17] >> 4, route = r[0x17] & 7, mode = r[0x18], volm = r[0x18] & 15;
        float cx = ax + 18, cy = fy + 13;
        text(cx, cy + 1, 12, DIM, "FILTER");
        cx += 56;
        cx += chip(cx, cy, "LP", VC[0], (mode & 0x10) != 0);
        cx += chip(cx, cy, "BP", VC[0], (mode & 0x20) != 0);
        cx += chip(cx, cy, "HP", VC[0], (mode & 0x40) != 0);
        cx += 10;
        text(cx, cy + 1, 12, DIM, "CUTOFF");
        cx += 56;
        rrect(cx, cy + 7, 120, 6, 3, alpha(FAINT, 0.35f));
        rrect(cx, cy + 7, 6 + 114.0f * cutoff / 2047.0f, 6, 3, VC[1]);
        cx += 130;
        char b[32];
        snprintf(b, sizeof b, "RES %d", res);
        text(cx, cy + 1, 12, DIM, b);
        cx += 62;
        text(cx, cy + 1, 12, DIM, "ROUTE");
        cx += 50;
        for (int v = 0; v < 3; v++) { snprintf(b, sizeof b, "V%d", v + 1); cx += chip(cx, cy, b, VC[v], (route >> v & 1) != 0); }
        {   /* VOICES | ORB switch (also for touch) */
            float sw = 54, sx0 = ax + aw - 18 - 130 - 4 * sw;
            static const char *const SL[4] = { "VOICES", "NOTES", "ORB", "INFO" }; static const int SV[4] = { 0, 2, 1, 3 };
            for (int i = 0; i < 4; i++) {
                float xx = sx0 + i * sw;
                int on = g_orb == SV[i], hv = hit(xx, cy - 2, sw - 4, 20);
                rrect(xx, cy - 2, sw - 4, 20, 6, on ? alpha(VC[2], 0.25f) : alpha(FAINT, hv ? 0.30f : 0.16f));
                text(xx + (sw - 4 - text_width(SL[i], 11)) / 2, cy + 2, 11, on ? VC[2] : alpha(DIM, 0.9f), SL[i]);
                if (clicked(xx, cy - 2, sw - 4, 20)) { g_orb = SV[i]; click = 0; }
            }
        }
        float rx = ax + aw - 18;
        snprintf(b, sizeof b, "%d", volm);
        rx -= text_right(rx, cy, 14, TXT, b);
        text_right(rx - 8, cy + 1, 12, DIM, "SID VOL");
    }

    /* ---------------- spectrum + VU */
    float sy = fy + fh + M, bot = 62;
    float sh = H - M - bot - sy - M;
    if (sh < 120) sh = 120;
    float vuw = 132, spw = aw - vuw - M;
    draw_spectrum_card(ax, sy, spw, sh);
    draw_level_card(ax + spw + M, sy, vuw, sh);

    /* ---------------- bottom control bar */
    {
        float by = H - M - bot + 0, bx = ax, bw = aw;
        card(bx, by, bw, bot - 0, 0);
        float cy = by + bot / 2;
        struct { float x; int kind; } btn[3] = { { bx + 22, 0 }, { bx + 74, 1 }, { bx + 126, 2 } };
        for (int i = 0; i < 3; i++) {
            float x = btn[i].x, r = 20;
            int hv = hit(x, cy - r, 2 * r, 2 * r);
            rrect(x, cy - r, 2 * r, 2 * r, r, alpha(hv ? VC[0] : EDGE, hv ? 0.30f : 0.35f));
            col ic = hv ? TXT : DIM;
            float ccx = x + r, ccy = cy;
            if (btn[i].kind == 0) { tri(ccx + 6, ccy - 8, ccx + 6, ccy + 8, ccx - 6, ccy, ic); rect(ccx - 9, ccy - 8, 3, 16, ic); }
            else if (btn[i].kind == 2) { tri(ccx - 6, ccy - 8, ccx - 6, ccy + 8, ccx + 6, ccy, ic); rect(ccx + 6, ccy - 8, 3, 16, ic); }
            else if (g_paused) tri(ccx - 5, ccy - 9, ccx - 5, ccy + 9, ccx + 9, ccy, ic);
            else { rect(ccx - 7, ccy - 8, 5, 16, ic); rect(ccx + 2, ccy - 8, 5, 16, ic); }
            if (clicked(x, cy - r, 2 * r, 2 * r)) {
                if (btn[i].kind == 0) step_tune(-1);
                else if (btn[i].kind == 2) step_tune(1);
                else g_paused = !g_paused;
                click = 0;
            }
        }
        text(bx + 190, cy - 16, 11, FAINT, "Space pause  \xb7  Left/Right song  \xb7  Up/Down file");
        text(bx + 190, cy + 2, 11, FAINT, "1-4 mute voice/digi  \xb7  +/- volume  \xb7  F fullscreen  \xb7  Esc quit");
        float vx = bx + bw - 22 - 170, vyb = cy - 3;
        text(vx - 44, cy - 9, 12, DIM, "VOL");
        rrect(vx, vyb, 170, 6, 3, alpha(FAINT, 0.4f));
        rrect(vx, vyb, 6 + 164 * g_vol, 6, 3, VC[0]);
        rrect(vx + 164 * g_vol - 4, vyb - 5, 14, 16, 7, TXT);
        if (clicked(vx - 8, cy - 18, 186, 36)) vol_drag = 1;
        if (vol_drag) {
            if (!mouse_down) vol_drag = 0;
            else { g_vol = (mx - vx) / 170.0f; vol_changed = 1; }
        }
    }
    if (vol_changed) { if (g_vol < 0) g_vol = 0; if (g_vol > 1) g_vol = 1; }
    (void)act;
}

/* ================================================================== main */

static void save_screenshot(const char *file, int w, int h) {
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    uint8_t *fl = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    gfx_read_pixels(w, h, px);
    for (int y = 0; y < h; y++) memcpy(fl + (size_t)y * (size_t)w * 4, px + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
    SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(fl, w, h, 32, w * 4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (sf) { SDL_SaveBMP(sf, file); SDL_FreeSurface(sf); }
    free(px); free(fl);
}


/* ---------------------------------------------------------------- main loop (a function so it can also run in the browser) */
#ifdef __EMSCRIPTEN__
#define AUDIO_SAMPLES 2048
#elif defined(__APPLE__) && TARGET_OS_IPHONE
#define AUDIO_SAMPLES 2048                         /* iOS: AirPlay and Bluetooth need larger buffers than the desktop's 1024 */
#else
#define AUDIO_SAMPLES 1024
#endif
static SDL_Window *win;
static const char *shot;
static int shot_frames, running, frame, fullscreen;
static Uint64 t0, tlast;
static double freq;

#ifdef __EMSCRIPTEN__
/* Called by the host page after a dropped/selected file was written to the virtual file system. */
/* Play a tune of the HVSC: "97064" (database id of hvsc.c64.org) or "MUSICIANS/K/KB/Katakis_Remix.sid". */
EMSCRIPTEN_KEEPALIVE int ng_web_hvsc(const char *what) {
    char path[768];
    const char *p = what;
    while (*p == '/') p++;
    size_t pn = strlen(p);
    if (pn > 1 && p[pn - 1] == '/' && !isdigit((unsigned char)p[0])) {      /* "MUSICIANS/N/Nata/": the whole folder becomes the playlist */
        hvsc_entry *res = NULL;
        int nres = 0;
        if (pn >= sizeof path || hvsc_list(p, &res, &nres) != 0) { free(res); snprintf(br_msg, sizeof br_msg, "HVSC unreachable"); return -1; }
        int first = npl;
        for (int i = 0; i < nres; i++) {
            if (res[i].dir || !is_sid_name(res[i].name)) continue;
            add_file(res[i].name);
            free(pl[npl - 1].path);
            pl[npl - 1].path = strdup("");
            char rel[900];
            snprintf(rel, sizeof rel, "%s%s", p, res[i].name);
            pl[npl - 1].rel = strdup(rel);
        }
        free(be);
        be = res; nbe = nres;                                   /* the HVSC browser opens in this folder */
        snprintf(rpath, sizeof rpath, "%s", p);
        br_scroll = 0; br_sel = br_at_root() ? 0 : 1; br_follow = 1;
        browse = 1; fav_mode = 0;
        if (npl > first) play_index(first, 0); else snprintf(br_msg, sizeof br_msg, "No SID files in this folder");
        return 0;
    }
    int digits = *p != 0;
    for (const char *q = p; *q; q++) if (*q < '0' || *q > '9') { digits = 0; break; }
    if (digits) {
        if (hvsc_resolve_id(atoi(p), path, sizeof path)) { snprintf(br_msg, sizeof br_msg, "Unknown HVSC id"); return -1; }
    } else {
        snprintf(path, sizeof path, "%s", p);
    }
    job_start(1, path);
    return 0;
}

/* canvas size in pixels (portrait layout on phones, see ngweb/shell.html) */
EMSCRIPTEN_KEEPALIVE void ng_web_size(int w, int h) { if (win && w > 0 && h > 0) SDL_SetWindowSize(win, w, h); }

EMSCRIPTEN_KEEPALIVE void ng_web_add(const char *path) {
    int was = npl;
    add_path(path, 0);
    if (npl > was && cur < 0) play_index(was, 0);
}
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
/* The audio output after a change of the route (CarPlay cable out, Bluetooth in ...) or an interruption (call, Siri): the output of SDL does not come back by itself.
 * It is opened again (the tune goes on where it is); if the route fell back to the built-in speaker the player pauses first. A watchdog does the same when a tune should
 * be playing but the audio callback stands still. */
static void audio_recover(void) {
    ios_audio_longform();
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    SDL_AudioSpec want, have; SDL_zero(want); SDL_zero(have);
    want.freq = g_rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = AUDIO_SAMPLES; want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    ios_audio_longform();
    if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
}
static void audio_watch(void) {
    static Uint32 recover_at, last_t, last_try; static uint64_t last_frames;
    Uint32 tn = SDL_GetTicks();
    int ev = ios_audio_event_poll();
    if (ev && tn - last_try < 1500) ev = 0;                                 /* the notifications that our own reopening causes */
    if (ev) { if (ev & 2) g_paused = 1; recover_at = tn + 400; }
    if (recover_at && !g_paused && (int)(tn - recover_at) >= 0) { recover_at = 0; audio_recover(); last_try = tn; last_t = tn; }   /* paused: do not reopen the output (that takes the audio session from the app that plays now); done when playing again */
    int should = g_tune && g_tune->e && !g_paused;
    uint64_t fr = g_vis.frames;
    if (!should || fr != last_frames) { last_frames = fr; last_t = tn; return; }
    if (tn - last_t > 2000 && tn - last_try > 5000) { last_try = tn; last_t = tn; audio_recover(); }
}
/* Lock screen / Control Center / CarPlay: title, author, released + tune number and the position go to Now Playing when something changed (and every 15 s); the remote
 * commands (0 play, 1 pause, 2 next tune, 3 previous tune) are picked up here. */
/* The cover of the lock screen / Control Center: the C64 of the chip model (6581 or 8580) in the middle of a dark square, drawn once per model (bilinear, the picture is premultiplied). */
#define COVER_PX 512
/* text into the cover (CPU): the glyphs of the font atlas, scaled, alpha-blended; returns nothing, y = top of the line */
static void cover_text(unsigned char *buf, float x, float y, float size, const unsigned char rgb[3], const char *str) {
    float sc = size / FONT_PX;
    for (; *str; str++) {
        int gi = glyph_idx[(unsigned char)*str];
        if (gi < 0) continue;
        const font_glyph *g = &font_glyphs[gi];
        if (g->w && g->h) {
            float gx = x + g->xoff * sc, gy = y + g->yoff * sc, gw = g->w * sc, gh = g->h * sc;
            for (int yy = (int)floorf(gy); yy < (int)ceilf(gy + gh); yy++) for (int xx = (int)floorf(gx); xx < (int)ceilf(gx + gw); xx++) {
                if (xx < 0 || yy < 0 || xx >= COVER_PX || yy >= COVER_PX) continue;
                float u = ((float)xx + 0.5f - gx) / sc - 0.5f, v = ((float)yy + 0.5f - gy) / sc - 0.5f;      /* position in the glyph bitmap */
                int ix = (int)floorf(u), iy = (int)floorf(v);
                float ax = u - (float)ix, ay = v - (float)iy, a = 0;
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                    int px = ix + dx, py = iy + dy;
                    if (px < 0 || py < 0 || px >= g->w || py >= g->h) continue;
                    a += (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay) * (float)font_atlas[(g->y + py) * FONT_ATLAS_W + g->x + px];
                }
                a /= 255.0f;
                unsigned char *o = buf + ((size_t)yy * COVER_PX + xx) * 4;
                for (int i = 0; i < 3; i++) o[i] = (unsigned char)((float)o[i] * (1 - a) + (float)rgb[i] * a);
            }
        }
        x += g->adv * sc;
    }
}
static void ios_cover_set(int idx) {
    static unsigned char buf[COVER_PX * COVER_PX * 4];
    const unsigned char *src = idx ? c64_8580_rgba : c64_6581_rgba;
    int sw = idx ? C64_8580_W : C64_6581_W, sh = idx ? 111 : C64_6581_H;           /* the 8580 picture has a pale blue reflection below the case (from row 111): cut off */
    float k = (float)COVER_PX * 0.86f / (float)sw, dw = sw * k, dh = sh * k, x0 = (COVER_PX - dw) / 2, y0 = (float)COVER_PX * 0.40f - dh / 2;      /* a little above the middle: the chip name goes below */
    for (int y = 0; y < COVER_PX; y++) {
        float t = (float)y / (float)COVER_PX;
        float bg[3] = { 12 + 10 * t, 16 + 12 * t, 30 + 18 * t };                          /* a soft dark gradient, like the app background */
        for (int x = 0; x < COVER_PX; x++) {
            float fx = (x + 0.5f - x0) / k - 0.5f, fy = (y + 0.5f - y0) / k - 0.5f, c[4] = { 0, 0, 0, 0 };
            if (fx > -1 && fy > -1 && fx < sw && fy < sh) {
                int ix = (int)floorf(fx), iy = (int)floorf(fy);
                float ax = fx - (float)ix, ay = fy - (float)iy;
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                    int px = ix + dx, py = iy + dy;
                    float w = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
                    if (px < 0 || py < 0 || px >= sw || py >= sh) continue;
                    const unsigned char *q = src + ((size_t)py * sw + px) * 4;
                    int qa = q[3];
                    if (qa < 24) continue;                                        /* the faintest edge pixels are dust: they shone as white dots */
                    for (int i = 0; i < 3; i++) c[i] += w * (float)(q[i] > qa ? qa : q[i]);      /* premultiplied: the colour cannot be brighter than the alpha */
                    c[3] += w * (float)qa;
                }
            }
            float inv = 1.0f - c[3] / 255.0f;
            unsigned char *o = buf + ((size_t)y * COVER_PX + x) * 4;
            for (int i = 0; i < 3; i++) { float v = c[i] + bg[i] * inv; o[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
            o[3] = 255;
        }
    }
    {   const char *chip = idx ? "MOS 8580" : "MOS 6581";
        static const unsigned char col_txt[3] = { 200, 212, 240 };
        const float size = 58;
        cover_text(buf, ((float)COVER_PX - text_width(chip, size)) / 2, y0 + dh + 30, size, col_txt, chip);
    }
    ios_media_set_cover(buf, COVER_PX, COVER_PX);
}
/* the first tune after a start in the background: the first entry of the playlist, the last favorite, or the start tune */
static void ios_start_idle(void) {
    if (g_tune || SDL_AtomicGet(&job.state)) return;
    if (npl) play_index(0, 0);
    else if (nfav) job_start(1, fav_find(fav_last) >= 0 ? fav_last : fav[0].rel);
    else job_start(1, "MUSICIANS/F/Feekzoid/A_Tribute_to_Martin_Galway.sid");
}
#if defined(__APPLE__) && TARGET_OS_TV
/* Apple TV: one press of Play/Pause arrives twice, as the key (SDL: PAUSE) and as a remote command of the system (play, pause or toggle, depending on what the system thinks the
 * state is). All of them are the same press: one toggle, the others within half a second are ignored. */
static void tv_press_toggle(void) {
    static Uint32 last; Uint32 t = SDL_GetTicks();
    if (last && t - last < 500) return;
    last = t;
    g_paused = !g_paused;
    if (!g_paused && (!g_dev || SDL_GetAudioDeviceStatus(g_dev) != SDL_AUDIO_PLAYING)) audio_recover();
}
#endif
static void ios_media_sync(void) {
    audio_watch();
    if (g_launch_bg && ios_app_active()) { g_launch_bg = 0; ios_start_idle(); }        /* the user opened the app: now it may start */
    {   static int cover_idx = -1;
        int idx = g_tune && g_tune->model[0] == 8580 ? 1 : 0;
        if (g_tune && idx != cover_idx) { cover_idx = idx; ios_cover_set(idx); }
    }
    int c; while ((c = ios_media_poll()) >= 0) {
#if defined(__APPLE__) && TARGET_OS_TV
        if (c == 0 || c == 1) { tv_press_toggle(); continue; }      /* play / pause / toggle of the Apple TV remote: one toggle per press */
#endif
        if (c == 0) { if (g_launch_bg && !g_tune) { g_launch_bg = 0; ios_start_idle(); } g_paused = 0; if (!g_dev || SDL_GetAudioDeviceStatus(g_dev) != SDL_AUDIO_PLAYING) audio_recover(); }
        else if (c == 1) g_paused = 1;
        else if (c == 2) step_tune(1);
        else if (c == 3) step_tune(-1);
    }
    static char mt[160], ma[96], mb[96]; static int mp = -1; static Uint32 mt0;
    char title[160] = "ngSid", artist[96] = "", album[96] = "";
    const tune_t *tn = g_tune;
    if (tn) {
        snprintf(title, sizeof title, "%s", tn->info.name[0] ? tn->info.name : (cur >= 0 ? pl[cur].label : "ngSid"));
        snprintf(artist, sizeof artist, "%s", tn->info.author);
        snprintf(album, sizeof album, "%s%sTune %d / %d", tn->info.released, tn->info.released[0] ? "  \xc2\xb7  " : "", tn->song, tn->info.songs);
    }
    int now = tn && !g_paused;
    Uint32 t = SDL_GetTicks();
    if (now != mp || strcmp(title, mt) || strcmp(artist, ma) || strcmp(album, mb) || t - mt0 > 15000) {
        mp = now; mt0 = t; snprintf(mt, sizeof mt, "%s", title); snprintf(ma, sizeof ma, "%s", artist); snprintf(mb, sizeof mb, "%s", album);
        ios_media_update(mt, ma, mb, now, (double)g_vis.frames / g_rate, 0);
    }
}
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
/* The Mac app: the media keys and the Now Playing display of the system (title, author, released, tune number, position) */
static void mac_media_sync(void) {
    int c; while ((c = mac_media_poll()) >= 0) {
        if (c == 0) g_paused = 0;
        else if (c == 1) g_paused = 1;
        else if (c == 2) step_tune(1);
        else if (c == 3) step_tune(-1);
    }
    static char mt[160], ma[96], mb[96]; static int mp = -1; static Uint32 mt0;
    char title[160] = "ngSid", artist[96] = "", album[96] = "";
    const tune_t *tn = g_tune;
    if (tn) {
        snprintf(title, sizeof title, "%s", tn->info.name[0] ? tn->info.name : (cur >= 0 ? pl[cur].label : "ngSid"));
        snprintf(artist, sizeof artist, "%s", tn->info.author);
        snprintf(album, sizeof album, "%s%sTune %d / %d", tn->info.released, tn->info.released[0] ? "  \xc2\xb7  " : "", tn->song, tn->info.songs);
    }
    int now = tn && !g_paused;
    Uint32 t = SDL_GetTicks();
    if (now != mp || strcmp(title, mt) || strcmp(artist, ma) || strcmp(album, mb) || t - mt0 > 15000) {
        mp = now; mt0 = t; snprintf(mt, sizeof mt, "%s", title); snprintf(ma, sizeof ma, "%s", artist); snprintf(mb, sizeof mb, "%s", album);
        mac_media_update(mt, ma, mb, now, (double)g_vis.frames / g_rate, 0);
    }
}
#endif

#ifndef __EMSCRIPTEN__
/* The HVSC path in a link of ours: .../player/#/MUSICIANS/K/KB/Katakis_Remix.sid, .../t/MUSICIANS/... or ngsid://MUSICIANS/... (decoded, no leading slash). A path ending in
 * "/" is a folder. The numeric form .../player/#97064 (database id of the HVSC) is only resolved by the web build. 0 = not a link of ours. */
__attribute__((unused)) static int url_hvsc_path(const char *u, char *out, size_t cap) {
    if (!u || (strncmp(u, "ngsid:", 6) && !(!strncmp(u, "http", 4) && strstr(u, "://ngsid.org/")))) return 0;       /* only the links of our own site and scheme */
    const char *p = NULL;
    if (!strncmp(u, "ngsid://", 8)) p = u + 8;
    else if ((p = strstr(u, "#/")) != NULL) p += 2;
    else if ((p = strstr(u, "/t/")) != NULL) p += 3;
    if (!p || !*p) return 0;
    size_t o = 0;
    for (; *p && o + 1 < cap; p++) {
        if (*p == '%' && p[1] && p[2]) { char h[3] = { p[1], p[2], 0 }; out[o++] = (char)strtol(h, NULL, 16); p += 2; }
        else if (*p == '?') break;
        else out[o++] = *p;
    }
    out[o] = 0;
    if (!o || strstr(out, "..")) return 0;
    return 1;
}
#endif
#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
/* ---- One ngSid for links: the "Open in ngSid" button of the web player opens ngsid://MUSICIANS/..., a running player takes the link over its socket (LINK <path>) ---- */
static char g_link[768];                                             /* an HVSC path from a link, waiting for the main loop (a path ending in / is a folder) */
static int ipc_fd = -1; static char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static void ipc_sockpath(void) {
    const char *r = getenv("XDG_RUNTIME_DIR"), *o = getenv("NGSID_SOCKET");
    if (o && *o) snprintf(ipc_path, sizeof ipc_path, "%s", o);
    else if (r && *r) snprintf(ipc_path, sizeof ipc_path, "%s/ngsid.sock", r); else snprintf(ipc_path, sizeof ipc_path, "/tmp/ngsid-%u.sock", (unsigned)getuid());
}
static int ipc_send_link(const char *path) {                          /* 1 = a running ngSid took the link */
    ipc_sockpath();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return 0;
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", ipc_path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return 0; }
    char msg[900]; int n = snprintf(msg, sizeof msg, "LINK %s\n", path); int ok = write(fd, msg, (size_t)n) == n; close(fd); return ok;
}
static void ipc_listen(void) {
    ipc_sockpath(); unlink(ipc_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return;
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", ipc_path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 8)) { close(fd); return; }
    chmod(ipc_path, 0600); fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); ipc_fd = fd;
}
static void ipc_close(void) { if (ipc_fd >= 0) { close(ipc_fd); ipc_fd = -1; unlink(ipc_path); } }
static void ipc_poll(void) {
    if (ipc_fd < 0) return;
    for (int k = 0; k < 4; k++) {
        int c = accept(ipc_fd, NULL, NULL); if (c < 0) return;
        char buf[1024]; size_t n = 0; struct pollfd p = { c, POLLIN, 0 };
        while (n < sizeof buf - 1 && poll(&p, 1, 100) > 0) { ssize_t r = read(c, buf + n, sizeof buf - 1 - n); if (r <= 0) break; n += (size_t)r; }
        close(c); buf[n] = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
            if (!strncmp(line, "LINK ", 5)) { const char *lg = getenv("NGSID_IPC_LOG"); if (lg) { FILE *f = fopen(lg, "a"); if (f) { fprintf(f, "LINK %s\n", line + 5); fclose(f); } } snprintf(g_link, sizeof g_link, "%s", line + 5); }
        if (win) { SDL_RestoreWindow(win); SDL_RaiseWindow(win); }
    }
}
static void link_play(const char *hp) {                              /* a tune plays, a folder opens in the browser */
    size_t hl = strlen(hp);
    if (hl && hp[hl - 1] == '/') { browse = 1; fav_mode = 0; g_plopen = 1; job_start(0, hp); }
    else job_start(1, hp);
}
#endif
static Uint32 *tv_menu_note;        /* Apple TV: the time of the last Menu press (see iter) */
static void iter(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
    ipc_poll();
    if (g_link[0]) { char hp[768]; snprintf(hp, sizeof hp, "%s", g_link); g_link[0] = 0; link_play(hp); }
#endif
#if defined(__APPLE__) && TARGET_OS_TV
    {   /* The system takes the Menu button over completely while "controller UI events" are on (the press goes Home whatever the app does with it): they are on only where Menu should
         * leave (top of the browser; not in the favorites list, not with the menu open); there the app gets the press as Escape (one level up / close). Not switched while a press is on its way. */
        static Uint32 menu_t; int pass = !tv_ctx_open && !fav_mode && !(browse && !br_at_root());
        if (SDL_GetTicks() - menu_t > 600) SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, pass ? "1" : "0");
        tv_menu_note = &menu_t;
        SDL_SetHint("NG_TV_MENU_TO_SYSTEM", pass ? "1" : "0");
        if (tv_dn_armed && SDL_GetTicks() - tv_dn_t >= 500) { tv_dn_armed = 0; tv_ctx_show(); }
        if (getenv("NGSID_TEST_TVCTX")) { static int o; if (!o && SDL_GetTicks() > 4000) { o = 1;
            if (getenv("NGSID_TEST_TVFAV")) { if (!nfav) { fav_add_raw("MUSICIANS/H/Hubbard_Rob/Commando.sid"); fav_add_raw("MUSICIANS/G/Galway_Martin/Comic_Bakery.sid"); } fav_mode = 1; browse = 0; }
            tv_ctx_show(); } }                                    /* test hook: the menu (and the favorites list) */
    }
#endif
        SDL_Event e;
        click = 0; rclick = 0;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_RIGHT) { rclick = 1; rclickx = (int)e.button.x; rclicky = (int)e.button.y; }
                if (e.button.button == SDL_BUTTON_LEFT) {
                    click = 1; clickx = (int)e.button.x; clicky = (int)e.button.y; mouse_down = 1;
                    lp_on = (g_mobile || g_portrait); lp_fired = 0; lp_swallow = 0; lp_x = clickx; lp_y = clicky; lp_t = SDL_GetTicks();
                }
                break;
            case SDL_MOUSEMOTION:
                if (lp_on && (abs(e.motion.x - lp_x) > 12 || abs(e.motion.y - lp_y) > 12)) lp_on = 0;   /* a drag is no long press */
                break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) { mouse_down = 0; lp_on = 0; } break;
            case SDL_MOUSEWHEEL:
                if (mx < 16 + 288 + 16) { if (browse) br_scroll -= e.wheel.y * 3; else list_scroll -= e.wheel.y * 3; }
                break;
            case SDL_DROPFILE: {
#if defined(__APPLE__) && TARGET_OS_IPHONE
                {   /* iOS: a link that opened the app (Universal Link https://ngsid.org/player/#/MUSICIANS/..., /t/..., or ngsid://...): play the tune / open the folder of the HVSC */
                    char hp[768];
                    if (url_hvsc_path(e.drop.file, hp, sizeof hp)) {
                        SDL_free(e.drop.file);
                        size_t hl = strlen(hp);
                        if (hl && hp[hl - 1] == '/') { browse = 1; fav_mode = 0; g_plopen = 1; job_start(0, hp); }       /* a folder */
                        else job_start(1, hp);                                                                   /* a tune: fetched from the HVSC and played */
                        break;
                    }
                }
#endif
                int was = npl;
                add_path(e.drop.file, 0);
                SDL_free(e.drop.file);
                if (npl > was && cur < 0) play_index(was, 0);
                break;
            }
#if defined(__APPLE__) && TARGET_OS_TV
            case SDL_KEYUP:                                                         /* Select acts on release (a long press opens the menu instead) */
                if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) && tv_dn_armed) {
                    tv_dn_armed = 0;
                    if (tv_ctx_open) break;
                    if (fav_mode) { if (tv_fav_sel >= 0 && tv_fav_sel < nfav && !SDL_AtomicGet(&job.state)) job_start(1, fav[tv_fav_sel].rel); }
                    else if (browse) browse_activate(br_sel);
                }
                break;
#endif
            case SDL_KEYDOWN:
#if defined(__APPLE__) && TARGET_OS_TV
                if (tv_ctx_open) {                                                  /* the menu is open: Up / Down choose, Select acts, Menu closes */
                    switch (e.key.keysym.sym) {
                    case SDLK_UP: if (tv_ctx_sel > 0) tv_ctx_sel--; break;
                    case SDLK_DOWN: if (tv_ctx_sel + 1 < tv_ctx_n) tv_ctx_sel++; break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: if (!e.key.repeat && SDL_GetTicks() - tv_ctx_t > 350) tv_ctx_act(); break;
                    case SDLK_ESCAPE: if (tv_menu_note) *tv_menu_note = SDL_GetTicks(); tv_ctx_open = 0; tv_ctx_t = SDL_GetTicks(); break;
                    default: break;
                    }
                    break;
                }
                {   int hd = 1;
                    switch (e.key.keysym.sym) {
                    case SDLK_RETURN: case SDLK_KP_ENTER: if (!e.key.repeat) { tv_dn_t = SDL_GetTicks(); tv_dn_armed = 1; } break;
                    case SDLK_UP: if (fav_mode) { if (tv_fav_sel > 0) tv_fav_sel--; } else hd = 0; break;
                    case SDLK_DOWN: if (fav_mode) { if (tv_fav_sel + 1 < nfav) tv_fav_sel++; } else hd = 0; break;
                    default: hd = 0; break;
                    }
                    if (hd) break;
                }
#endif
                switch (e.key.keysym.sym) {
#if defined(__APPLE__) && TARGET_OS_TV
                case SDLK_ESCAPE: if (tv_menu_note) *tv_menu_note = SDL_GetTicks();
                    if (tv_ctx_open) { tv_ctx_open = 0; tv_ctx_t = SDL_GetTicks(); }
                    else if (fav_mode) { fav_mode = 0; browse = 1; }
                    else if (browse && !br_at_root()) browse_up(); else running = 0;
                    break;                                                          /* Apple TV: the Menu button closes the menu / leaves the favorites / goes one level up, at the top it leaves */
#else
                case SDLK_ESCAPE: running = 0; break;
#endif
                case SDLK_SPACE: g_paused = !g_paused; break;
                case SDLK_PAUSE:
#if defined(__APPLE__) && TARGET_OS_TV
                    tv_press_toggle();
#else
                    g_paused = !g_paused;
#endif
                    break;
#if defined(__APPLE__) && TARGET_OS_TV
                case SDLK_LEFT: case SDLK_RIGHT: {   /* Apple TV: left / right switch the view (voices, notes, orb); the tunes of a multi-tune file are not on the remote */
                    static const int order[4] = { 0, 2, 1, 3 }; int pos = g_orb == 2 ? 1 : (g_orb == 1 ? 2 : (g_orb == 3 ? 3 : 0));
                    pos = (pos + (e.key.keysym.sym == SDLK_RIGHT ? 1 : 3)) % 4; g_orb = order[pos]; break; }
#else
                case SDLK_LEFT: change_song(-1); break;
                case SDLK_RIGHT: change_song(1); break;
#endif
                case SDLK_UP: if (browse) browse_move(-1); else change_file(-1); break;
                case SDLK_DOWN: if (browse) browse_move(1); else change_file(1); break;
                case SDLK_PAGEUP: if (browse) browse_move(-10); break;
                case SDLK_PAGEDOWN: if (browse) browse_move(10); break;
                case SDLK_HOME: if (browse) browse_move(-100000); break;
                case SDLK_END: if (browse) browse_move(100000); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: if (browse) browse_activate(br_sel); break;
                case SDLK_b: if (browse) browse = 0; else browse_open(); fav_mode = 0; break;
                case SDLK_h: if (cur >= 0 && cur < npl && pl[cur].rel) fav_toggle(pl[cur].rel); break;    /* favorite on / off for the tune that plays */
                case SDLK_v: g_orb = g_orb == 0 ? 2 : (g_orb == 2 ? 1 : (g_orb == 1 ? 3 : 0)); break;      /* voices -> notes -> orb -> info */
                case SDLK_BACKSPACE: if (browse) browse_up(); break;
                case SDLK_r: if (cur >= 0) play_index(cur, cur_song); break;
                case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
                    { unsigned bit = (unsigned)(e.key.keysym.sym - SDLK_1);
                      if (bit == 3) { int ds = digi_source(&g_vis); if (ds >= 0) bit = (unsigned)ds; }
                      g_mute ^= 1u << bit; }
                    apply_mute();
                    break;
                case SDLK_PLUS: case SDLK_KP_PLUS: case SDLK_EQUALS: g_vol += 0.05f; if (g_vol > 1) g_vol = 1; break;
                case SDLK_MINUS: case SDLK_KP_MINUS: g_vol -= 0.05f; if (g_vol < 0) g_vol = 0; break;
                case SDLK_f: case SDLK_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }
#if defined(__APPLE__) && TARGET_OS_IPHONE
        ios_media_sync();
        if (g_bg) { SDL_Delay(100); return; }               /* in the background: no drawing, the sound goes on */
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
        mac_media_sync();
#endif
        int ww, wh, dw, dh;
        SDL_GetWindowSize(win, &ww, &wh);
        gfx_drawable_size(win, &dw, &dh);
        int fdw = dw, fdh = dh, vpx = 0, vpy = 0;           /* the whole framebuffer; dw/dh become the part the UI is laid out in */
#if defined(__APPLE__) && TARGET_OS_IPHONE
#if TARGET_OS_TV
        g_duo = ww > wh;                                                  /* Apple TV: always the two pages (browser left, player right, 16:9) */
#else
        g_duo = ios_hinge_status(win) >= 2 && ww > wh;               /* iPhone Duo opened: the hinge is open and the picture is wide; it uses the whole window (time and Wi-Fi lie over it) */
#endif
        {   /* iOS: keep the UI out of the notch / Dynamic Island, the home bar and the rounded corners (insets in points) */
            float il, it, ir, ib;
            if ((!g_duo || NG_TV) && ww > 0 && wh > 0 && !ios_safe_insets(win, &il, &it, &ir, &ib)) {      /* (the Duo uses the whole window, the television keeps its safe area) */
#if TARGET_OS_TV
                {   /* Apple TV: the system asks for 90 points at the sides and 60 above and below (a television may cut the edges), which left 81 percent of the picture to the player.
                     * Modern televisions show every pixel: only a small part of it is kept free. */
                    if (il > 30) il = 30; if (ir > 30) ir = 30; if (it > 20) it = 20; if (ib > 20) ib = 20;
                }
#endif
                float kx = (float)fdw / ww, ky = (float)fdh / wh;
                int l = (int)(il * kx), t = (int)(it * ky), r = (int)(ir * kx), b = (int)(ib * ky);
#if TARGET_OS_TV
                tv_inset_px[0] = l; tv_inset_px[1] = t; tv_inset_px[2] = r; tv_inset_px[3] = b;      /* the picture (background) keeps the whole screen, only the cards keep this distance */
#else
                if (l + r < fdw / 3 && t + b < fdh / 3) { vpx = l; vpy = b; dw = fdw - l - r; dh = fdh - t - b; }      /* GL counts from below: the bottom inset is the y offset */
#endif
            }
        }
#endif
        /* portrait (aspect < 0.8) or too small for landscape: narrow stacked layout, virtual width 440 */
        g_portrait = dw * 10 < dh * 12 || dw < 640 || dh < 430;      /* aspect < 1.2: tablets upright, phones */
        if (g_duo) g_portrait = 1;                                   /* the phone layout is the right page, the list on the left works like on the phone */
        if (g_duo) {                                                   /* two pages side by side, 2 x 440 */
            view_scale = (float)dw / 920.0f;
            if ((float)dh / 640.0f < view_scale) view_scale = (float)dh / 640.0f;
        }
        else if (g_portrait) {                                              /* virtual width 440, but at least 640 high (tablets are wider than phones) */
            view_scale = (float)dw / 440.0f;
            if ((float)dh / 640.0f < view_scale) view_scale = (float)dh / 640.0f;
        }
        else {
            view_scale = (float)dw / 1280.0f;
            if ((float)dh / 860.0f < view_scale) view_scale = (float)dh / 860.0f;
            if (view_scale < 0.5f) view_scale = 0.5f;
        }
        view_w = (float)dw / view_scale;
        view_h = (float)dh / view_scale;
#if defined(__APPLE__) && TARGET_OS_TV
        g_tvi_l = (float)tv_inset_px[0] / view_scale; g_tvi_t = (float)tv_inset_px[1] / view_scale; g_tvi_r = (float)tv_inset_px[2] / view_scale; g_tvi_b = (float)tv_inset_px[3] / view_scale;
#endif
        int mxi, myi;
        Uint32 mb = SDL_GetMouseState(&mxi, &myi);
        mouse_down = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        float k = ww ? (float)fdw / (float)ww / view_scale : 1;
        mx = ((float)mxi - (float)vpx / ((float)fdw / (ww ? ww : 1))) * k; my = ((float)myi - (float)(fdh - dh - vpy) / ((float)fdh / (wh ? wh : 1))) * k;
        if (lp_on && !lp_fired && SDL_GetTicks() - lp_t >= 500) {      /* held still for half a second: open the menu like a right click */
            lp_fired = 1; lp_on = 0; lp_swallow = 1;
            rclick = 1; rclickx = lp_x; rclicky = lp_y;
        }
#if defined(__APPLE__) && TARGET_OS_IPHONE
        /* iOS: the event coordinates know nothing of the safe area: a tap is where the pointer is now (mx, my, already in view units) */
        if (click) { clickx = (int)mx; clicky = (int)my; }
        if (rclick) { rclickx = (int)mx; rclicky = (int)my; }
#else
        if (click) { clickx = (int)(clickx * k); clicky = (int)(clicky * k); }
        if (rclick) { rclickx = (int)(rclickx * k); rclicky = (int)(rclicky * k); }
#endif
        if (ctx_on && click) {                               /* click on the open menu: Share / Info; any click closes it */
            if (clickx >= ctx_x && clickx < ctx_x + CTX_W && clicky >= ctx_y && clicky < ctx_y + ctx_h()) {
                int jj = (int)((clicky - ctx_y - 1) / CTX_ROW);
                if (jj <= 0) ctx_share(); else if (jj == 1) ctx_info(); else if (ctx_n() == 3) fav_toggle(ctx_rel);
            }
            ctx_on = 0; click = 0; ctx_swallow = 1;
        }
        if (rclick && ctx_on) ctx_on = 0;

        {   /* test aid: NGSID_TEST_KEYS="down,down,return,menu,play" one key per 1.5 s, the first after 12 s (the Apple TV remote cannot be pressed from a script) */
            static char *kbuf; static char *kv[64]; static int nk = -1, ki; static Uint32 kt;
            if (nk < 0) {
                const char *e = getenv("NGSID_TEST_KEYS"); nk = 0;
                if (e) { kbuf = strdup(e); for (char *t = strtok(kbuf, ","); t && nk < 64; t = strtok(NULL, ",")) kv[nk++] = t; }
                kt = SDL_GetTicks() + 12000;
            }
            if (ki < nk && SDL_GetTicks() > kt) {
                static const struct { const char *n; SDL_Keycode k; } K[] = { { "up", SDLK_UP }, { "down", SDLK_DOWN }, { "left", SDLK_LEFT }, { "right", SDLK_RIGHT }, { "return", SDLK_RETURN }, { "menu", SDLK_ESCAPE }, { "play", SDLK_PAUSE } };
                for (size_t j = 0; j < sizeof K / sizeof *K; j++) if (!strcmp(kv[ki], K[j].n) || (!strcmp(kv[ki], "play2") && K[j].k == SDLK_PAUSE)) { SDL_Event ev; SDL_zero(ev); ev.type = SDL_KEYDOWN; ev.key.keysym.sym = K[j].k; SDL_PushEvent(&ev); if (!strcmp(kv[ki], "play2")) SDL_PushEvent(&ev); }
                ki++; kt = SDL_GetTicks() + 1500;
            }
        }
#if defined(__APPLE__) && TARGET_OS_TV
        {   /* Apple TV: the browser has no tab to tap, so its first listing is retried until it is there (the first try collides with the start tune that is fetched at the same time) */
            static Uint32 last_try;
            if (!nbe && br_at_root() && !SDL_AtomicGet(&job.state) && SDL_GetTicks() - last_try > 3000) { last_try = SDL_GetTicks(); job_start(0, BR_ROOT); }
        }
#endif
        job_poll();
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - tlast) / freq;
        tlast = now;
        if (dt > 0.1) dt = 0.1;

        static vis_t snap;
        if (g_dev) SDL_LockAudioDevice(g_dev);
        snap = g_vis;
        g_vis.peak[0] = g_vis.peak[1] = 0;
        if (g_dev) SDL_UnlockAudioDevice(g_dev);
        analyse(&snap, (float)dt);

        gfx_frame_begin(fdw, fdh, vpx, vpy, dw, dh, BG0.r, BG0.g, BG0.b);
        g_draw_t0 = SDL_GetPerformanceCounter();                       /* after the wait for the display: what follows is the time this program needs for the frame */
        gfx_set_params(view_w, view_h, view_scale);
        gfx_set_texture(fonttex);
        draw_ui(&snap, (double)(now - t0) / freq, dt);
#if defined(__APPLE__) && TARGET_OS_TV
        tv_ctx_draw(view_w, view_h);
#endif
        draw_ctx_menu(dt);
        set_additive(0);
        flush();
        if (shot && ++frame >= shot_frames) {
            save_screenshot(shot, dw, dh);
            running = 0;
        }
#if defined(__APPLE__) && TARGET_OS_IPHONE
        {   /* the AirPlay button: view units -> points (vpx / vpy is in pixels, GL counts the bottom inset as y) */
            int show = g_ap_seen && ww > 0 && wh > 0;
            if (show) ios_airplay_place(win, (((float)vpx + g_ap_x * view_scale) * ww / fdw), ((((float)(fdh - dh - vpy)) + g_ap_y * view_scale) * wh / fdh), 2 * g_ap_r * view_scale * ww / fdw, 1);
            else ios_airplay_place(win, 0, 0, 0, 0);
            g_ap_seen = 0;
        }
#endif
        g_draw_ms = (float)((double)(SDL_GetPerformanceCounter() - g_draw_t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
#if defined(__APPLE__) && TARGET_OS_TV
        tv_adapt(win, fdw, fdh);
#endif
        gfx_frame_end(win);
#ifndef __EMSCRIPTEN__
        double spent = (double)(SDL_GetPerformanceCounter() - now) / freq;
#if defined(__APPLE__) && TARGET_OS_TV
        {   /* Apple TV: the display paces (the wait for the next drawable). A sleep to the end of the period would push the frames against its beat, so it only goes to 85 percent of the
             * period: never faster than the screen (where nothing paces, as in the simulator) and the last part is left to the beat of the screen. */
            double period = 1.0 / (double)(tv_hz > 0 ? tv_hz : 60);
            if (spent < period * 0.85) SDL_Delay((Uint32)((period * 0.85 - spent) * 1000));
        }
#else
        if (spent < 0.012) SDL_Delay((Uint32)((0.0167 - spent) * 1000));   /* in case vsync is unavailable */
#endif
#endif
    }

int main(int argc, char **argv) {
    int first_song = 0;
    shot = NULL; shot_frames = 90;
    ngsid_config_default(&g_cfg);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pal")) g_pal_override = 1;
        else if (!strcmp(argv[i], "--ntsc")) g_pal_override = 0;
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) g_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--6581")) g_sid_model = 0;
        else if (!strcmp(argv[i], "--8580")) g_sid_model = 1;
        else if (!strcmp(argv[i], "--no-update-check")) g_check_updates = 0;
        else if (!strcmp(argv[i], "--version")) { printf("ngSid %s\n", NGSID_VERSION); return 0; }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) first_song = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shot_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [--pal|--ntsc] [--6581|--8580] [--rate N] [-s song]\n"
                   "       [--no-update-check] [--version]\n"
                   "       [--screenshot out.bmp [--frames N]] [file.sid|dir|ngsid://PATH ...]\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
        else if (url_hvsc_path(argv[i], g_link, sizeof g_link)) { if (!shot && ipc_send_link(g_link)) return 0; }        /* a link (ngsid://MUSICIANS/..., https://ngsid.org/player/#/MUSICIANS/...): a running ngSid plays it; else this one does */
#endif
        else add_path(argv[i], 0);
    }
#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
    if (!shot) ipc_listen();
#endif

    /* desktop integration: the Wayland app id must match ngsid.desktop (KDE takes the icon from it),
     * the X11 class matches StartupWMClass; unknown hints are ignored by older SDL versions */
    SDL_SetHint("SDL_APP_ID", "ngsid");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "ngSid");
#ifdef __EMSCRIPTEN__
    /* keyboard only from the canvas, so HTML inputs of the host page work normally */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");          /* iOS: audio session "playback" (the sound goes on with the screen locked, with the background mode "audio") */
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    gfx_window_attrs();
    win = SDL_CreateWindow("ngSid", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 860,
                                       gfx_window_flags() | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 360, 480);      /* narrow is fine: the portrait layout takes over */
    {
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)icon_rgba, ICON_W, ICON_H, 32, ICON_W * 4,
                                                             SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
#if !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE) && !defined(NG_NET_NATIVE)
    if (!shot) update_check_start();
#endif
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (window.ngFit) window.ngFit(); });
#endif
    if (gfx_init(win, MAXV)) return 1;
    vb = (vtx *)malloc(sizeof(vtx) * MAXV);
    for (int i = 0; i < 256; i++) glyph_idx[i] = -1;
    for (int i = 0; i < FONT_GLYPH_COUNT; i++) if (font_glyphs[i].code < 256) glyph_idx[font_glyphs[i].code] = i;
    fonttex = gfx_tex_create(FONT_ATLAS_W, FONT_ATLAS_H, 1, font_atlas);
    for (int i = 0; i < 2; i++)
        c64tex[i] = gfx_tex_create(i ? C64_8580_W : C64_6581_W, i ? C64_8580_H : C64_6581_H, 4, i ? c64_8580_rgba : c64_6581_rgba);
    if (!fonttex || !c64tex[0] || !c64tex[1]) return 1;

#ifdef __EMSCRIPTEN__
    g_mobile = EM_ASM_INT({ return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0; });
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
    g_mobile = 1;                                              /* iOS: the touch layout */
    SDL_AddEventWatch(ios_bg_watch, NULL);
#endif
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = g_rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = AUDIO_SAMPLES; want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
    else SDL_PauseAudioDevice(g_dev, 0);
#if defined(__APPLE__) && TARGET_OS_IPHONE
    ios_audio_longform();                                      /* after SDL set its audio session: long-form playback, so that AirPlay 2 targets (HomePod) are offered */
    ios_media_setup();
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
    mac_setup();
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
    {   /* iOS: the music folder of the app (Documents/Music, visible in the Files app); the HVSC cache goes to Library/Caches */
        char dp[1024], cp[1100]; const char *h = getenv("HOME");
        if (!ios_music_dir(dp, sizeof dp)) add_path(dp, 0);
        if (h && *h) { snprintf(cp, sizeof cp, "%s/Library/Caches", h); setenv("XDG_CACHE_HOME", cp, 1); }
        if (h && *h) { snprintf(cp, sizeof cp, "%s/Library/Application Support", h); setenv("XDG_CONFIG_HOME", cp, 1); }   /* favorites.txt and lastfav.txt: the top folder of the container is not writable on iOS */
    }
#endif
    fav_load();
    if (getenv("NGSID_TEST_FAV")) fav_mode = 1;                     /* test hook: start with the FAV tab */
    if (getenv("NGSID_TEST_BROWSE")) { browse = 1; fav_mode = 0; job_start(0, getenv("NGSID_TEST_BROWSE")); }     /* test hook: start in the HVSC browser, in this folder (e.g. MUSICIANS/H/Hubbard_Rob/) */
    if (getenv("NGSID_TEST_ORB")) g_orb = 1;                        /* test hook: start with the orb instead of the voices */
    if (getenv("NGSID_TEST_NOTES")) g_orb = 2;                      /* test hook: start with the notes */
    if (getenv("NGSID_TEST_INFO")) g_orb = 3;                       /* test hook: start with the info view */
    if (getenv("NGSID_TEST_TOGGLE")) fav_toggle(getenv("NGSID_TEST_TOGGLE"));   /* test hook: favorite on / off for this HVSC path at the start */
    if (getenv("NGSID_TEST_CTX") && nfav) { snprintf(ctx_rel, sizeof ctx_rel, "%s", fav[atoi(getenv("NGSID_TEST_CTX")) % nfav].rel); ctx_on = 1; ctx_x = 60; ctx_y = 60; }   /* test hook: the open context menu */
    fav_last_load();
    int fav_start = !npl && !shot && nfav && !getenv("NGSID_TEST_FAV") && !NG_TV;    /* nothing given and there are favorites: start with them */
    if (fav_start) { browse = 0; fav_mode = 1; }
    if (!npl && !shot && !fav_start) {   /* nothing given: look for SID files in ~/Musik, then ~/Music */
        const char *home = getenv("HOME");
        static const char *const dirs[] = { "Musik", "Music" };
        for (int i = 0; home && *home && i < 2 && !npl; i++) {
            char dir[1024];
            snprintf(dir, sizeof dir, "%s/%s", home, dirs[i]);
            add_path(dir, 0);
        }
    }
#if defined(__APPLE__) && TARGET_OS_IPHONE
    g_launch_bg = ios_launched_in_background();
#endif
    if (npl) { if (!g_launch_bg) play_index(0, first_song); }
#ifndef __EMSCRIPTEN__
    else if (fav_start) {      /* the last favorite that was played, or the first one */
        if (!g_launch_bg) job_start(1, fav_find(fav_last) >= 0 ? fav_last : fav[0].rel);
    }
    else if (!shot) {
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(NG_NET_NATIVE)
        /* nothing to play: fetch one tune from the HVSC right away (not bundled with the app), the browser opens in the background */
        if (!g_launch_bg) job_start(1, "MUSICIANS/F/Feekzoid/A_Tribute_to_Martin_Galway.sid");
#endif
        browse_open();   /* nothing given: start in the online HVSC browser */
    }
#endif

    running = 1;
    t0 = SDL_GetPerformanceCounter(); tlast = t0;
    freq = (double)SDL_GetPerformanceFrequency();
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(iter, 0, 1);
#else
    while (running) iter();
#endif
    if (g_dev) { SDL_CloseAudioDevice(g_dev); }
    tune_free(g_tune);
    gfx_shutdown();
#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
    ipc_close();
#endif
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
