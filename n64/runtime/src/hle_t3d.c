/*
 * A host model of tiny3d's RSP microcode (rsp_tiny3d), behind the rspq tap (hle_rspq.c).
 * See docs/research/tiny3d-hle.md, section 3.3.
 *
 * tiny3d's C code stays recompiled; it encodes commands for its microcode, and this file does what
 * the microcode would: a matrix stack (in host memory, indexed like the one tiny3d keeps in RDRAM),
 * the projection, per-vertex transform and lighting on the CPU into a vertex cache addressed by its
 * DMEM offsets (as tiny3d encodes them), culling, and triangle assembly. Triangles are handed to
 * hle_rdpq.c in clip space and drawn with the current RDP render mode (combiner, blender, depth).
 *
 * The microcode's DMEM addresses come from tiny3d's generated rsp_tiny3d.h (built with libt3d.a);
 * without it the commands are only counted.
 *
 * Not modelled yet: vertex effects, depth offset, alpha-to-tile, IMEM patches.
 * tinypx, tiny3d's particle microcode, is modelled at the end of this file.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "hle_internal.h"

#if __has_include("rsp_tiny3d.h")
#include "rsp_tiny3d.h"
#define T3D_HAVE 1
#define DMEM(sym) ((uint32_t)(sym) & 0xFFFu)
#else
#define T3D_HAVE 0
#endif

#define VERT_OUTPUT_SIZE 36
#define CACHE 128
#define STACK 64

typedef struct {
    float clip[4];
    uint8_t c[4];
    float s, t;       /* texels */
} CVert;

static struct {
    float proj[16];   /* column-major: m[col * 4 + row] */
    float stack[STACK][16];
    int sp;           /* the stack pointer in bytes, relative to where tiny3d's started */
    float mv[16], mvp[16], nrm[9];
    CVert v[CACHE];
    uint8_t ambient[4];  /* RGBA: the alpha is lit too (tiny3d's fresnel puts a light in it) */
    struct {
        uint8_t c[4];
        float dir[3];       /* directional: towards the light, eye space */
        bool point;         /* point light: position (eye space) and size */
        bool no_normals;    /* point light lit by distance only */
        float pos[3];
        int size;           /* the size word without its flag (8 x world size, at most 0x7FFF) */
    } light[8];
    int nlights;
    float exposure;
    int cull;         /* 0 cull back, 1 cull front, 2 none */
    bool unlit;
    bool fog;         /* FOG_STATE: the vertex alpha holds fog */
    uint16_t fog_slope_i, fog_slope_f;  /* t3d_fog_set_range: per clip-space z, s16.16 */
    int16_t fog_off;
    uint32_t seg[16];
    int vx, vy, vw, vh;  /* the viewport, N64 pixels */
    GfxVert3D *tri;      /* pending triangles */
    int ntri, cap;
    bool logged;
} T = {.exposure = 1.0f, .cull = 2, .fog_off = (int16_t)0x80FF, .ambient = {255, 255, 255, 255}};

/* N64_T3D_TRACE=<n>: log the first n matrix, vertex-load, strip and triangle events (on the Mac:
 * where do a model's vertices land, which matrices did it get, which cache slots do triangles use). */
static int g_trace = -1;
static bool tracing(void) {
    if (g_trace < 0) g_trace = SDL_getenv("N64_T3D_TRACE") ? SDL_atoi(SDL_getenv("N64_T3D_TRACE")) : 0;
    return g_trace > 0 && g_trace--;
}

/* ---- maths ---------------------------------------------------------------------------------------- */
static void mat_mul(float *out, const float *a, const float *b) {  /* out = a * b */
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] + a[2 * 4 + row] * b[c * 4 + 2] +
                             a[3 * 4 + row] * b[c * 4 + 3];
    SDL_memcpy(out, r, sizeof r);
}

/* As the ucode's resolveSegmentAddr: `addr >> 24` is a byte offset into the segment table (a
 * placeholder is id << 26), its entry is added, and the DMA sees the low 24 bits. */
static uint32_t resolve(uint32_t addr) {
    const uint32_t seg = (addr >> 26) & 7;
    if (seg) addr = (T.seg[seg] + (addr & 0xFFFFFF)) & 0xFFFFFF;
    return (addr & 0x1FFFFFFFu) | 0x80000000u;
}

/* T3DMat4FP: per column, four s16 integer parts then four u16 fractions (16.16). */
static void load_mat(uint8_t *rdram, uint32_t addr, float *m) {
    addr = resolve(addr);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            const uint32_t i = n64_r16(rdram, addr + (uint32_t)(c * 16 + r * 2));
            const uint32_t f = n64_r16(rdram, addr + (uint32_t)(c * 16 + 8 + r * 2));
            m[c * 4 + r] = (float)(int32_t)(i << 16 | f) / 65536.0f;
        }
}

static void update_mv(const float *m) {
    SDL_memcpy(T.mv, m, sizeof T.mv);
    mat_mul(T.mvp, T.proj, T.mv);
    /* the normal matrix: the model-view's columns, normalised (uniform scale assumed, as tiny3d does) */
    for (int c = 0; c < 3; c++) {
        const float x = m[c * 4], y = m[c * 4 + 1], z = m[c * 4 + 2], l = sqrtf(x * x + y * y + z * z);
        const float k = l > 1e-9f ? 1.0f / l : 0.0f;
        T.nrm[c * 3] = x * k;
        T.nrm[c * 3 + 1] = y * k;
        T.nrm[c * 3 + 2] = z * k;
    }
}

/* ---- commands ------------------------------------------------------------------------------------- */
static void matrix_stack(uint8_t *rdram, uint32_t w0, uint32_t addr) {
    const int advance = (int16_t)((w0 >> 8) & 0xFFFF);
    T.sp += advance;
    if (w0 & 2) return;  /* only move the stack pointer */
    const int idx = SDL_clamp(T.sp / 64, 0, STACK - 1);
    float m[16];
    if (advance < 0) SDL_memcpy(m, T.stack[idx], sizeof m);  /* pop: back to what's on the stack */
    else load_mat(rdram, addr, m);
    if (w0 & 1) mat_mul(m, T.stack[SDL_clamp(idx - 1, 0, STACK - 1)], m);
    if (tracing()) {
        float l[16];
        if (advance >= 0) load_mat(rdram, addr, l);
        n64rt_log("t3d mat %08X adv %d sp %d src %08X: diag %.3f %.3f %.3f t %.2f %.2f %.2f -> diag %.3f %.3f %.3f t %.2f %.2f %.2f",
                  w0, advance, T.sp, addr, advance >= 0 ? l[0] : 0, advance >= 0 ? l[5] : 0, advance >= 0 ? l[10] : 0,
                  advance >= 0 ? l[12] : 0, advance >= 0 ? l[13] : 0, advance >= 0 ? l[14] : 0, m[0], m[5], m[10], m[12], m[13], m[14]);
    }
    SDL_memcpy(T.stack[idx], m, sizeof m);
    update_mv(m);
}

static void screen_size(const uint32_t *w) {
    const int guard = SDL_max(1, (int)(w[0] & 0xF));
    const float norm_w = (float)(w[3] & 0xFFFF) / 65535.0f;
    const float sx = (float)(int16_t)(w[2] >> 16) + (float)(w[4] >> 16) / 65536.0f;
    const float sy = (float)(int16_t)(w[2] & 0xFFFF) + (float)(w[4] & 0xFFFF) / 65536.0f;
    const float k = norm_w * 4.0f * (float)guard;
    if (k <= 0) return;
    const int width = (int)lroundf(sx / k), height = (int)lroundf(-sy / k);
    const int ox2 = (int32_t)w[1] >> 17, oy2 = (int16_t)((w[1] >> 1) & 0xFFFF);
    T.vw = width;
    T.vh = height;
    T.vx = (ox2 - width) / 2;
    T.vy = (oy2 - height) / 2;
}

/* A point light's strength at a vertex (eye-space position ev, normal n). The microcode works in eye
 * units / 16: 1/sqrt of the squared distance, times the size word, saturated to s16, squared as a
 * fraction; then the dot with the direction to the light (or not, for "ignore normals"). */
static float light_point(int l, const float *ev, const float *n) {
    const float d[3] = {T.light[l].pos[0] - ev[0], T.light[l].pos[1] - ev[1], T.light[l].pos[2] - ev[2]};
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float raw = dist > 1e-6f ? SDL_min(SDL_truncf(2048.0f * (float)T.light[l].size / dist), 32767.0f) : 32767.0f;
    const float f = (raw / 32768.0f) * (raw / 32768.0f);
    if (T.light[l].no_normals) return f;
    if (dist <= 1e-6f) return 0;
    return f * (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) / dist;
}

static void vert_load(uint8_t *rdram, const uint32_t *w) {
#if T3D_HAVE
    const int count = (int)(w[0] & 0xFFFF) / 16;
    const uint32_t src = resolve(w[1]);
    const int slot0 = (int)((w[2] & 0xFFFF) - DMEM(RSP_T3D_VERT_BUFFER)) / VERT_OUTPUT_SIZE;
    const bool trace = tracing();
    if (trace)
        n64rt_log("t3d vload %08X %08X %08X: %d verts at slot %d; first pos %d %d %d", w[0], w[1], w[2], count, slot0,
                  (int16_t)n64_r16(rdram, src), (int16_t)n64_r16(rdram, src + 2), (int16_t)n64_r16(rdram, src + 4));
    for (int i = 0; i < count; i++) {
        const int slot = slot0 + i;
        if (slot < 0 || slot >= CACHE) continue;
        const uint32_t p = src + (uint32_t)(i / 2) * 32, b = (i & 1) ? 8u : 0u;
        const float x = (float)(int16_t)n64_r16(rdram, p + b), y = (float)(int16_t)n64_r16(rdram, p + b + 2);
        const float z = (float)(int16_t)n64_r16(rdram, p + b + 4);
        const uint16_t nq = n64_r16(rdram, p + b + 6);
        const uint32_t rgba = n64_r32(rdram, p + 16 + ((i & 1) ? 4u : 0u));
        const uint32_t st = n64_r32(rdram, p + 24 + ((i & 1) ? 4u : 0u));
        CVert *o = &T.v[slot];
        for (int r = 0; r < 4; r++) o->clip[r] = T.mvp[r] * x + T.mvp[4 + r] * y + T.mvp[8 + r] * z + T.mvp[12 + r];
        float lc[4] = {1, 1, 1, 1};
        if (!T.unlit) {
            /* 5.6.5 normal, into view space */
            const float nx0 = (float)((int16_t)(nq & 0xF800) >> 11) / 15.5f, ny0 = (float)((int16_t)((nq << 5) & 0xFC00) >> 10) / 31.5f;
            const float nz0 = (float)((int16_t)((nq << 11) & 0xF800) >> 11) / 15.5f;
            float n[3], ev[3];
            bool have_ev = false;
            for (int r = 0; r < 3; r++) n[r] = T.nrm[r] * nx0 + T.nrm[3 + r] * ny0 + T.nrm[6 + r] * nz0;
            for (int ch = 0; ch < 4; ch++) lc[ch] = (float)T.ambient[ch] / 255.0f;
            for (int l = 0; l < T.nlights; l++) {
                float d;
                if (T.light[l].point) {
                    /* as the microcode: full strength within half the size, then (size / 2 dist)^2, times
                     * the angle to the light unless it ignores normals (light_point) */
                    if (!have_ev) {
                        for (int r = 0; r < 3; r++) ev[r] = T.mv[r] * x + T.mv[4 + r] * y + T.mv[8 + r] * z + T.mv[12 + r];
                        have_ev = true;
                    }
                    d = light_point(l, ev, n);
                } else {
                    d = n[0] * T.light[l].dir[0] + n[1] * T.light[l].dir[1] + n[2] * T.light[l].dir[2];
                }
                if (d <= 0) continue;
                for (int ch = 0; ch < 4; ch++) lc[ch] += d * (float)T.light[l].c[ch] / 255.0f;
            }
        }
        for (int ch = 0; ch < 4; ch++) {
            const float v = (float)((rgba >> (24 - ch * 8)) & 255) * SDL_min(lc[ch], 1.0f) * T.exposure;
            o->c[ch] = (uint8_t)SDL_clamp(v + 0.5f, 0.0f, 255.0f);
        }
        if (T.fog) {
            /* as the microcode: offset + slope * (integer clip z), saturated to [0x8000, 0x80FF]; its
             * low byte is the alpha the fog blender mixes by (255 at the near end, 0 past the far end) */
            const float slope = (float)(int16_t)T.fog_slope_i + (float)T.fog_slope_f / 65536.0f;
            const float f = SDL_clamp((float)T.fog_off + slope * SDL_floorf(o->clip[2]), -32768.0f, -32513.0f);
            o->c[3] = (uint8_t)((int)SDL_floorf(f) + 32768);
        }
        o->s = (float)(int16_t)(st >> 16) / 32.0f;
        o->t = (float)(int16_t)(st & 0xFFFF) / 32.0f;
        if (trace && i < 3)
            n64rt_log("t3d   v%d pos %.0f %.0f %.0f -> ndc %.3f %.3f w %.1f", slot, x, y, z, o->clip[0] / o->clip[3], o->clip[1] / o->clip[3], o->clip[3]);
    }
#else
    (void)rdram;
    (void)w;
#endif
}

static int slot_of(uint32_t dmem) {
#if T3D_HAVE
    const int s = (int)((dmem & 0x3FFF) - DMEM(RSP_T3D_VERT_BUFFER)) / VERT_OUTPUT_SIZE;
    return (s >= 0 && s < CACHE) ? s : -1;
#else
    (void)dmem;
    return -1;
#endif
}

static void push_vert(const CVert *c) {
    GfxVert3D *o = &T.tri[T.ntri++];
    o->x = c->clip[0];
    o->y = c->clip[1];
    o->w = c->clip[3];
    o->z = c->clip[2];  /* unclamped: gfx_gl.c clamps the depth per fragment, as the RDP does */
    o->u = c->s;
    o->v = c->t;
    SDL_memcpy(o->c, c->c, 4);
}

/* Near-plane clipping on the CPU (Sutherland-Hodgman against w = NEAR_W): every vertex handed to GL
 * is in front of the eye. The stick's GPU driver doesn't clip triangles with vertices behind the
 * eye (w < 0) the way desktop GL does: Sword Strike's walls, which run past the camera, vanished. */
#define NEAR_W 1e-3f
static CVert lerp_vert(const CVert *p, const CVert *q, float t) {
    CVert o;
    for (int i = 0; i < 4; i++) o.clip[i] = p->clip[i] + (q->clip[i] - p->clip[i]) * t;
    for (int i = 0; i < 4; i++) o.c[i] = (uint8_t)((float)p->c[i] + ((float)q->c[i] - (float)p->c[i]) * t + 0.5f);
    o.s = p->s + (q->s - p->s) * t;
    o.t = p->t + (q->t - p->t) * t;
    return o;
}

static void emit_tri(const CVert *va, const CVert *vb, const CVert *vc) {
    if (T.ntri + 3 > T.cap) {
        T.cap = SDL_max(T.cap * 2, 3072);
        T.tri = (GfxVert3D *)SDL_realloc(T.tri, sizeof(GfxVert3D) * (size_t)T.cap);
    }
    push_vert(va);
    push_vert(vb);
    push_vert(vc);
}

static void tri(int a, int b, int c) {
    if (tracing()) n64rt_log("t3d   tri %d %d %d", a, b, c);
    if (a < 0 || b < 0 || c < 0) return;
    const CVert *va = &T.v[a], *vb = &T.v[b], *vc = &T.v[c];
    if (va->clip[3] < NEAR_W || vb->clip[3] < NEAR_W || vc->clip[3] < NEAR_W) {
        const CVert *in[3] = {va, vb, vc};
        CVert out[4];
        int n = 0;
        for (int i = 0; i < 3; i++) {
            const CVert *p = in[i], *q = in[(i + 1) % 3];
            const bool pin = p->clip[3] >= NEAR_W, qin = q->clip[3] >= NEAR_W;
            if (pin) out[n++] = *p;
            if (pin != qin) out[n++] = lerp_vert(p, q, (NEAR_W - p->clip[3]) / (q->clip[3] - p->clip[3]));
        }
        /* culling isn't reliable across the eye plane; the clipped pieces are drawn as they wind */
        for (int i = 1; i + 1 < n; i++) emit_tri(&out[0], &out[i], &out[i + 1]);
        return;
    }
    if (T.cull != 2 && va->clip[3] > 0 && vb->clip[3] > 0 && vc->clip[3] > 0) {
        const float ax = va->clip[0] / va->clip[3], ay = va->clip[1] / va->clip[3];
        const float bx = vb->clip[0] / vb->clip[3], by = vb->clip[1] / vb->clip[3];
        const float cx = vc->clip[0] / vc->clip[3], cy = vc->clip[1] / vc->clip[3];
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);  /* > 0: counter-clockwise (front) */
        if ((T.cull == 0 && area <= 0) || (T.cull == 1 && area >= 0)) return;
    }
    emit_tri(va, vb, vc);
}

void hle_t3d_flush(uint8_t *rdram, recomp_context *ctx) {
    if (!T.ntri) return;
    const int vw = T.vw > 0 ? T.vw : hle_video.w, vh = T.vh > 0 ? T.vh : hle_video.h;
    hle_rdpq_draw3d(rdram, ctx, T.tri, T.ntri, T.vw > 0 ? T.vx : 0, T.vh > 0 ? T.vy : 0, vw, vh);
    T.ntri = 0;
}

void hle_t3d_exec(uint8_t *rdram, recomp_context *ctx, const uint32_t *w, int n) {
    if (n < 1) return;
    const int cmd = (int)((w[0] >> 24) & 0xF);
    switch (cmd) {
        case 0x0: tri(slot_of(w[0]), slot_of(w[1] >> 16), slot_of(w[1])); break;  /* TRI_DRAW */
        case 0x1: hle_t3d_flush(rdram, ctx); if (n >= 6) screen_size(w); break;  /* SCREEN_SIZE */
        case 0x2: if (n >= 2) matrix_stack(rdram, w[0], w[1]); break;              /* MATRIX_STACK */
        case 0x3:                                                                    /* SET_WORD */
#if T3D_HAVE
            if (n >= 2) {
                const uint32_t addr = w[0] & 0x7FFF, v = w[1];
                if (addr == DMEM(RSP_T3D_ACTIVE_LIGHT_SIZE)) T.nlights = SDL_min((int)((v >> 8) & 0xFF) / 16, 7);
                else if (addr == DMEM(RSP_T3D_SCREEN_SCALE) + 6) T.fog_slope_i = (uint16_t)v;   /* the fog range */
                else if (addr == DMEM(RSP_T3D_SCREEN_SCALE) + 14) T.fog_slope_f = (uint16_t)v;
                else if (addr == DMEM(RSP_T3D_SCREEN_OFFSET) + 6) T.fog_off = (int16_t)v;
                else if (addr == DMEM(RSP_T3D_COLOR_EXPOSURE)) T.exposure = (float)(int16_t)v / 128.0f;
                else if (addr >= DMEM(RSP_T3D_SEGMENT_TABLE) && addr < DMEM(RSP_T3D_SEGMENT_TABLE) + 64)
                    T.seg[((addr - DMEM(RSP_T3D_SEGMENT_TABLE)) / 4) & 15] = v;
            }
#endif
            break;
        case 0x4: if (n >= 3) vert_load(rdram, w); break;                            /* VERT_LOAD */
        case 0x5:                                                                    /* LIGHT_SET */
#if T3D_HAVE
            if (n >= 4) {
                const uint32_t addr = w[0] & 0xFFFF;
                if (addr == DMEM(RSP_T3D_COLOR_AMBIENT)) {
                    for (int i = 0; i < 4; i++) T.ambient[i] = (uint8_t)(w[1] >> (24 - i * 8));
                } else {
                    const int i = (int)(addr - DMEM(RSP_T3D_LIGHT_DIR_COLOR)) / 16;
                    if (i >= 0 && i < 8) {
                        for (int k = 0; k < 4; k++) T.light[i].c[k] = (uint8_t)(w[1] >> (24 - k * 8));
                        T.light[i].dir[0] = (float)(int8_t)(w[2] >> 24) / 127.0f;
                        T.light[i].dir[1] = (float)(int8_t)(w[2] >> 16) / 127.0f;
                        T.light[i].dir[2] = (float)(int8_t)(w[2] >> 8) / 127.0f;
                        /* a point light: XXYY ZZSS, positions in quarter eye units; byte 15 (S's low byte)
                         * is non-zero only for point lights */
                        T.light[i].point = (w[3] & 0xFF) != 0;
                        T.light[i].pos[0] = (float)(int16_t)(w[2] >> 16) / 4.0f;
                        T.light[i].pos[1] = (float)(int16_t)(w[2] & 0xFFFF) / 4.0f;
                        T.light[i].pos[2] = (float)(int16_t)(w[3] >> 16) / 4.0f;
                        T.light[i].size = (int)(w[3] & 0x7FFF);
                        T.light[i].no_normals = (w[3] & 0x8000) != 0;
                    }
                }
            }
#endif
            break;
        case 0x6:                                                                    /* DRAWFLAGS */
            if (n >= 2) {
                T.cull = (int)(w[0] & 3);
                T.unlit = (w[1] >> 24) & 1;  /* T3D_FLAG_NO_LIGHT */
            }
            break;
        case 0x7: hle_t3d_flush(rdram, ctx); load_mat(rdram, w[0] & 0xFFFFFF, T.proj); mat_mul(T.mvp, T.proj, T.mv); break;
        case 0xB:                                                                    /* TRI_STRIP */
            if (tracing()) n64rt_log("t3d strip %08X %08X", w[0], w[1]);
            if (n >= 2) {
                const uint32_t src = resolve(w[0] & 0xFFFFFF);
                const int count = (int)((w[1] & 0xFFFF) + 1) / 2;
                int start = 0, prev[2] = {-1, -1}, k = 0;
                for (int i = 0; i < count; i++) {
                    const uint16_t ix = n64_r16(rdram, src + (uint32_t)i * 2);
                    if ((ix & 0x8000) || i == 0) { start = i; k = 0; }
                    const int s = slot_of(ix);
                    if (k >= 2) {
                        if ((i - start) & 1) tri(prev[1], prev[0], s);
                        else tri(prev[0], prev[1], s);
                    }
                    prev[0] = prev[1];
                    prev[1] = s;
                    k++;
                    if (ix & 0x4000) break;
                }
            }
            break;
        case 0xC:                                                                    /* TRI_SEQ */
            if (tracing()) n64rt_log("t3d seq %08X %08X", w[0], w[1]);
            if (n >= 2) {
                const bool quad = (w[1] >> 16) != 0;
                const uint32_t end = w[1] & 0xFFFF, step = (quad ? 4 : 3) * VERT_OUTPUT_SIZE;
                for (uint32_t a = w[0] & 0xFFFF; a + step <= end + 0; a += step) {
                    const int s0 = slot_of(a), s1 = slot_of(a + 36), s2 = slot_of(a + 72);
                    tri(s0, s1, s2);
                    if (quad) tri(s0, s2, slot_of(a + 108));
                }
            }
            break;
        case 0x9: hle_t3d_flush(rdram, ctx); T.fog = (w[0] & 0xFF) == 0x0B; break;  /* FOG_STATE: 0x0B on, 0x0C off */
        default:  /* 0x8 PATCH, 0xA TRI_SYNC: nothing to do here */
            break;
    }
    if (!T.logged && cmd == 0x0) {
        T.logged = true;
        n64rt_log("t3d: drawing (host microcode%s)", T3D_HAVE ? "" : ": NO rsp_tiny3d.h, nothing drawn");
    }
}

/* ==== tinypx (rsp_tinypx): tiny3d's particles ======================================================
 * A draw command carries a buffer of particles, two per record (TPXParticleS8: s8 positions, 16 bytes;
 * TPXParticleS16: s16 positions, 24 bytes). The microcode projects each with its own MVP (tiny3d's
 * projection times tinypx's matrix stack, taken from tiny3d by SYNC_T3D) and writes three RDP
 * commands per particle: a PRIM colour, a PRIM depth and a rectangle (textured: tile 0, one 8x8
 * texel cell of the sprite, offset per particle for animation). Here each becomes a quad in the
 * same clip space as tiny3d's triangles, with its colour as the vertex colour and its depth constant,
 * drawn in one batch with the current render mode (src/t3d/rsp/tpxLoops.rspl in tiny3d). */
#if __has_include("rsp_tinypx.h")
#include "rsp_tinypx.h"
#define TPX_HAVE 1
#else
#define TPX_HAVE 0
#endif

static struct {
    float proj[16], mvp[16];
    float stack[STACK][16];
    int sp;           /* bytes, relative to where tinypx's stack started */
    float normw;      /* W normalisation (tiny3d's, from SYNC_T3D) */
    float scale[2];   /* PARTICLE_SCALE, 0..1 */
    int base;         /* BASE_SIZE */
    int16_t tex_off;  /* TEX_OFFSET: quarter cells */
    uint16_t tiles;   /* TILE_COUNT: cells before the animation mirrors */
    GfxVert3D *quad;
    int cap;
    int logged;
} P = {.normw = 1, .scale = {1, 1}, .base = 128, .tiles = 128};

static void tpx_matrix(uint8_t *rdram, uint32_t w0, uint32_t addr) {
    const int advance = (int16_t)((w0 >> 8) & 0xFFFF);
    P.sp += advance;
    if (w0 & 2) return;  /* only move the stack pointer */
    const int idx = SDL_clamp(P.sp / 64, 0, STACK - 1);
    float m[16];
    if (advance < 0) SDL_memcpy(m, P.stack[idx], sizeof m);
    else load_mat(rdram, addr & 0xFFFFFF, m);  /* tinypx has no segments: a physical address */
    if (w0 & 1) mat_mul(m, P.stack[SDL_clamp(idx - 1, 0, STACK - 1)], m);
    SDL_memcpy(P.stack[idx], m, sizeof m);
    mat_mul(P.mvp, P.proj, m);
}

/* A quad of 6 vertices (two triangles) around a particle: NDC centre and half extents, its colour,
 * and its texture cell (u0, v0, side) in texels before the tile's shift. */
static void tpx_quad(const float *ndc, float hx, float hy, const uint8_t *c, float u0, float v0, float cell, int *nv) {
    if (*nv + 6 > P.cap) {
        P.cap = SDL_max(P.cap * 2, 1536);
        P.quad = (GfxVert3D *)SDL_realloc(P.quad, sizeof(GfxVert3D) * (size_t)P.cap);
    }
    const float xs[4] = {ndc[0] - hx, ndc[0] + hx, ndc[0] + hx, ndc[0] - hx};
    const float ys[4] = {ndc[1] + hy, ndc[1] + hy, ndc[1] - hy, ndc[1] - hy};  /* the top first */
    const float us[4] = {u0, u0 + cell, u0 + cell, u0}, vs[4] = {v0, v0, v0 + cell, v0 + cell};
    static const int corner[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        GfxVert3D *o = &P.quad[(*nv)++];
        const int k = corner[i];
        o->x = xs[k];
        o->y = ys[k];
        o->z = ndc[2];
        o->w = 1.0f;
        o->u = us[k];
        o->v = vs[k];
        SDL_memcpy(o->c, c, 4);
    }
}

/* DRAW_COLOR / DRAW_TEXTURE: w0 = byte count of the buffer, w1 = its address (bit 31: 16-bit positions). */
static void tpx_draw(uint8_t *rdram, recomp_context *ctx, const uint32_t *w, bool tex) {
    const bool s16 = (w[1] & 0x80000000u) != 0;
    const uint32_t bytes = w[0] & 0xFFFF, src = (w[1] & 0xFFFFFF) | 0x80000000u, stride = s16 ? 24u : 16u;
    const int vx = T.vw > 0 ? T.vx : 0, vy = T.vh > 0 ? T.vy : 0;
    const int vw = T.vw > 0 ? T.vw : hle_video.w, vh = T.vh > 0 ? T.vh : hle_video.h;
    if (vw <= 0 || vh <= 0) return;
    /* the animation's cells: offsets wrap at twice TILE_COUNT cells, mirrored in the second half (16-bit lanes) */
    const int16_t mask = (int16_t)(uint16_t)(2u * P.tiles * 256u - 1u), cmp = (int16_t)(uint16_t)(P.tiles * 256u - 1u);
    hle_t3d_flush(rdram, ctx);  /* tiny3d's pending triangles come first */
    int nv = 0;
    for (uint32_t off = 0; off + stride <= bytes; off += stride) {
        const uint32_t rec = src + off;
        for (int k = 0; k < 2; k++) {
            float pos[3];
            int size_word;  /* the size as the microcode's w lane: s8 size << 8 (| texture offset, 16-bit) */
            int8_t toff;
            uint8_t c[4];
            if (s16) {
                const uint32_t q = rec + (uint32_t)k * 8;
                for (int i = 0; i < 3; i++) pos[i] = (float)(int16_t)n64_r16(rdram, q + (uint32_t)i * 2);
                size_word = (int16_t)n64_r16(rdram, q + 6);
                toff = (int8_t)n64_r8(rdram, q + 7);
                for (int i = 0; i < 4; i++) c[i] = n64_r8(rdram, rec + 16 + (uint32_t)(k * 4 + i));
            } else {
                const uint32_t q = rec + (uint32_t)k * 4;
                for (int i = 0; i < 3; i++) pos[i] = (float)(int8_t)n64_r8(rdram, q + (uint32_t)i);
                size_word = (int8_t)n64_r8(rdram, q + 3) * 256;
                for (int i = 0; i < 4; i++) c[i] = n64_r8(rdram, rec + 8 + (uint32_t)(k * 4 + i));
                toff = (int8_t)c[3];  /* textured: the alpha channel is the cell offset */
            }
            float clip[4];
            for (int r = 0; r < 4; r++) clip[r] = P.mvp[r] * pos[0] + P.mvp[4 + r] * pos[1] + P.mvp[8 + r] * pos[2] + P.mvp[12 + r];
            /* only Z is clip-tested (X/Y are clamped to the scissor) */
            if (!(clip[3] > 0) || clip[2] > clip[3] || clip[2] < -clip[3]) continue;
            /* the size: base size / (2 w'), w' = w normalised, as an integer, times size/128 (10.2 pixels) */
            const float persp = SDL_floorf((float)P.base * 0.5f / (clip[3] * P.normw));
            const float hx = SDL_truncf((float)size_word / 32768.0f * P.scale[0] * persp) / 4.0f;
            const float hy = SDL_truncf((float)size_word / 32768.0f * P.scale[1] * persp) / 4.0f;
            if (hx <= 0 || hy <= 0) continue;
            const float ndc[3] = {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
            float u0 = 0, v0 = 0;
            if (tex) {
                int16_t total = (int16_t)(((int)toff + P.tex_off) * 8);
                total = (int16_t)(total & mask);
                if (cmp >= total) {
                    u0 = (float)total / 32.0f;
                } else {  /* the mirrored half runs backwards */
                    u0 = (float)(int16_t)(mask + 1 - 256 - total + (int)P.tiles * 256) / 32.0f;
                    v0 = (float)(int16_t)(256 + (int)P.tiles * 256) / 32.0f;
                }
            }
            if (!P.logged) {
                P.logged = 1;
                n64rt_log("tpx: drawing particles (host microcode); first: size %d at w %.1f -> %.1f x %.1f px", size_word >> 8,
                          clip[3], hx * 2, hy * 2);
            }
            tpx_quad(ndc, hx * 2.0f / (float)vw, hy * 2.0f / (float)vh, c, u0, v0, 8.0f, &nv);
        }
    }
    if (nv) hle_rdpq_draw_particles(rdram, ctx, P.quad, nv, vx, vy, vw, vh);
}

void hle_tpx_exec(uint8_t *rdram, recomp_context *ctx, const uint32_t *w, int n) {
    if (n < 1) return;
    const int cmd = (int)((w[0] >> 24) & 0xF);
    switch (cmd) {
        case 0x0:  /* SYNC_T3D: tiny3d's projection and MVP, and its model-view onto our stack */
            SDL_memcpy(P.proj, T.proj, sizeof P.proj);
            SDL_memcpy(P.mvp, T.mvp, sizeof P.mvp);
            SDL_memcpy(P.stack[SDL_clamp(P.sp / 64, 0, STACK - 1)], T.mv, sizeof T.mv);
            if (n >= 3 && (w[2] & 0xFFFF)) P.normw = (float)(w[2] & 0xFFFF) / 65535.0f;
            break;
        case 0x2: if (n >= 2) tpx_matrix(rdram, w[0], w[1]); break;  /* MATRIX_STACK */
        case 0x3:                                                     /* SET_DMEM */
#if TPX_HAVE
            if (n >= 2) {
                const uint32_t addr = w[0] & 0x7FFF, v = w[1];
                const bool u16 = (w[0] & 0x8000) != 0;
                if (addr == ((uint32_t)RSP_TPX_PARTICLE_SCALE & 0xFFF) && !u16) {
                    P.scale[0] = (float)(v >> 16) / 32768.0f;
                    P.scale[1] = (float)(v & 0xFFFF) / 32768.0f;
                } else if (addr == ((uint32_t)RSP_TPX_BASE_SIZE & 0xFFF)) {
                    P.base = (int)(v & 0xFFFF);
                } else if (addr == ((uint32_t)RSP_TPX_TEX_OFFSET & 0xFFF)) {
                    if (u16) P.tex_off = (int16_t)v;
                    else { P.tex_off = (int16_t)(v >> 16); P.tiles = (uint16_t)v; }
                } else if (addr == ((uint32_t)RSP_TPX_TILE_COUNT & 0xFFF) && u16) {
                    P.tiles = (uint16_t)v;
                }
            }
#endif
            break;
        case 0x1:  /* DRAW_COLOR */
        case 0x4:  /* DRAW_TEXTURE */
            if (n >= 2) tpx_draw(rdram, ctx, w, cmd == 0x4);
            break;
        default: break;
    }
}
