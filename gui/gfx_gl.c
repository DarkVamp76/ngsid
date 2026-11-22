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

struct gfx_tex { GLuint id; };

static GLuint prog, vao, vbo;
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
    gfx_tex *t = (gfx_tex *)malloc(sizeof *t);
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

void gfx_read_pixels(int w, int h, unsigned char *rgba) {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}
