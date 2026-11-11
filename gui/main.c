/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ngSid - OpenGL 3.3 SID player (NextGenSid).
 *
 *   ngSid [options] [file.sid|directory ...]     (no argument: opens the online HVSC browser)
 *
 * Window, input and audio come from SDL2; everything on screen is drawn through one small
 * batched OpenGL renderer (rounded rectangles as signed distance fields, soft glow lines,
 * a baked DejaVu Sans atlas). OpenGL is loaded at run time via SDL_GL_GetProcAddress, so
 * only libSDL2 is needed at link time.
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
#include "icon_data.h"
#include "hvsc.h"
#include "version.h"

#include <SDL2/SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ================================================================== OpenGL loader */
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
static int gl_load(void) { return 0; }
#define GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
#define GLSL_VERSION "#version 330 core\n"

typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef float GLfloat;
typedef char GLchar;
typedef long GLsizeiptr;
typedef long GLintptr;

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 4
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05

#define GLFUNCS(X) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLuint)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glEnable, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, unsigned char, GLsizei, const void *)) \
    X(void, glGenerateMipmap, (GLenum))

#define X(ret, name, args) static ret(*name) args;
GLFUNCS(X)
#undef X

static int gl_load(void) {
#define X(ret, name, args) \
    name = (ret(*) args)SDL_GL_GetProcAddress(#name); \
    if (!name) { fprintf(stderr, "missing OpenGL function %s\n", #name); return -1; }
    GLFUNCS(X)
#undef X
    return 0;
}
#endif

/* ================================================================== batched 2D renderer */

typedef struct { float r, g, b, a; } col;
typedef struct { float x, y, u, v, r, g, b, a, m, p1, p2, p3; } vtx;

#define MAXV 200000
static vtx *vb;
static int nv;
static int add_blend;
static GLuint prog, vao, vbo, fonttex;
static GLint u_res, u_pxs, u_tex;

static float view_w, view_h, view_scale = 1;
static int glyph_idx[256];

static const char *VS =
    GLSL_VERSION
    "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col;"
    "layout(location=3) in vec4 par;\n"
    "uniform vec2 res; out vec2 vuv; out vec4 vcol; out vec4 vpar;\n"
    "void main(){ vuv=uv; vcol=col; vpar=par;"
    " gl_Position=vec4(pos.x/res.x*2.0-1.0, 1.0-pos.y/res.y*2.0, 0.0, 1.0); }\n";

static const char *FS =
    GLSL_VERSION
    "in vec2 vuv; in vec4 vcol; in vec4 vpar; out vec4 o;\n"
    "uniform sampler2D tex; uniform float pxs;\n"
    "float sdbox(vec2 p, vec2 h, float r){ vec2 q=abs(p)-h+vec2(r); return length(max(q,0.0))+min(max(q.x,q.y),0.0)-r; }\n"
    "void main(){\n"
    " int m=int(vpar.x+0.5);\n"
    " if(m==0){ o=vcol; }\n"
    " else if(m==1){ float a=texture(tex,vuv).r; o=vec4(vcol.rgb,vcol.a*a); }\n"
    " else if(m==2){ float d=sdbox(vuv,vpar.yz,vpar.w); o=vec4(vcol.rgb,vcol.a*clamp(0.5-d*pxs,0.0,1.0)); }\n"
    " else if(m==3){ float t=abs(vuv.y); o=vec4(vcol.rgb,vcol.a*exp(-t*t*vpar.y)); }\n"
    " else if(m==5){ float d=sdbox(vuv,vpar.yz,vpar.w); float a=clamp(1.0-d/vpar.w,0.0,1.0); o=vec4(vcol.rgb,vcol.a*a*a); }\n"
    " else { o=vec4(vcol.rgb,vcol.a*exp(-dot(vuv,vuv)/(2.0*vpar.y*vpar.y))); }\n"
    "}\n";

static void flush(void) {
    if (!nv) return;
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)nv * sizeof(vtx)), vb);
    glDrawArrays(GL_TRIANGLES, 0, nv);
    nv = 0;
}

static void set_additive(int on) {
    if (on == add_blend) return;
    flush();
    add_blend = on;
    glBlendFunc(GL_SRC_ALPHA, on ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
}

static void V(float x, float y, float u, float v, col c, float m, float p1, float p2, float p3) {
    if (nv >= MAXV) flush();
    vtx *t = &vb[nv++];
    t->x = x; t->y = y; t->u = u; t->v = v;
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

static void tune_free(tune_t *t) {
    if (!t) return;
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
    t->e = engine_create(t->buf, (size_t)len, song, g_rate, &cfg, &eo, emsg, sizeof emsg);
    if (t->e) {
        t->s = engine_synth(t->e);
        t->nsid = (int)engine_sid_count(t->e);
        if (t->nsid > 3) t->nsid = 3;
        for (int i = 0; i < t->nsid; i++) t->model[i] = engine_sid_model(t->e, (unsigned)i);
        for (int i = 0; i < t->nsid; i++) t->syn[i] = engine_synth_at(t->e, (unsigned)i);
        for (int i = 0; i < t->nsid; i++) if (t->syn[i]) ngsid_set_voice_mute(t->syn[i], g_mute);
        engine_mute(t->e, g_mute);
    } else {
        snprintf(g_error, sizeof g_error, "init: %s", emsg);
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
    pl[npl].rel = NULL;
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
    int off = rpath[0] ? 1 : 0;
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
    browse = 1;
    if (!nbe && !rpath[0] && !SDL_AtomicGet(&job.state)) job_start(0, "");
}

static void browse_up(void) {
    if (!rpath[0]) return;
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
    int off = rpath[0] ? 1 : 0;
    if (off && row == 0) { browse_up(); return; }
    int i = row - off;
    if (i < 0 || i >= nbe) return;
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", rpath, be[i].name, be[i].dir ? "/" : "");
    job_start(be[i].dir ? 0 : 1, path);
}

static void browse_move(int delta) {
    int count = nbe + (rpath[0] ? 1 : 0);
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
    if (!npl) return;
    play_index(((cur + delta) % npl + npl) % npl, 0);
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
    if (glow[v] > 0.01f) {
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
    text(x + 16, y + 12, 13, DIM, "DIGI CHANNEL");
    if (src >= 0) {
        char sb[40];
        snprintf(sb, sizeof sb, "voice %d oscillator", src + 1);
        text(x + 16, y + 30, 11, FAINT, "Samples via the");
        text(x + 16, y + 44, 11, FAINT, sb);
    } else {
        text(x + 16, y + 30, 11, FAINT, "Samples via the");
        text(x + 16, y + 44, 11, FAINT, "volume register");
    }
    char b[48];
    snprintf(b, sizeof b, "%.1f kHz", rate / 1000.0f);
    text(x + 16, y + 66, 13, alpha(active && !muted ? dc : DIM, active ? 1.0f : 0.5f), active ? b : "--");
    float lx = x + 130, ly = y + 21;
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

/* ================================================================== Orb: sphere of dots deformed by the spectrum */
#define ORB_RINGS 78
#define ORB_LON 132
#define ORB_MAX 8000
static int g_portrait, g_plopen;      /* portrait (phones): stacked layout, playlist/browser as overlay */
static int g_orb;                       /* 1 = orb instead of the voice cards (key V / switch in the filter strip) */
static float orb_u[ORB_MAX][3];
static int orb_n;
static float orb_bs[NBANDS];

static void draw_orb(float x, float y, float w, float h, float dtf) {
    static int init;
    static float t, spin;
    if (!init) {
        for (int j = 0; j < ORB_RINGS && orb_n < ORB_MAX; j++) {
            float th = 3.14159265f * (j + 0.5f) / ORB_RINGS, yy = cosf(th), rr = sinf(th);
            int cnt = (int)(ORB_LON * rr + 0.5f);
            if (cnt < 6) cnt = 6;
            float off = (j & 1) ? 0.5f : 0.0f;
            for (int k = 0; k < cnt && orb_n < ORB_MAX; k++) {
                float ph = 6.2831853f * (k + off) / cnt;
                orb_u[orb_n][0] = cosf(ph) * rr; orb_u[orb_n][1] = yy; orb_u[orb_n][2] = sinf(ph) * rr;
                orb_n++;
            }
        }
        init = 1;
    }
    float dt = dtf > 0.1f ? 0.1f : dtf;
    float beat = g_paused ? 0.0f : g_beat;
    if (!g_paused) { t += dt; spin += dt * (0.20f + 0.45f * beat); }
    card(x, y, w, h, 0);
    text(x + 18, y + 12, 12, DIM, "ORB");
    for (int b = 0; b < NBANDS; b++) {
        float l = band[b > 0 ? b - 1 : b], r = band[b < NBANDS - 1 ? b + 1 : b];
        float v = 0.5f * band[b] + 0.25f * (l + r);
        orb_bs[b] += (v - orb_bs[b]) * (v > orb_bs[b] ? 0.7f : 0.25f);
    }
    float cx = x + w / 2, cy = y + h / 2 + 6, R = 0.26f * (w < h ? w : h);
    float ca = cosf(spin), sa = sinf(spin), ct = cosf(0.42f), st = sinf(0.42f), fz = 8.0f * R;
    set_additive(1);
    blob(cx, cy, R * (0.55f + 0.12f * beat), alpha(VC[0], 0.05f + 0.08f * beat));
    for (int i = 0; i < orb_n; i++) {
        float ux = orb_u[i][0], uy = orb_u[i][1], uz = orb_u[i][2];
        float lat = fabsf(uy), bf = lat * (NBANDS - 1);
        int b0 = (int)bf;
        if (b0 > NBANDS - 2) b0 = NBANDS - 2;
        float fr = bf - b0, lon = atan2f(uz, ux);
        float e = (orb_bs[b0] * (1 - fr) + orb_bs[b0 + 1] * fr) * (0.65f + 0.35f * sinf(lon * 3.0f + t * 1.3f + lat * 5.0f));
        if (e > 1) e = 1;
        float rad = R * (1.0f + 0.07f * beat + 0.55f * e);
        float px = ux * rad, py = uy * rad, pz = uz * rad;
        float rx = px * ca + pz * sa, rz = -px * sa + pz * ca;
        float ry = py * ct - rz * st, rz2 = py * st + rz * ct;
        float sc = fz / (fz + rz2), sx = cx + rx * sc, sy = cy + ry * sc;
        float dn = 0.5f - 0.5f * rz2 / (1.6f * R);
        if (dn < 0) dn = 0;
        if (dn > 1) dn = 1;
        col c = e < 0.5f ? mix(VC[0], VC[1], e * 2.0f) : mix(VC[1], VC[2], (e - 0.5f) * 2.0f);
        c = mix(c, C(1, 1, 1, 1), 0.18f * dn);
        blob(sx, sy, 0.60f + 0.75f * dn + 0.70f * e, alpha(c, (0.13f + 0.62f * dn) * (0.42f + 0.95f * e)));
    }
    set_additive(0);
}

/* ---- playlist / HVSC browser panel (sidebar in landscape, full overlay in portrait: tap selects, drag scrolls) */
static int sb_pd, sb_moved, sb_tap, sb_pressed;
static float sb_py0, sb_sc0;
static int sb_click(float x, float y, float w, float h) {
    if (!g_portrait) return clicked(x, y, w, h);
    return sb_tap && mx >= x && mx < x + w && my >= y && my < y + h;
}
static void draw_sidebar(float x, float y, float w, float h) {
    sb_tap = 0; sb_pressed = 0;
    if (g_portrait) {
        if (mouse_down) {
            if (!sb_pd) { sb_pd = 1; sb_py0 = my; sb_moved = 0; sb_pressed = 1; }
            if (fabsf(my - sb_py0) > 8) sb_moved = 1;
        } else if (sb_pd) { sb_pd = 0; if (!sb_moved) sb_tap = 1; }
    }
        card(x, y, w, h, 0);
        float tw1 = text_width("PLAYLIST", 13);
        text(x + 18, y + 14, 13, browse ? FAINT : TXT, "PLAYLIST");
        text(x + 18 + tw1 + 22, y + 14, 13, browse ? TXT : FAINT, "HVSC ONLINE");
        if (sb_click(x + 12, y + 8, tw1 + 16, 26)) { browse = 0; click = 0; }
        else if (sb_click(x + 18 + tw1 + 14, y + 8, text_width("HVSC ONLINE", 13) + 16, 26)) { browse_open(); click = 0; }
        int boff = rpath[0] ? 1 : 0;
        int count = browse ? nbe + boff : npl;
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%d", browse ? nbe : npl);
        text_right(x + w - 18, y + 14, 13, DIM, cnt);
        float ry = y + 44, rh = g_portrait ? 36 : 28, area = h - 44 - 40;
        if (browse) {
            char crumb[800];
            snprintf(crumb, sizeof crumb, "/%s", rpath);
            fit_text(crumb, w - 36, 12);
            text(x + 18, ry, 12, FAINT, crumb);
            ry += 22; area -= 22 + 18;
        }
        float *sc = browse ? &br_scroll : &list_scroll;
        if (sb_pressed) sb_sc0 = *sc;
        if (g_portrait && sb_pd && sb_moved) *sc = sb_sc0 + (sb_py0 - my) / rh;
        int vis_rows = (int)(area / rh);
        int maxscroll = count > vis_rows ? count - vis_rows : 0;
        if (!browse && list_follow && cur >= 0) {
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
            int isc = !browse && i == cur, anc = 0;
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
            if (!browse)
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
            if (!browse && pl[i].rel) {          /* tune that came from the online HVSC */
                rrect(lx, yy + 6, 26, 17, 4, alpha(VC[2], isc ? 0.32f : 0.20f));
                text(lx + 4, yy + 8, 11, VC[2], "HV");
                lx += 33; lw -= 33;
            }
            fit_text(lab, lw, 14);
            text(lx, yy + 5, 14, isc || isdir ? TXT : DIM, lab);
            if (sb_click(x + 8, yy, w - 16, rh)) {
                if (browse) { br_sel = i; browse_activate(i); } else play_index(i, 0);
                click = 0;
                if (g_portrait && (!browse || !isdir)) g_plopen = 0;
            }
        }
        if (!browse && !npl) {
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
        set_additive(1);
        for (int b = 0; b < NBANDS; b += 2) {
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
static void draw_ui_portrait(const vis_t *vs, double t, double dt) {
    (void)t;
    const float M = 10, BOT = 64, gap = 8;
    float W = view_w, H = view_h, aw = W - 2 * M;
    static const tune_t *last_tune;
    static int auto_done;
    if (g_tune != last_tune) { last_tune = g_tune; if (g_tune) g_plopen = 0; }
    if (!auto_done) { auto_done = 1; if (browse && !g_tune) g_plopen = 1; }
    const tune_t *tn = g_tune;
    const int nsid = tn && tn->nsid > 1 ? (tn->nsid > 3 ? 3 : tn->nsid) : 1;

    /* ---------------- header */
    float hy = M, hh = 100;
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
        float tmax = aw - 32 - 96, tsize = 22;
        while (tsize > 15 && text_width(title, tsize) > tmax) tsize -= 1;
        fit_text(title, tmax, tsize);
        text(M + 16, hy + 10, tsize, TXT, title);
        fit_text(sub, tmax, 13);
        if (g_error[0]) text(M + 16, hy + 14 + tsize, 12, WARN, g_error);
        else text(M + 16, hy + 14 + tsize, 13, DIM, sub);
        char tb[32];
        mmss(tb, sizeof tb, (double)vs->frames / g_rate);
        text_right(M + aw - 16, hy + 10, 28, TXT, tb);
        if (tn) {
            char b[48];
            snprintf(b, sizeof b, "%d / %d", tn->song, tn->info.songs);
            text_right(M + aw - 16, hy + 44, 13, VC[0], b);
            if (g_paused) text_right(M + aw - 16, hy + 62, 12, VC[2], "PAUSED");
            float cx = M + 16, cy = hy + hh - 30;
            cx += chip(cx, cy, tn->info.rsid ? "RSID" : "PSID", VC[0], 1);
            cx += chip(cx, cy, tn->pal ? "PAL" : "NTSC", VC[2], 1);
            if (tn->nsid == 1 && tn->model[0]) { snprintf(b, sizeof b, "MOS %d", tn->model[0]); cx += chip(cx, cy, b, DIM, 1); }
            else if (tn->nsid > 1) { snprintf(b, sizeof b, "%d SIDs", tn->nsid); cx += chip(cx, cy, b, DIM, 1); }
        }
    }

    /* ---------------- vertical budget */
    float by = H - M - BOT, sh = 96, sy = by - M - sh, fh = 46, fy = sy - M - fh;
    float cy0 = hy + hh + M, midh = fy - M - cy0;

    if (g_plopen) {
        draw_sidebar(M, cy0, aw, sy + sh - cy0);
        click = 0;
    } else {
        /* ---------------- voices + digi, or the orb */
        if (g_orb) draw_orb(M, cy0, aw, midh, (float)dt);
        else {
            float dh = 64;
            int rows = nsid == 1 ? 3 : nsid;
            float rhv = (midh - dh - M - gap * (float)(rows - 1)) / (float)rows;
            float rmax = nsid == 1 ? 120.0f : 90.0f;
            if (rhv > rmax) rhv = rmax;
            if (rhv < 76) rhv = 76;
            float vw3 = (aw - 2 * gap) / 3;
            for (int c = 0; c < rows; c++) {
                if (nsid == 1) {
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
            float sw = 70, sx0 = M + aw - 14 - 2 * sw;
            for (int i = 0; i < 2; i++) {
                float xx = sx0 + i * sw;
                int on = (i == 1) == (g_orb != 0), hv = hit(xx, cy - 2, sw - 4, 20);
                rrect(xx, cy - 2, sw - 4, 20, 6, on ? alpha(VC[2], 0.25f) : alpha(FAINT, hv ? 0.30f : 0.16f));
                const char *l = i ? "ORB" : "VOICES";
                text(xx + (sw - 4 - text_width(l, 11)) / 2, cy + 2, 11, on ? VC[2] : alpha(DIM, 0.9f), l);
                if (clicked(xx, cy - 2, sw - 4, 20)) { g_orb = i; click = 0; }
            }
        }
        float vuw = 130, spw = aw - vuw - M;
        draw_spectrum_card(M, sy, spw, sh);
        draw_level_card(M + spw + M, sy, vuw, sh);
    }

    /* ---------------- transport */
    {
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
                if (i == 0) change_song(-1);
                else if (i == 2) change_song(1);
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

static void draw_ui(const vis_t *vs, double t, double dt) {
    float W = view_w, H = view_h;
    int vol_changed = 0;
    (void)dt;

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
    set_additive(1);
    blob(W * 0.25f, -30, 380, alpha(VC[0], 0.06f + 0.05f * glow[0]));
    blob(W * 0.60f, -60, 420, alpha(VC[1], 0.05f + 0.05f * glow[1]));
    blob(W * 0.92f, H * 0.9f, 380, alpha(VC[2], 0.04f + 0.05f * glow[2]));
    set_additive(0);

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
        float tsize = 32;
        while (tsize > 18 && text_width(title, tsize) > tmax) tsize -= 2;
        text(ax + 22, hy + 14, tsize, TXT, title);
        fit_text(sub, tmax, 16);
        text(ax + 22, hy + 14 + tsize + 6, 16, DIM, sub);

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
        if (g_error[0]) text(ax + 22, hy + hh - 54, 13, WARN, g_error);
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
    if (g_orb) draw_orb(ax, vy, aw, dy + dh - vy, (float)dt);
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
            float sw = 70, sx0 = ax + aw - 18 - 150 - 2 * sw;
            for (int i = 0; i < 2; i++) {
                float xx = sx0 + i * sw;
                int on = (i == 1) == (g_orb != 0), hv = hit(xx, cy - 2, sw - 4, 20);
                rrect(xx, cy - 2, sw - 4, 20, 6, on ? alpha(VC[2], 0.25f) : alpha(FAINT, hv ? 0.30f : 0.16f));
                const char *l = i ? "ORB" : "VOICES";
                text(xx + (sw - 4 - text_width(l, 11)) / 2, cy + 2, 11, on ? VC[2] : alpha(DIM, 0.9f), l);
                if (clicked(xx, cy - 2, sw - 4, 20)) { g_orb = i; click = 0; }
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
                if (btn[i].kind == 0) change_song(-1);
                else if (btn[i].kind == 2) change_song(1);
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
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) memcpy(fl + (size_t)y * (size_t)w * 4, px + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
    SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(fl, w, h, 32, w * 4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (sf) { SDL_SaveBMP(sf, file); SDL_FreeSurface(sf); }
    free(px); free(fl);
}

static int gl_compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "shader error: %s\n", log);
        return 0;
    }
    return (int)sh;
}

/* ---------------------------------------------------------------- main loop (a function so it can also run in the browser) */
#ifdef __EMSCRIPTEN__
#define AUDIO_SAMPLES 2048
#else
#define AUDIO_SAMPLES 1024
#endif
static SDL_Window *win;
static SDL_GLContext ctx;
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

static void iter(void) {
        SDL_Event e;
        click = 0;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) { click = 1; clickx = (int)e.button.x; clicky = (int)e.button.y; mouse_down = 1; }
                break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) mouse_down = 0; break;
            case SDL_MOUSEWHEEL:
                if (mx < 16 + 288 + 16) { if (browse) br_scroll -= e.wheel.y * 3; else list_scroll -= e.wheel.y * 3; }
                break;
            case SDL_DROPFILE: {
                int was = npl;
                add_path(e.drop.file, 0);
                SDL_free(e.drop.file);
                if (npl > was && cur < 0) play_index(was, 0);
                break;
            }
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_SPACE: g_paused = !g_paused; break;
                case SDLK_LEFT: change_song(-1); break;
                case SDLK_RIGHT: change_song(1); break;
                case SDLK_UP: if (browse) browse_move(-1); else change_file(-1); break;
                case SDLK_DOWN: if (browse) browse_move(1); else change_file(1); break;
                case SDLK_PAGEUP: if (browse) browse_move(-10); break;
                case SDLK_PAGEDOWN: if (browse) browse_move(10); break;
                case SDLK_HOME: if (browse) browse_move(-100000); break;
                case SDLK_END: if (browse) browse_move(100000); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: if (browse) browse_activate(br_sel); break;
                case SDLK_b: if (browse) browse = 0; else browse_open(); break;
                case SDLK_v: g_orb = !g_orb; break;
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
        int ww, wh, dw, dh;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        /* portrait (aspect < 0.8) or too small for landscape: narrow stacked layout, virtual width 440 */
        g_portrait = dw * 10 < dh * 8 || dw < 640 || dh < 430;
        if (g_portrait) view_scale = (float)dw / 440.0f;
        else {
            view_scale = (float)dw / 1280.0f;
            if ((float)dh / 860.0f < view_scale) view_scale = (float)dh / 860.0f;
            if (view_scale < 0.5f) view_scale = 0.5f;
        }
        view_w = (float)dw / view_scale;
        view_h = (float)dh / view_scale;
        int mxi, myi;
        Uint32 mb = SDL_GetMouseState(&mxi, &myi);
        mouse_down = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        float k = ww ? (float)dw / (float)ww / view_scale : 1;
        mx = (float)mxi * k; my = (float)myi * k;
        if (click) { clickx = (int)(clickx * k); clicky = (int)(clicky * k); }

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

        glViewport(0, 0, dw, dh);
        glClearColor(BG0.r, BG0.g, BG0.b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform2f(u_res, view_w, view_h);
        glUniform1f(u_pxs, view_scale);
        glUniform1i(u_tex, 0);
        glBindVertexArray(vao);
        glBindTexture(GL_TEXTURE_2D, fonttex);
        draw_ui(&snap, (double)(now - t0) / freq, dt);
        set_additive(0);
        flush();
        if (shot && ++frame >= shot_frames) {
            save_screenshot(shot, dw, dh);
            running = 0;
        }
        SDL_GL_SwapWindow(win);
#ifndef __EMSCRIPTEN__
        double spent = (double)(SDL_GetPerformanceCounter() - now) / freq;
        if (spent < 0.012) SDL_Delay((Uint32)((0.0167 - spent) * 1000));   /* in case vsync is unavailable */
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
                   "       [--screenshot out.bmp [--frames N]] [file.sid|dir ...]\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else add_path(argv[i], 0);
    }

    /* desktop integration: the Wayland app id must match ngsid.desktop (KDE takes the icon from it),
     * the X11 class matches StartupWMClass; unknown hints are ignored by older SDL versions */
    SDL_SetHint("SDL_APP_ID", "ngsid");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "ngSid");
#ifdef __EMSCRIPTEN__
    /* keyboard only from the canvas, so HTML inputs of the host page work normally */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
    win = SDL_CreateWindow("ngSid", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 860,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 360, 480);      /* narrow is fine: the portrait layout takes over */
    {
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)icon_rgba, ICON_W, ICON_H, 32, ICON_W * 4,
                                                             SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
#ifndef __EMSCRIPTEN__
    if (!shot) update_check_start();
#endif
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (window.ngFit) window.ngFit(); });
#endif
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "OpenGL 3.3 context: %s\n", SDL_GetError()); return 1; }
    if (gl_load()) return 1;
    SDL_GL_SetSwapInterval(1);

    int vsh = gl_compile(GL_VERTEX_SHADER, VS), fsh = gl_compile(GL_FRAGMENT_SHADER, FS);
    if (!vsh || !fsh) return 1;
    prog = glCreateProgram();
    glAttachShader(prog, (GLuint)vsh); glAttachShader(prog, (GLuint)fsh);
    glLinkProgram(prog);
    GLint lk = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &lk);
    if (!lk) { char log[1024]; glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "link: %s\n", log); return 1; }
    u_res = glGetUniformLocation(prog, "res");
    u_pxs = glGetUniformLocation(prog, "pxs");
    u_tex = glGetUniformLocation(prog, "tex");

    vb = (vtx *)malloc(sizeof(vtx) * MAXV);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vtx) * MAXV), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(vtx), (void *)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(vtx), (void *)16);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_FLOAT, 0, sizeof(vtx), (void *)32);

    for (int i = 0; i < 256; i++) glyph_idx[i] = -1;
    for (int i = 0; i < FONT_GLYPH_COUNT; i++) if (font_glyphs[i].code < 256) glyph_idx[font_glyphs[i].code] = i;
    glGenTextures(1, &fonttex);
    glBindTexture(GL_TEXTURE_2D, fonttex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_W, FONT_ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, font_atlas);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = g_rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = AUDIO_SAMPLES; want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
    else SDL_PauseAudioDevice(g_dev, 0);

    if (!npl && !shot) {   /* nothing given: look for SID files in ~/Musik, then ~/Music */
        const char *home = getenv("HOME");
        static const char *const dirs[] = { "Musik", "Music" };
        for (int i = 0; home && *home && i < 2 && !npl; i++) {
            char dir[1024];
            snprintf(dir, sizeof dir, "%s/%s", home, dirs[i]);
            add_path(dir, 0);
        }
    }
    if (npl) play_index(0, first_song);
#ifndef __EMSCRIPTEN__
    else if (!shot) browse_open();   /* nothing given: start in the online HVSC browser */
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
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
