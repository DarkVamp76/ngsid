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
    "}\n"
    /* ---------------------------------------------------------------- the 3D C64 of the Info view (the shader of gfx_gl.c in Metal) */
    "struct C6In { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };\n"
    "struct C6Out { float4 position [[position]]; float3 vv; float3 vp; float2 vuv; float4 vcol; };\n"
    "struct C6P { float4x4 mvp; float4x4 mv; float4 uled; float4 pts[6]; int umode; int pad[3]; };\n"
    "vertex C6Out vs_c64(C6In in [[stage_in]], constant C6P &p [[buffer(1)]]) {\n"
    "  C6Out o; float4 q = float4(in.pos, 1.0); o.vv = (p.mv * q).xyz; o.vp = in.pos; o.vuv = in.uv; o.vcol = in.col;\n"
    "  float4 gp = p.mvp * q; o.position = float4(gp.x, gp.y, (gp.z + gp.w) * 0.5, gp.w);\n"      /* depth 0..1; rows of a Metal texture run from the top, which is where +y of the clip space is */
    "  return o;\n"
    "}\n"
    "float3 hsv3(float h, float s, float v) { float3 k = clamp(abs(fract(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0); return v * mix(float3(1.0), k, s); }\n"
    "float hash2(float2 c) { return fract(sin(dot(c, float2(12.9898, 78.233))) * 43758.5453); }\n"
    "float3 glow3(float3 q, constant C6P &p) { float3 g = float3(0.0); for (int i = 0; i < 6; i++) { float d = distance(q, p.pts[i].xyz); float a = 1.0 / (1.0 + d * d * 45.0); g += hsv3(p.uled.x + 0.17 * float(i) + 0.05, 0.85, 1.0) * a * a; } return g * p.uled.w; }\n"
    "fragment float4 fs_c64(C6Out in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], constant C6P &p [[buffer(1)]]) {\n"
    "  float3 n = normalize(cross(dfdx(in.vv), dfdy(in.vv))); if (dot(n, -in.vv) < 0.0) n = -n;\n"
    "  float d = max(dot(n, normalize(float3(0.33, 0.52, 0.79))), 0.0);\n"
    "  float sp = 0.65 + 0.35 * sin(in.vp.x * 38.0 + p.uled.x * 40.0) * sin(in.vp.z * 53.0 - p.uled.x * 25.0);\n"
    "  float3 L = hsv3(p.uled.x + in.vp.x * p.uled.y + in.vp.z * 0.18, p.uled.z, 1.0) * p.uled.w;\n"
    "  float3 G = glow3(in.vp, p);\n"
    "  int m = p.umode;\n"
    "  if (m == 2) {\n"
    "    float rim = pow(1.0 - abs(dot(n, normalize(-in.vv))), 3.0);\n"
    "    float3 rc = hsv3(p.uled.x + in.vp.x * p.uled.y * 1.5 + in.vp.z * 0.3 + 0.1, 1.0, 1.0);\n"
    "    float3 c = float3(0.86, 0.89, 0.98) * (0.40 + 0.45 * d) + L * 0.30 + rc * rim * p.uled.w * 2.4 + G * 1.4;\n"
    "    float a = clamp(in.vcol.a * 1.05 + 0.45 * rim + length(G) * 0.25, 0.0, 0.95);\n"
    "    return float4(c * a, a);\n"
    "  } else if (m == 3) {\n"
    "    float3 lc = hsv3(p.uled.x + in.vp.x * p.uled.y * 2.0 + in.vp.z * 0.5, 0.8, 1.0);\n"
    "    return float4(mix(in.vcol.rgb, lc, 0.8) * (1.3 + 2.2 * p.uled.w) + float3(0.35), 1.0);\n"
    "  } else if (m == 1) {\n"
    "    bool red = in.vcol.r > 0.5;\n"
    "    float h = hash2(floor(in.vuv * float2(16.0, 4.0)));\n"
    "    float3 kc = hsv3(p.uled.x + 0.55 * h + in.vp.x * 0.2, 0.85, 1.0);\n"
    "    float3 base = in.vcol.rgb * (0.35 + 0.9 * d);\n"
    "    return float4(base + (red ? float3(0.0) : kc * p.uled.w * (0.60 + 0.40 * sp) * (0.50 + 0.50 * d)) + G * 0.5, 1.0);\n"
    "  }\n"
    "  float3 base = m == 4 ? tex.sample(smp, in.vuv).rgb : in.vcol.rgb;\n"
    "  float sh = m == 4 ? 0.62 + 0.55 * d : 0.30 + 0.80 * d;\n"
    "  return float4(base * sh * (m == 4 ? 1.0 : 0.75) + L * (m == 4 ? 0.30 : 0.18 * sp) + G * 0.7, 1.0);\n"
    "}\n";

struct gfx_tex { id<MTLTexture> tex; id<MTLTexture> depth; int w, h; };
struct gfx_mesh { id<MTLBuffer> vb, ib; int n; };

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
static int vp_x, vp_y, vp_w, vp_h; static float clear_rgb[3];               /* the window pass: its viewport and clear colour (a 3D pass in the middle of a frame ends it and starts it again with Load) */
static id<MTLRenderPipelineState> pso_c64[2];                              /* 0 = opaque parts, 1 = the glass (premultiplied) */
static id<MTLDepthStencilState> dss_write, dss_nowrite;

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
        {   /* the 3D C64: a vertex of gfx_v3 (position, uv, colour), an RGBA target with a depth buffer */
            MTLVertexDescriptor *v3 = [MTLVertexDescriptor vertexDescriptor];
            v3.attributes[0].format = MTLVertexFormatFloat3; v3.attributes[0].offset = 0;  v3.attributes[0].bufferIndex = 0;
            v3.attributes[1].format = MTLVertexFormatFloat2; v3.attributes[1].offset = 12; v3.attributes[1].bufferIndex = 0;
            v3.attributes[2].format = MTLVertexFormatUChar4Normalized; v3.attributes[2].offset = 20; v3.attributes[2].bufferIndex = 0;
            v3.layouts[0].stride = 24; v3.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
            for (int g = 0; g < 2; g++) {
                MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
                d.vertexFunction = [lib newFunctionWithName:@"vs_c64"]; d.fragmentFunction = [lib newFunctionWithName:@"fs_c64"];
                d.vertexDescriptor = v3; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
                c.pixelFormat = MTLPixelFormatRGBA8Unorm; c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = g ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;      /* the glass is premultiplied (like gfx_gl.c) */
                c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                NSError *e3 = nil; pso_c64[g] = [dev newRenderPipelineStateWithDescriptor:d error:&e3];
                if (!pso_c64[g]) fprintf(stderr, "Metal C64 pipeline: %s\n", e3.localizedDescription.UTF8String);
            }
            MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
            dd.depthCompareFunction = MTLCompareFunctionLess; dd.depthWriteEnabled = YES; dss_write = [dev newDepthStencilStateWithDescriptor:dd];
            dd.depthWriteEnabled = NO; dss_nowrite = [dev newDepthStencilStateWithDescriptor:dd];
        }
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
        gfx_tex *g = (gfx_tex *)calloc(1, sizeof *g);
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
        vp_x = vx; vp_y = vy; vp_w = vw; vp_h = vh; clear_rgb[0] = r; clear_rgb[1] = g; clear_rgb[2] = b;
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

/* ---------------------------------------------------------------- the 3D C64 */
gfx_mesh *gfx_mesh_create(const gfx_v3 *verts, int nverts, const uint16_t *idx, int nidx) {
    gfx_mesh *m = (gfx_mesh *)calloc(1, sizeof *m);
    if (!m) return NULL;
    if (nverts <= 0 || nidx <= 0) return m;
    m->vb = [dev newBufferWithBytes:verts length:(NSUInteger)(sizeof(gfx_v3) * (size_t)nverts) options:MTLResourceStorageModeShared];
    m->ib = [dev newBufferWithBytes:idx length:(NSUInteger)(sizeof(uint16_t) * (size_t)nidx) options:MTLResourceStorageModeShared];
    if (m->vb && m->ib) m->n = nidx; else { m->vb = nil; m->ib = nil; }
    return m;
}
int gfx_target_v_flipped(void) { return 0; }
gfx_tex *gfx_target_create(int w, int h) {
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
        gfx_tex *t = (gfx_tex *)calloc(1, sizeof *t);
        if (!t) return NULL;
        t->tex = [dev newTextureWithDescriptor:td]; t->depth = [dev newTextureWithDescriptor:dd]; t->w = w; t->h = h;
        if (!t->tex || !t->depth) { t->tex = nil; t->depth = nil; return NULL; }
        return t;
    }
}
/* a window pass again after the off-screen pass in the middle of the frame (Load: what was drawn so far stays) */
static void window_pass_resume(void) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    MTLViewport v = { (double)vp_x, (double)(fb_h - vp_y - vp_h), (double)vp_w, (double)vp_h, 0, 1 };
    [enc setViewport:v];
    [enc setRenderPipelineState:pso[additive ? 1 : 0]];
    [enc setFragmentSamplerState:sampler atIndex:0];
}
void gfx_c64_render(gfx_tex *target, const float *mvp, const float *mv, gfx_mesh *solid, gfx_mesh *keys, gfx_tex *keys_tex, gfx_mesh *badge, gfx_tex *badge_tex, gfx_mesh *glass, gfx_mesh *leds, const float *led, const float *pts) {
    (void)keys_tex;
    if (!frame_ok || !target || !target->tex || !pso_c64[0] || !pso_c64[1]) return;
    struct { float mvp[16], mv[16], uled[4], pts[24]; int umode, pad[3]; } p;
    memcpy(p.mvp, mvp, sizeof p.mvp); memcpy(p.mv, mv, sizeof p.mv); memcpy(p.uled, led, sizeof p.uled); memcpy(p.pts, pts, sizeof p.pts); memset(p.pad, 0, sizeof p.pad);
    [enc endEncoding]; enc = nil;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target->tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.depthAttachment.texture = target->depth; rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare; rp.depthAttachment.clearDepth = 1.0;
    id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
    MTLViewport v = { 0, 0, (double)target->w, (double)target->h, 0, 1 };
    [e setViewport:v];
    [e setFrontFacingWinding:MTLWindingCounterClockwise];                /* like OpenGL: the glass is drawn far side first (cull front), then near side (cull back) */
    [e setFragmentSamplerState:sampler atIndex:0];
    [e setRenderPipelineState:pso_c64[0]]; [e setDepthStencilState:dss_write]; [e setCullMode:MTLCullModeNone];
    gfx_mesh *om[4] = { solid, keys, badge, leds }; const int mode[4] = { 0, 1, 4, 3 };
    for (int i = 0; i < 4; i++) {
        gfx_mesh *m = om[i];
        if (!m || m->n <= 0 || !m->ib || !m->vb) continue;                 /* an indexed draw without an index buffer crashes the GPU driver */
        p.umode = mode[i];
        [e setVertexBuffer:m->vb offset:0 atIndex:0]; [e setVertexBytes:&p length:sizeof p atIndex:1]; [e setFragmentBytes:&p length:sizeof p atIndex:1];
        [e setFragmentTexture:(i == 2 && badge_tex ? badge_tex->tex : target->tex) atIndex:0];
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)m->n indexType:MTLIndexTypeUInt16 indexBuffer:m->ib indexBufferOffset:0];
    }
    if (glass && glass->n > 0 && glass->ib && glass->vb) {
        p.umode = 2;
        [e setRenderPipelineState:pso_c64[1]]; [e setDepthStencilState:dss_nowrite];
        [e setVertexBuffer:glass->vb offset:0 atIndex:0]; [e setVertexBytes:&p length:sizeof p atIndex:1]; [e setFragmentBytes:&p length:sizeof p atIndex:1];
        [e setFragmentTexture:target->tex atIndex:0];
        for (int pass = 0; pass < 2; pass++) {
            [e setCullMode:pass == 0 ? MTLCullModeFront : MTLCullModeBack];
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)glass->n indexType:MTLIndexTypeUInt16 indexBuffer:glass->ib indexBufferOffset:0];
        }
    }
    [e endEncoding];
    window_pass_resume();
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
