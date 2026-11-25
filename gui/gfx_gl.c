/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gfx_gl.c - OpenGL backend of gfx.h: OpenGL 3.3 core on the desktop (loaded at run time via SDL_GL_GetProcAddress), OpenGL ES 3 in the browser and on iOS. */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define NG_GLES 1                      /* OpenGL ES 3: the browser and iOS */
#endif

/* ================================================================== OpenGL loader */
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
static int gl_load(void) { return 0; }
#define GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#elif defined(NG_GLES)
#include <OpenGLES/ES3/gl.h>
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
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_LESS 0x0201
#define GL_UNSIGNED_SHORT 0x1403
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0

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
    X(void, glGenerateMipmap, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glDepthMask, (unsigned char)) \
    X(void, glCullFace, (GLenum)) \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, unsigned char, const GLfloat *)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum))

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

/* ================================================================== the shader (one for everything: the mode of a vertex selects the look) */

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
    " else if(m==7){ vec4 t=texture(tex,vuv); o=vec4(t.rgb/max(t.a,0.001),t.a*vcol.a); }\n"
    " else if(m==5){ float d=sdbox(vuv,vpar.yz,vpar.w); float a=clamp(1.0-d/vpar.w,0.0,1.0); o=vec4(vcol.rgb,vcol.a*a*a); }\n"
    " else { o=vec4(vcol.rgb,vcol.a*exp(-dot(vuv,vuv)/(2.0*vpar.y*vpar.y))); }\n"
    "}\n";

/* ================================================================== backend */

struct gfx_tex { GLuint id, fbo, depth; int w, h; };
struct gfx_mesh { GLuint vao, vbo, ebo; int n; };

static GLuint prog, vao, vbo;
static int g_vp[4];                          /* the viewport of the 2D pass (restored after a 3D pass) */
static GLint u_res, u_pxs, u_tex;
static SDL_GLContext ctx;

Uint32 gfx_window_flags(void) { return SDL_WINDOW_OPENGL; }

void gfx_window_attrs(void) {
#ifdef NG_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
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

int gfx_init(SDL_Window *win, int max_vtx) {
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "OpenGL 3.3 context: %s\n", SDL_GetError()); return -1; }
    if (gl_load()) return -1;
    SDL_GL_SetSwapInterval(1);

    int vsh = gl_compile(GL_VERTEX_SHADER, VS), fsh = gl_compile(GL_FRAGMENT_SHADER, FS);
    if (!vsh || !fsh) return -1;
    prog = glCreateProgram();
    glAttachShader(prog, (GLuint)vsh); glAttachShader(prog, (GLuint)fsh);
    glLinkProgram(prog);
    GLint lk = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &lk);
    if (!lk) { char log[1024]; glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "link: %s\n", log); return -1; }
    u_res = glGetUniformLocation(prog, "res");
    u_pxs = glGetUniformLocation(prog, "pxs");
    u_tex = glGetUniformLocation(prog, "tex");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(gfx_vtx) * (size_t)max_vtx), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(gfx_vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(gfx_vtx), (void *)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(gfx_vtx), (void *)16);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_FLOAT, 0, sizeof(gfx_vtx), (void *)32);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return 0;
}

void gfx_shutdown(void) { if (ctx) SDL_GL_DeleteContext(ctx); ctx = NULL; }

void gfx_drawable_size(SDL_Window *win, int *w, int *h) { SDL_GL_GetDrawableSize(win, w, h); }

gfx_tex *gfx_tex_create(int w, int h, int channels, const void *px) {
    gfx_tex *t = (gfx_tex *)calloc(1, sizeof *t);
    if (!t) return NULL;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, channels == 1 ? GL_R8 : GL_RGBA, w, h, 0, channels == 1 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void gfx_frame_begin(int fw, int fh, int vx, int vy, int vw, int vh, float r, float g, float b) {
    glViewport(0, 0, fw, fh);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vx, vy, vw, vh);
    g_vp[0] = vx; g_vp[1] = vy; g_vp[2] = vw; g_vp[3] = vh;
    glUseProgram(prog);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
}

void gfx_set_params(float res_w, float res_h, float pxs) {
    glUniform2f(u_res, res_w, res_h);
    glUniform1f(u_pxs, pxs);
    glUniform1i(u_tex, 0);
}

void gfx_set_texture(gfx_tex *t) { glBindTexture(GL_TEXTURE_2D, t->id); }

void gfx_set_additive(int on) { glBlendFunc(GL_SRC_ALPHA, on ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); }

void gfx_draw(const gfx_vtx *v, int n) {
    if (n <= 0) return;
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)n * sizeof(gfx_vtx)), v);
    glDrawArrays(GL_TRIANGLES, 0, n);
}

void gfx_frame_end(SDL_Window *win) { SDL_GL_SwapWindow(win); }
float gfx_last_gpu_ms(void) { return 0; }
void gfx_native_size(SDL_Window *win, int *w, int *h) { gfx_drawable_size(win, w, h); }
void gfx_set_render_scale(float s) { (void)s; }
int gfx_refresh_hz(SDL_Window *win) { (void)win; return 60; }

void gfx_read_pixels(int w, int h, unsigned char *rgba) {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}


/* ================================================================== the 3D C64 */
static const char *C64_VS =
    GLSL_VERSION
    "layout(location=0) in vec3 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col;\n"
    "uniform mat4 mvp; uniform mat4 mv; out vec3 vv; out vec3 vp; out vec2 vuv; out vec4 vcol;\n"
    "void main(){ vec4 p=vec4(pos,1.0); vv=(mv*p).xyz; vp=pos; vuv=uv; vcol=col; gl_Position=mvp*p; }\n";
/* umode 0: solid colour, 1: textured, 2: glass (alpha). uled = hue, spread (how much the hue changes across the case), saturation, strength of the light inside: the light is a
 * rainbow that runs over the C64 (the hue follows the position), it lights the keys from below, the board and the case */
static const char *C64_FS =
    GLSL_VERSION
    "in vec3 vv; in vec3 vp; in vec2 vuv; in vec4 vcol; out vec4 o; uniform sampler2D tex; uniform int umode; uniform vec4 uled;\n"
    "vec3 hsv(float h,float s,float v){ vec3 k=clamp(abs(fract(h+vec3(0.0,2.0/3.0,1.0/3.0))*6.0-3.0)-1.0,0.0,1.0); return v*mix(vec3(1.0),k,s); }\n"
    "void main(){\n"
    " vec3 n=normalize(cross(dFdx(vv),dFdy(vv))); if(dot(n,-vv)<0.0) n=-n;\n"
    " float d=max(dot(n,normalize(vec3(0.33,0.52,0.79))),0.0);\n"
    " float sp=0.65+0.35*sin(vp.x*38.0+uled.x*40.0)*sin(vp.z*53.0-uled.x*25.0);\n"
    " vec3 L=hsv(uled.x+vp.x*uled.y+vp.z*0.18,uled.z,1.0)*uled.w;\n"
    " if(umode==2){\n"
    "  float rim=pow(1.0-abs(dot(n,normalize(-vv))),2.0);\n"
    "  vec3 c=vcol.rgb*(0.45+0.45*d)+L*(0.55+1.0*rim);\n"
    "  float a=clamp(vcol.a+0.45*rim+0.12*uled.w,0.0,0.95);\n"
    "  o=vec4(c*a,a); }\n"
    " else if(umode==3){\n"
    "  vec3 lc=hsv(uled.x+vp.x*uled.y*2.0+vp.z*0.5,0.9,1.0);\n"
    "  o=vec4(mix(vcol.rgb,lc,0.75)*(0.8+1.6*uled.w),1.0); }\n"
    " else {\n"
    "  vec3 base=umode==1?texture(tex,vuv).rgb:vcol.rgb;\n"
    "  float sh=umode==1?0.62+0.55*d:0.36+0.64*d;\n"
    "  o=vec4(base*sh+L*(umode==1?0.75*sp:0.30*sp),1.0); } }\n";

static GLuint c64_prog; static GLint c64_mvp, c64_mv, c64_umode, c64_utex, c64_uled;

gfx_mesh *gfx_mesh_create(const gfx_v3 *verts, int nverts, const uint16_t *idx, int nidx) {
    gfx_mesh *m = (gfx_mesh *)calloc(1, sizeof *m);
    if (!m) return NULL;
    glGenVertexArrays(1, &m->vao); glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo); glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nverts * sizeof(gfx_v3)), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(gfx_v3), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(gfx_v3), (void *)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(gfx_v3), (void *)20);
    glGenBuffers(1, &m->ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)nidx * 2), idx, GL_STATIC_DRAW);
    m->n = nidx;
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);          /* back to the 2D renderer's vertex array */
    return m;
}

int gfx_target_v_flipped(void) { return 1; }

gfx_tex *gfx_target_create(int w, int h) {
    gfx_tex *t = (gfx_tex *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->w = w; t->h = h;
    glGenTextures(1, &t->id); glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &t->depth); glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &t->fbo); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "render target incomplete\n"); }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t;
}

void gfx_c64_render(gfx_tex *target, const float *mvp, const float *mv, gfx_mesh *solid, gfx_mesh *keys, gfx_tex *keys_tex, gfx_mesh *badge, gfx_tex *badge_tex, gfx_mesh *glass, gfx_mesh *leds, const float *led) {
    if (!c64_prog) {
        c64_prog = glCreateProgram();
        GLuint vs = (GLuint)gl_compile(GL_VERTEX_SHADER, C64_VS), fs = (GLuint)gl_compile(GL_FRAGMENT_SHADER, C64_FS);
        glAttachShader(c64_prog, vs); glAttachShader(c64_prog, fs); glLinkProgram(c64_prog);
        GLint ok = 0; glGetProgramiv(c64_prog, GL_LINK_STATUS, &ok);
        if (!ok) { char log[1024]; glGetProgramInfoLog(c64_prog, sizeof log, NULL, log); fprintf(stderr, "c64 link: %s\n", log); }
        c64_mvp = glGetUniformLocation(c64_prog, "mvp"); c64_mv = glGetUniformLocation(c64_prog, "mv");
        c64_umode = glGetUniformLocation(c64_prog, "umode"); c64_utex = glGetUniformLocation(c64_prog, "tex"); c64_uled = glGetUniformLocation(c64_prog, "uled");
    }
    GLint vp0[4] = { 0 }; (void)vp0;
    glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    glViewport(0, 0, target->w, target->h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glUseProgram(c64_prog);
    glUniformMatrix4fv(c64_mvp, 1, 0, mvp); glUniformMatrix4fv(c64_mv, 1, 0, mv);
    glUniform1i(c64_utex, 0); glUniform4f(c64_uled, led[0], led[1], led[2], led[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(solid->vao); glUniform1i(c64_umode, 0);
    glDrawElements(GL_TRIANGLES, solid->n, GL_UNSIGNED_SHORT, (const void *)0);
    glUniform1i(c64_umode, 1);
    glBindTexture(GL_TEXTURE_2D, keys_tex->id); glBindVertexArray(keys->vao);
    glDrawElements(GL_TRIANGLES, keys->n, GL_UNSIGNED_SHORT, (const void *)0);
    glBindTexture(GL_TEXTURE_2D, badge_tex->id); glBindVertexArray(badge->vao);
    glDrawElements(GL_TRIANGLES, badge->n, GL_UNSIGNED_SHORT, (const void *)0);
    if (leds) { glUniform1i(c64_umode, 3); glBindVertexArray(leds->vao); glDrawElements(GL_TRIANGLES, leds->n, GL_UNSIGNED_SHORT, (const void *)0); }
    /* the glass last: blended, no depth writes, first the far side, then the near side */
    if (glass) {
    glUniform1i(c64_umode, 2);
    glDepthMask(0);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);                       /* the glass is premultiplied: colour and alpha of the picture stay consistent */
    glEnable(GL_CULL_FACE); glBindVertexArray(glass->vao);
    glCullFace(GL_FRONT); glDrawElements(GL_TRIANGLES, glass->n, GL_UNSIGNED_SHORT, (const void *)0);
    glCullFace(GL_BACK); glDrawElements(GL_TRIANGLES, glass->n, GL_UNSIGNED_SHORT, (const void *)0);
    glDisable(GL_CULL_FACE); glDepthMask(1);
    }
    glDisable(GL_DEPTH_TEST);                                          /* the 2D renderer draws without depth */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    /* back to the 2D renderer */
    glUseProgram(prog); glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindTexture(GL_TEXTURE_2D, 0);
}
