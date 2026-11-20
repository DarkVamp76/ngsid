/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gfx.h - the small interface between the batched 2D renderer of ngSid (main.c) and the graphics API.
 *
 * Everything the renderer needs from the GPU is here: one shader (SDF shapes, font atlas, a textured picture), one vertex format, a few textures, two blend modes, a
 * viewport, a screenshot read-back. The backends: gfx_gl.c (OpenGL 3.3 core on the desktop, OpenGL ES 3 in the browser and on iOS) and gfx_metal.m (Metal on Apple).
 * One backend is compiled in; there is no run time switch. */
#ifndef GFX_H
#define GFX_H

#include <SDL2/SDL.h>

/* one vertex: position (pixels), uv, colour, and 4 parameters (the mode m selects the look in the shader, p1..p3 are its parameters) */
typedef struct { float x, y, u, v, r, g, b, a, m, p1, p2, p3; } gfx_vtx;

typedef struct gfx_tex gfx_tex;                       /* a texture (opaque) */

Uint32 gfx_window_flags(void);                        /* the SDL window flag of the backend (SDL_WINDOW_OPENGL or SDL_WINDOW_METAL) */
void   gfx_window_attrs(void);                        /* before the window is created: attributes of the context (OpenGL); nothing for Metal */
int    gfx_init(SDL_Window *win, int max_vtx);        /* after the window: context / device, shader, vertex buffer. 0 = ok, else -1 (a message is printed) */
void   gfx_shutdown(void);
void   gfx_drawable_size(SDL_Window *win, int *w, int *h);   /* size of the frame buffer in pixels */

/* a texture with a full mip chain, linear filtering, clamped edges; channels 1 = one 8 bit channel (red), 4 = RGBA8 */
gfx_tex *gfx_tex_create(int w, int h, int channels, const void *px);

/* a frame: clear the whole frame buffer (fw x fh) to r,g,b, then draw only in the part vx,vy,vw,vh (pixels, y counted from the BOTTOM like OpenGL) */
void   gfx_frame_begin(int fw, int fh, int vx, int vy, int vw, int vh, float r, float g, float b);
void   gfx_set_params(float res_w, float res_h, float pxs);  /* size of the view in view units, and pixels per view unit */
void   gfx_set_texture(gfx_tex *t);                   /* the texture of the following draws (the caller flushes its batch first) */
void   gfx_set_additive(int on);                      /* blend mode of the following draws: 0 = alpha, 1 = additive (the caller flushes first) */
void   gfx_draw(const gfx_vtx *v, int n);             /* n vertices = n/3 triangles */
void   gfx_frame_end(SDL_Window *win);                /* show the frame */
void   gfx_read_pixels(int w, int h, unsigned char *rgba);   /* the frame buffer as RGBA, bottom row first (like glReadPixels) */

#endif
