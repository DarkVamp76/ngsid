/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gfx_metal.m - Metal backend of gfx.h (Apple: iOS now, macOS later). The shader is the one of gfx_gl.c in the Metal shading language (compiled at run time from
 * the source below, so no Metal toolchain is needed to build the app). The window is created with SDL_WINDOW_METAL; SDL gives the CAMetalLayer. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "gfx.h"
#include <SDL2/SDL_metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *const MSL =
    @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; float4 par [[attribute(3)]]; };\n"
    "struct VOut { float4 position [[position]]; float2 uv; float4 col; float4 par; };\n"
    "struct Params { float2 res; float pxs; float pad; };\n"
    "vertex VOut vs_main(VIn in [[stage_in]], constant Params &p [[buffer(1)]]) {\n"
    "  VOut o; o.uv = in.uv; o.col = in.col; o.par = in.par;\n"
    "  o.position = float4(in.pos.x / p.res.x * 2.0 - 1.0, 1.0 - in.pos.y / p.res.y * 2.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "float sdbox(float2 p, float2 h, float r) { float2 q = abs(p) - h + float2(r); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "fragment float4 fs_main(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], constant Params &p [[buffer(1)]]) {\n"
    "  int m = int(in.par.x + 0.5);\n"
    "  float4 o;\n"
    "  if (m == 0) { o = in.col; }\n"
    "  else if (m == 1) { float a = tex.sample(smp, in.uv).r; o = float4(in.col.rgb, in.col.a * a); }\n"
    "  else if (m == 2) { float d = sdbox(in.uv, in.par.yz, in.par.w); o = float4(in.col.rgb, in.col.a * clamp(0.5 - d * p.pxs, 0.0, 1.0)); }\n"
    "  else if (m == 3) { float t = abs(in.uv.y); o = float4(in.col.rgb, in.col.a * exp(-t * t * in.par.y)); }\n"
    "  else if (m == 7) { float4 t = tex.sample(smp, in.uv); o = float4(t.rgb / max(t.a, 0.001), t.a * in.col.a); }\n"
    "  else if (m == 5) { float d = sdbox(in.uv, in.par.yz, in.par.w); float a = clamp(1.0 - d / in.par.w, 0.0, 1.0); o = float4(in.col.rgb, in.col.a * a * a); }\n"
    "  else { o = float4(in.col.rgb, in.col.a * exp(-dot(in.uv, in.uv) / (2.0 * in.par.y * in.par.y))); }\n"
    "  return o;\n"
    "}\n";

struct gfx_tex { id<MTLTexture> tex; };

#define INFLIGHT 3
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static id<MTLRenderPipelineState> pso[2];            /* 0 = alpha blend, 1 = additive */
static id<MTLSamplerState> sampler;
static id<MTLBuffer> vbuf[INFLIGHT];
static size_t vcap, voff;                            /* capacity of one frame's buffer, bytes used so far */
static dispatch_semaphore_t inflight;
static int slot;
static id<MTLCommandBuffer> cmd;
static id<MTLRenderCommandEncoder> enc;
static id<CAMetalDrawable> drawable;
static gfx_tex *cur_tex;
static int additive, frame_ok, committed;
static struct { float res[2]; float pxs; float pad; } params;
static int fb_w, fb_h;

Uint32 gfx_window_flags(void) { return SDL_WINDOW_METAL; }
void gfx_window_attrs(void) {}

static id<MTLRenderPipelineState> make_pso(id<MTLLibrary> lib, MTLVertexDescriptor *vd, int add) {
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
    d.vertexDescriptor = vd;
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = layer.pixelFormat;
    c.blendingEnabled = YES;                                           /* glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA or GL_ONE): the same for colour and alpha */
    c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = add ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) fprintf(stderr, "Metal pipeline: %s\n", err.localizedDescription.UTF8String);
    return p;
}

int gfx_init(SDL_Window *win, int max_vtx) {
    @autoreleasepool {
        SDL_MetalView view = SDL_Metal_CreateView(win);
        if (!view) { fprintf(stderr, "Metal view: %s\n", SDL_GetError()); return -1; }
        layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
        dev = MTLCreateSystemDefaultDevice();
        if (!layer || !dev) { fprintf(stderr, "Metal: no device\n"); return -1; }
        layer.device = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;                                    /* the screenshot reads the frame back */
        queue = [dev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:MSL options:nil error:&err];
        if (!lib) { fprintf(stderr, "Metal shader: %s\n", err.localizedDescription.UTF8String); return -1; }
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8;  vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat4; vd.attributes[2].offset = 16; vd.attributes[2].bufferIndex = 0;
        vd.attributes[3].format = MTLVertexFormatFloat4; vd.attributes[3].offset = 32; vd.attributes[3].bufferIndex = 0;
        vd.layouts[0].stride = sizeof(gfx_vtx); vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        pso[0] = make_pso(lib, vd, 0); pso[1] = make_pso(lib, vd, 1);
        if (!pso[0] || !pso[1]) return -1;
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler = [dev newSamplerStateWithDescriptor:sd];
        vcap = (size_t)max_vtx * sizeof(gfx_vtx);                       /* one frame's vertices: the batches of a frame lie behind each other */
        for (int i = 0; i < INFLIGHT; i++) vbuf[i] = [dev newBufferWithLength:vcap options:MTLResourceStorageModeShared];
        inflight = dispatch_semaphore_create(INFLIGHT);
    }
    return 0;
}

void gfx_shutdown(void) { enc = nil; cmd = nil; drawable = nil; }

#if TARGET_OS_TV
#include <SDL2/SDL_syswm.h>
static float render_scale = 1.0f;
void gfx_set_render_scale(float sc) { render_scale = sc < 0.25f ? 0.25f : (sc > 1.0f ? 1.0f : sc); }
int gfx_refresh_hz(SDL_Window *win) {
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_UIKIT && info.info.uikit.window) { NSInteger r = info.info.uikit.window.screen.maximumFramesPerSecond; if (r >= 24 && r <= 240) return (int)r; }
    return 60;
}
/* Apple TV: the window is 1920 x 1080 points whatever the output; at 4K the screen scale is 2. SDL reports the size of the layer, which this program sets itself every frame,
 * so the native size is computed from the window and the screen. */
void gfx_native_size(SDL_Window *win, int *w, int *h) {
    int pw = 0, ph = 0; SDL_GetWindowSize(win, &pw, &ph);
    float ns = 1;
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_UIKIT && info.info.uikit.window) ns = (float)info.info.uikit.window.screen.nativeScale;
    if (ns < 1) ns = 1;
    *w = (int)((float)pw * ns + 0.5f); *h = (int)((float)ph * ns + 0.5f);
}
void gfx_drawable_size(SDL_Window *win, int *w, int *h) {
    gfx_native_size(win, w, h);
    *w = ((int)((float)*w * render_scale + 0.5f)) & ~1; *h = ((int)((float)*h * render_scale + 0.5f)) & ~1;     /* even sizes */
}
#else
void gfx_drawable_size(SDL_Window *win, int *w, int *h) { SDL_Metal_GetDrawableSize(win, w, h); }
void gfx_native_size(SDL_Window *win, int *w, int *h) { SDL_Metal_GetDrawableSize(win, w, h); }
void gfx_set_render_scale(float sc) { (void)sc; }
#endif

gfx_tex *gfx_tex_create(int w, int h, int channels, const void *px) {
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(channels == 1 ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm) width:(NSUInteger)w height:(NSUInteger)h mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        if (!t) return NULL;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w * (NSUInteger)channels];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be generateMipmapsForTexture:t];
        [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        gfx_tex *g = (gfx_tex *)malloc(sizeof *g);
        if (!g) return NULL;
        g->tex = t;
        return g;
    }
}

void gfx_frame_begin(int fw, int fh, int vx, int vy, int vw, int vh, float r, float g, float b) {
    dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
    slot = (slot + 1) % INFLIGHT; voff = 0; committed = 0; frame_ok = 0; additive = 0;
    fb_w = fw; fb_h = fh;
    @autoreleasepool {
        layer.drawableSize = CGSizeMake(fw, fh);
        drawable = [layer nextDrawable];
        if (!drawable) { dispatch_semaphore_signal(inflight); return; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1);
        cmd = [queue commandBuffer];
        enc = [cmd renderCommandEncoderWithDescriptor:rp];
        MTLViewport vp = { (double)vx, (double)(fh - vy - vh), (double)vw, (double)vh, 0, 1 };   /* Metal counts y from the top, gfx.h from the bottom */
        [enc setViewport:vp];
        [enc setRenderPipelineState:pso[0]];
        [enc setFragmentSamplerState:sampler atIndex:0];
        frame_ok = 1;
    }
}

void gfx_set_params(float res_w, float res_h, float pxs) { params.res[0] = res_w; params.res[1] = res_h; params.pxs = pxs; }
void gfx_set_texture(gfx_tex *t) { cur_tex = t; }
void gfx_set_additive(int on) { additive = on; }

void gfx_draw(const gfx_vtx *v, int n) {
    if (!frame_ok || n <= 0) return;
    size_t bytes = (size_t)n * sizeof(gfx_vtx);
    if (voff + bytes > vcap) return;                                   /* more than a frame can hold: dropped (the buffer is sized for that) */
    memcpy((char *)vbuf[slot].contents + voff, v, bytes);
    [enc setRenderPipelineState:pso[additive ? 1 : 0]];
    [enc setVertexBuffer:vbuf[slot] offset:voff atIndex:0];
    [enc setVertexBytes:&params length:sizeof params atIndex:1];
    [enc setFragmentBytes:&params length:sizeof params atIndex:1];
    if (cur_tex) [enc setFragmentTexture:cur_tex->tex atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)n];
    voff += bytes;
}

static volatile float last_gpu_ms;
float gfx_last_gpu_ms(void) { return last_gpu_ms; }

static void commit_frame(void) {
    if (committed) return;
    committed = 1;
    [enc endEncoding];
    [cmd presentDrawable:drawable];
    dispatch_semaphore_t s = inflight;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> c) { last_gpu_ms = (float)((c.GPUEndTime - c.GPUStartTime) * 1000.0); dispatch_semaphore_signal(s); }];
    [cmd commit];
}

void gfx_frame_end(SDL_Window *win) {
    (void)win;
    if (!frame_ok) return;
    commit_frame();
    enc = nil; cmd = nil; drawable = nil; frame_ok = 0;
}

void gfx_read_pixels(int w, int h, unsigned char *rgba) {
    if (!frame_ok) { memset(rgba, 0, (size_t)w * (size_t)h * 4); return; }
    id<MTLTexture> tex = drawable.texture;
    id<MTLCommandBuffer> waitcmd = cmd;
    commit_frame();                                                    /* the frame is complete here: finish it, wait, read the frame buffer back */
    [waitcmd waitUntilCompleted];
    size_t row = (size_t)w * 4;
    unsigned char *bgra = (unsigned char *)malloc(row * (size_t)h);
    if (!bgra) return;
    [tex getBytes:bgra bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0];
    for (int y = 0; y < h; y++) {                                      /* BGRA, top row first -> RGBA, bottom row first (like glReadPixels) */
        const unsigned char *src = bgra + (size_t)(h - 1 - y) * row;
        unsigned char *dst = rgba + (size_t)y * row;
        for (int x = 0; x < w; x++) { dst[x * 4 + 0] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4 + 0]; dst[x * 4 + 3] = src[x * 4 + 3]; }
    }
    free(bgra);
}
