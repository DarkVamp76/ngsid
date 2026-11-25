/* The INFO view: the C64 of the tune in 3D (the one with the MOS 6581R4 or the CSG 8580R5, whichever the tune runs on), swinging slowly, lit from inside by a light that changes
 * its colours with the beat; the facts of the tune in a table. The models are baked by tools/bake_c64.py (c64_data.h), drawn by gfx_c64_render (gfx.h) into a picture that
 * the 2D renderer shows like any texture. GPL-3.0-or-later */
#include "c64_data.h"

typedef struct { const int16_t *pos; const uint16_t *uv; const uint8_t *col; const uint16_t *idx; int nv, ni; } C64Part;
typedef struct { C64Part p[5]; const uint16_t *ktex, *btex; int ktw, kth, btw, bth; const int16_t *ledpts; } C64Model;     /* parts: solid, keys, badge, glass */
#define C64P(T, t, K, k) { t##_##k##_pos, t##_##k##_uv, t##_##k##_col, t##_##k##_idx, T##_##K##_NV, T##_##K##_NI }
#define C64M(T, t) { { C64P(T, t, SOLID, solid), C64P(T, t, KEYS, keys), C64P(T, t, BADGE, badge), C64P(T, t, GLASS, glass), C64P(T, t, LED, led) }, t##_keys_tex, t##_badge_tex, T##_KEYS_TW, T##_KEYS_TH, T##_BADGE_TW, T##_BADGE_TH, t##_ledpts }
static const C64Model C64D[2] = { C64M(C64_6581, c64_6581), C64M(C64_8580, c64_8580) };

typedef struct { gfx_mesh *mesh[5]; gfx_tex *ktex, *btex, *target, *target2; int ready; float pts[24]; } C64Gl;
static const float C64_YSC[2] = { 1.00f, 1.50f };      /* height of the model: 6581 breadbin file 7.2 cm (real 7 to 8 cm), the 8580 C64C file only 3.8 cm (real 5.7 to 7 cm) */
static C64Gl c64gl[2];
#define C64_TW 1100
#define C64_TH 640

static gfx_tex *c64d_tex565(const uint16_t *src, int w, int h) {
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!px) return NULL;
    for (int i = 0; i < w * h; i++) {
        unsigned v = src[i];
        px[4 * i] = (uint8_t)(((v >> 11) & 31) * 255 / 31); px[4 * i + 1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); px[4 * i + 2] = (uint8_t)((v & 31) * 255 / 31); px[4 * i + 3] = 255;
    }
    gfx_tex *t = gfx_tex_create(w, h, 4, px);
    free(px);
    return t;
}

static void c64d_setup(int mi) {
    C64Gl *g = &c64gl[mi]; const C64Model *m = &C64D[mi];
    if (g->ready) return;
    for (int k = 0; k < 5; k++) {
        const C64Part *p = &m->p[k];
        gfx_v3 *v = (gfx_v3 *)malloc(sizeof(gfx_v3) * (size_t)p->nv);
        if (!v) continue;
        for (int i = 0; i < p->nv; i++) {
            v[i].x = (float)p->pos[3 * i] / 20000.0f; v[i].y = (float)p->pos[3 * i + 1] / 20000.0f; v[i].z = (float)p->pos[3 * i + 2] / 20000.0f;
            v[i].u = (float)p->uv[2 * i] / 65535.0f; v[i].v = (float)p->uv[2 * i + 1] / 65535.0f;
            v[i].r = p->col[4 * i]; v[i].g = p->col[4 * i + 1]; v[i].b = p->col[4 * i + 2]; v[i].a = p->col[4 * i + 3];
        }
        g->mesh[k] = gfx_mesh_create(v, p->nv, p->idx, p->ni);
        free(v);
    }
    for (int i = 0; i < 6; i++) { g->pts[4 * i] = (float)m->ledpts[3 * i] / 20000.0f; g->pts[4 * i + 1] = (float)m->ledpts[3 * i + 1] / 20000.0f; g->pts[4 * i + 2] = (float)m->ledpts[3 * i + 2] / 20000.0f; g->pts[4 * i + 3] = 0; }
    g->ktex = c64d_tex565(m->ktex, m->ktw, m->kth);
    g->btex = c64d_tex565(m->btex, m->btw, m->bth);
    g->target = gfx_target_create(C64_TW, C64_TH);
    g->target2 = gfx_target_create(440, 256);                         /* the small one: the picture in the header */
    g->ready = g->mesh[0] && g->mesh[1] && g->mesh[2] && g->mesh[3] && g->mesh[4] && g->ktex && g->btex && g->target && g->target2;
    if (!g->ready) g->ready = -1;                                  /* no 3D here: the view shows the table only */
}

/* model -> view (column major): turn about the vertical axis, tilt about the horizontal axis, move away from the camera; a perspective that fits the box */
static void c64d_matrices(float ang, float tilt, float fit, float aspect, float ysc, float *mvp, float *mv) {
    const float D = 7.0f, zn = 2.5f, zf = 12.0f;
    float ca = cosf(ang), sa = sinf(ang), ct = cosf(tilt), st = sinf(tilt);
    float M[16] = { ca, st * sa, -ct * sa, 0,   0, ct * ysc, st * ysc, 0,   sa, -st * ca, ct * ca, 0,   0, -0.12f, -D, 1 };
    for (int i = 0; i < 16; i++) mv[i] = M[i];
    float fx = fit * D, fy = fit * D * aspect;                 /* aspect = width / height of the picture: the same number of pixels per unit across and up */
    float P[16] = { fx, 0, 0, 0,  0, fy, 0, 0,  0, 0, -(zf + zn) / (zf - zn), -1,  0, 0, -2 * zf * zn / (zf - zn), 0 };
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) { float s = 0; for (int k = 0; k < 4; k++) s += P[k * 4 + r] * M[c * 4 + k]; mvp[c * 4 + r] = s; }
}

/* the picture of the 3D C64 in the box x,y,w,h (centred, keeping its shape) */
static void c64d_picture(gfx_tex *t, float x, float y, float w, float h) {
    float asp = (float)C64_TW / (float)C64_TH, bw = w, bh = w / asp;
    if (bh > h) { bh = h; bw = h * asp; }
    float x0 = x + (w - bw) / 2, y0 = y + (h - bh) / 2;
    float vt = gfx_target_v_flipped() ? 1.0f : 0.0f, vb = 1.0f - vt;
    col c = C(1, 1, 1, 1);
    flush();
    gfx_set_texture(t);
    V(x0, y0, 0, vt, c, 7, 0, 0, 0); V(x0 + bw, y0, 1, vt, c, 7, 0, 0, 0); V(x0, y0 + bh, 0, vb, c, 7, 0, 0, 0);
    V(x0 + bw, y0, 1, vt, c, 7, 0, 0, 0); V(x0 + bw, y0 + bh, 1, vb, c, 7, 0, 0, 0); V(x0, y0 + bh, 0, vb, c, 7, 0, 0, 0);
    flush();
    gfx_set_texture(fonttex);
}

static const char *c64d_chip(int model) { return model == 8580 ? "CSG 8580R5" : "MOS 6581R4"; }

/* one row of the table: label, value; returns the next y */
static float c64d_row(float rx, float ty, float rw, float k, const char *label, const char *value, col vc) {
    text(rx, ty + 3 * k, 12 * k, FAINT, label);
    char v[200]; snprintf(v, sizeof v, "%s", value);
    fit_text(v, rw - 108 * k, 16 * k);
    text(rx + 108 * k, ty, 16 * k, vc, v);
    return ty + 28 * k;
}

/* the light of the C64s: the colours move on with every beat, the strength follows the beat and the loudness (once per frame, shared by the header and the Info view) */
static float c64d_t, c64d_hue, c64d_drift, c64d_glow;
static void c64d_light(float dtf) {
    static Uint32 last; static float prevbeat, lg;
    Uint32 now = SDL_GetTicks(); if (now == last) return; last = now;
    float dt = dtf > 0.1f ? 0.1f : dtf, beat = g_paused ? 0.0f : g_beat;
    if (!g_paused) c64d_t += dt;
    if (beat > 0.95f && prevbeat <= 0.95f) c64d_hue += 0.13f + 0.05f * sinf(c64d_t * 1.7f);
    prevbeat = beat;
    c64d_drift += dt * (g_paused ? 0.01f : 0.05f);
    float lvl = (g_vis.peak[0] + g_vis.peak[1]) * 0.5f; if (lvl > 1) lvl = 1;
    lg += ((g_paused ? 0.12f : 0.18f + 0.55f * lvl) - lg) * 0.25f;
    c64d_glow = lg + 0.55f * beat;
}
/* the small turning C64 in the header (in the place of the flat picture): 1 = drawn, 0 = no 3D here (the caller draws the flat picture) */
static int c64d_header(int mi, float cx, float cy, float w, float h, float dtf) {
    C64Gl *g = &c64gl[mi];
    c64d_setup(mi);
    if (g->ready <= 0) return 0;
    c64d_light(dtf);
    float mvp[16], mv[16], led[4] = { c64d_hue + c64d_drift, 0.30f, 0.88f, c64d_glow };
    c64d_matrices(c64d_t * 0.9f, 0.50f, 0.80f, 440.0f / 256.0f, C64_YSC[mi], mvp, mv);
    flush();
    gfx_c64_render(g->target2, mvp, mv, g->mesh[0], g->mesh[1], g->ktex, g->mesh[2], g->btex, g->mesh[3], g->mesh[4], led, g->pts);
    float vt = gfx_target_v_flipped() ? 1.0f : 0.0f, vb = 1.0f - vt, bh = h, bw = h * 440.0f / 256.0f;
    col c = C(1, 1, 1, 1);
    gfx_set_texture(g->target2);
    V(cx - bw / 2, cy - bh / 2, 0, vt, c, 7, 0, 0, 0); V(cx + bw / 2, cy - bh / 2, 1, vt, c, 7, 0, 0, 0); V(cx - bw / 2, cy + bh / 2, 0, vb, c, 7, 0, 0, 0);
    V(cx + bw / 2, cy - bh / 2, 1, vt, c, 7, 0, 0, 0); V(cx + bw / 2, cy + bh / 2, 1, vb, c, 7, 0, 0, 0); V(cx - bw / 2, cy + bh / 2, 0, vb, c, 7, 0, 0, 0);
    flush();
    gfx_set_texture(fonttex);
    return 1;
}

static void draw_info(float x, float y, float w, float h, float dtf) {
    const tune_t *tn = g_tune;
    card(x, y, w, h, 0);
    text(x + 18, y + 10, 12, DIM, "INFO");
    if (!tn) { text(x + 24, y + h / 2 - 8, 16, DIM, "no tune"); return; }
    int mi = tn->model[0] == 8580 ? 1 : 0;
    { const char *tm = getenv("NGSID_TEST_C64MODEL"); if (tm) mi = atoi(tm) ? 1 : 0; }      /* test hook */
    c64d_light(dtf);
    float t = c64d_t, hue = c64d_hue, drift = c64d_drift, glow = c64d_glow;

    /* the picture on the left */
    float bw = w * 0.5f; if (bw > 640) bw = 640;
    float px = x + 8, py = y + 26, pw = bw, ph = h - 34;
    C64Gl *g = &c64gl[mi];
    c64d_setup(mi);
    if (g->ready > 0) {
        float ang = t * 0.6f, tilt = 0.62f + 0.05f * sinf(t * 0.37f), mvp[16], mv[16];                  /* turns all the way round */
        int noglass = 0;
        { const char *tc = getenv("NGSID_TEST_C64"); int ta, tt, tg; if (tc && sscanf(tc, "%d:%d:%d", &ta, &tt, &tg) == 3) { ang = (float)ta / 1000.0f; tilt = (float)tt / 1000.0f; noglass = tg; } }      /* test hook: angle:tilt (thousandths of a radian):no glass (whole numbers: a German locale does not read 0.5) */
        c64d_matrices(ang, tilt, 0.68f, (float)C64_TW / (float)C64_TH, C64_YSC[mi], mvp, mv);
        float led[4] = { hue + drift, 0.30f, 0.88f, glow };
        flush();
        gfx_c64_render(g->target, mvp, mv, g->mesh[0], g->mesh[1], g->ktex, g->mesh[2], g->btex, noglass ? NULL : g->mesh[3], g->mesh[4], led, g->pts);
        c64d_picture(g->target, px, py, pw, ph);
    }

    /* the table on the right */
    float rx = x + bw + 14, rw = w - bw - 14 - 24;
    char b[260], b2[80];
    int rows = 9 + (tn->info.sid2 ? 1 : 0) + (cur >= 0 && cur < npl && pl[cur].rel ? 1 : 0);
    float k = (h - 52) / ((float)rows * 28.0f + 60.0f); if (k > 1.25f) k = 1.25f; if (k < 0.6f) k = 0.6f;
    float need = 40 * k + 12 * k + rows * 28 * k;
    float ty = y + (h - need) / 2; if (ty < y + 26) ty = y + 26;
    snprintf(b, sizeof b, "%s", tn->info.name[0] ? tn->info.name : (cur >= 0 ? pl[cur].label : ""));
    fit_text(b, rw, 26 * k);
    text(rx, ty, 26 * k, TXT, b);
    ty += 40 * k; rect(rx, ty, rw, 1, alpha(EDGE, 0.5f)); ty += 12 * k;
    ty = c64d_row(rx, ty, rw, k, "AUTHOR", tn->info.author[0] ? tn->info.author : "-", TXT);
    ty = c64d_row(rx, ty, rw, k, "RELEASED", tn->info.released[0] ? tn->info.released : "-", TXT);
    snprintf(b, sizeof b, "%s, version %d", tn->info.rsid ? "RSID" : "PSID", tn->info.version);
    ty = c64d_row(rx, ty, rw, k, "FORMAT", b, TXT);
    static const char *const clk[4] = { "not given", "PAL", "NTSC", "PAL or NTSC" };
    snprintf(b, sizeof b, "%s  (tune: %s)", tn->pal ? "PAL" : "NTSC", clk[tn->info.clock & 3]);
    ty = c64d_row(rx, ty, rw, k, "CLOCK", b, TXT);
    b[0] = 0;
    for (int i = 0; i < tn->nsid && i < 3; i++) { snprintf(b2, sizeof b2, "%s%s", i ? " + " : "", c64d_chip(tn->model[i])); strncat(b, b2, sizeof b - strlen(b) - 1); }
    ty = c64d_row(rx, ty, rw, k, "SID CHIP", b[0] ? b : c64d_chip(6581), VC[2]);
    snprintf(b, sizeof b, "%d  (start %d, playing %d)", tn->info.songs, tn->info.start_song, tn->song);
    ty = c64d_row(rx, ty, rw, k, "SONGS", b, TXT);
    snprintf(b, sizeof b, "load $%04X   init $%04X   play $%04X", tn->info.load, tn->info.init, tn->info.play);
    ty = c64d_row(rx, ty, rw, k, "ADDRESSES", b, TXT);
    if (tn->info.sid2) {
        snprintf(b, sizeof b, "SID 2 $%04X%s", tn->info.sid2, tn->info.sid3 ? "" : "");
        if (tn->info.sid3) { snprintf(b2, sizeof b2, "   SID 3 $%04X", tn->info.sid3); strncat(b, b2, sizeof b - strlen(b) - 1); }
        ty = c64d_row(rx, ty, rw, k, "MORE SIDS", b, TXT);
    }
    if (cur >= 0 && cur < npl && pl[cur].rel) ty = c64d_row(rx, ty, rw, k, "HVSC", pl[cur].rel, DIM);
    snprintf(b, sizeof b, "%zu bytes", tn->len);
    ty = c64d_row(rx, ty, rw, k, "SIZE", b, DIM);
}
