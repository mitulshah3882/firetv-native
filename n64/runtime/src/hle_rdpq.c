/*
 * Host implementation of libdragon's RDP command layer (rdpq) for 2D drawing.
 *
 * Where the line is drawn: libdragon code that only computes command words stays recompiled.
 * That covers rdpq_set_mode_standard/copy/fill, rdpq_mode_begin/end, the out-of-line rectangle
 * helpers, and every inline function in libdragon's headers (rdpq_mode_combiner,
 * rdpq_mode_blender, rdpq_mode_alphacompare, rdpq_set_prim_color, rdpq_fill_rectangle, ...).
 * Their leaves, the functions that would write the RSP command queue, land here:
 * __rdpq_reset_render_mode, __rdpq_fixup_mode*, __rdpq_write8_syncchange, __rdpq_fill_rectangle,
 * __rdpq_texture_rectangle, rdpq_mode_push/pop, ... From them we keep a model of the RDP state:
 * the colour combiner, the blender, the other-modes word, the constant colours, the render-mode
 * stack, the scissor and the tile descriptors.
 *
 * Drawing goes through the SDL renderer. A rectangle has no per-pixel input besides its texture,
 * so the combiner (1 or 2 cycles), alpha compare, and the blender terms that don't read the
 * framebuffer are evaluated exactly, per texel, into a cached "baked" texture. What does read the
 * framebuffer maps onto SDL blend modes: P*A + MEM*(1-A) is alpha blending, P*A + MEM additive.
 * Texture loads (rdpq_sprite_upload, rdpq_tex_upload) bind a decoded texture to a tile; texture
 * rectangles then sample it with the tile's wrap/mirror/clamp settings.
 *
 * rspq blocks record these calls and replay them.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "hle_internal.h"

/* Recompiled libdragon helper (weak: absent in programs that never link it). */
extern void n64_sprite_get_palette(uint8_t *rdram, recomp_context *ctx) __attribute__((weak));

static float bits_f(uint32_t b) { return n64_bits_to_float(b); }

/* ================================================================================================
 * RDP state
 * ============================================================================================== */
enum { CYC_1 = 0, CYC_2 = 1, CYC_COPY = 2, CYC_FILL = 3 };

/* What rdpq_mode_push/pop save: the render mode. */
typedef struct {
    uint64_t cc;       /* combiner (both cycle slots) */
    bool cc_2pass;     /* the combiner needs two cycles */
    uint64_t som;      /* other modes */
    uint32_t blend;    /* blender formula (SET_BLENDING_MODE), 0 = off */
    uint32_t fog;      /* fog formula (SET_FOG_MODE) */
} RenderMode;

typedef struct {
    int kind;          /* 0 = none, 1 = sprite, 2 = surface */
    uint32_t addr;     /* sprite_t, or the surface's pixel buffer */
    int fmt, w, h, stride;
    float s_shift, t_shift;   /* texparms translate */
    int s_scale, t_scale;     /* texparms scale_log */
    bool s_wrap, t_wrap, s_mirror, t_mirror;
} Tile;

static struct {
    RenderMode m;
    RenderMode stack[16];
    int sp;
    uint32_t prim, env, fog, blend, fill; /* RGBA32 */
    uint8_t prim_lod;
    Tile tile[8];
    uint16_t tlut[256];                   /* palette loaded with rdpq_tex_upload_tlut */
    bool tlut_loaded;
    uint32_t frame;
} R;

/* ================================================================================================
 * Texture decoding and cache
 * ============================================================================================== */
#define TEX_CACHE 384
static HleTex g_tex[TEX_CACHE];
static uint32_t g_tex_clock;

static void rgba16(uint16_t p, uint8_t *out) {
    out[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((p >> 6) & 31) * 255 / 31);
    out[2] = (uint8_t)(((p >> 1) & 31) * 255 / 31);
    out[3] = (p & 1) ? 255 : 0;
}
static void ia16(uint16_t p, uint8_t *out) {
    out[0] = out[1] = out[2] = (uint8_t)(p >> 8);
    out[3] = (uint8_t)p;
}

/* A cheap fingerprint of a texture (format, size and a sample of its bytes): memory is reused
 * after sprite_free(), so the address alone can't identify a texture. */
static uint32_t texture_sig(uint8_t *rdram, int fmt, int w, int h, int stride, uint32_t data) {
    uint32_t s = 2166136261u ^ (uint32_t)(fmt << 24 | w << 12 | h);
    const uint32_t size = (uint32_t)(stride * h);
    for (int i = 0; i < 24; i++) {
        const uint32_t off = (size / 24) * (uint32_t)i & ~3u;
        s = (s ^ n64_r32(rdram, data + off)) * 16777619u;
    }
    return s;
}

static HleTex *decode_texture(uint8_t *rdram, uint64_t key, int fmt, int w, int h, int stride, uint32_t data,
                              uint32_t pal, bool host_pal, int layer, bool mask) {
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    const uint32_t sig = texture_sig(rdram, fmt, w, h, stride, data) ^ pal ^ (host_pal ? 0x5A5A : 0);
    HleTex *slot = NULL, *lru = &g_tex[0];
    for (int i = 0; i < TEX_CACHE; i++) {
        HleTex *t = &g_tex[i];
        if (t->tex && t->key == key) {
            if (t->sig == sig && t->w == w && t->h == h) {
                t->stamp = ++g_tex_clock;
                return t;
            }
            slot = t;  /* same address, new content: decode again in place */
            break;
        }
        if (!t->tex && !slot) slot = t;
        if (t->stamp < lru->stamp) lru = t;
    }
    if (!slot) slot = lru;
    if (slot->tex) {
        SDL_DestroyTexture(slot->tex);
        SDL_free(slot->px);
    }
    const int bpp = 4 << (fmt & 3); /* TEX_FORMAT_BITDEPTH */
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint32_t row = data + (uint32_t)(y * stride);
        for (int x = 0; x < w; ++x) {
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            uint32_t v = 0;
            if (bpp == 4) v = (n64_r8(rdram, row + x / 2) >> ((x & 1) ? 0 : 4)) & 0xF;
            else if (bpp == 8) v = n64_r8(rdram, row + x);
            else if (bpp == 16) v = (uint32_t)n64_r8(rdram, row + x * 2) << 8 | n64_r8(rdram, row + x * 2 + 1);
            else v = n64_r32(rdram, row + x * 4);
            switch (fmt) {
                case 2: rgba16((uint16_t)v, o); break;                                   /* RGBA16 */
                case 3: o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = v; break;  /* RGBA32 */
                case 8: case 9:                                                          /* CI4/CI8 */
                    if (layer >= 0) {
                        const uint8_t on = (v >> (3 - layer)) & 1;
                        o[0] = o[1] = o[2] = 255;
                        o[3] = on ? 255 : 0;
                    } else if (pal) {
                        rgba16(n64_r16(rdram, pal + v * 2), o);
                    } else if (host_pal) {
                        rgba16(R.tlut[v & 255], o);
                    } else {
                        o[0] = o[1] = o[2] = (uint8_t)(v * (fmt == 8 ? 17 : 1));
                        o[3] = 255;
                    }
                    break;
                case 12: { const uint8_t i = (uint8_t)((v >> 1) * 255 / 7); o[0] = o[1] = o[2] = mask ? 255 : i; o[3] = (v & 1) ? 255 : 0; } break;  /* IA4 */
                case 13: { const uint8_t i = (uint8_t)((v >> 4) * 17); o[0] = o[1] = o[2] = mask ? 255 : i; o[3] = (uint8_t)((v & 15) * 17); } break; /* IA8 */
                case 14: ia16((uint16_t)v, o); if (mask) o[0] = o[1] = o[2] = 255; break;                                                                 /* IA16 */
                case 16: { const uint8_t i = (uint8_t)(v * 17); o[0] = o[1] = o[2] = mask ? 255 : i; o[3] = i; } break;                              /* I4 */
                case 17: o[0] = o[1] = o[2] = mask ? 255 : (uint8_t)v; o[3] = (uint8_t)v; break;                                                     /* I8 */
                default: o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; break;
            }
        }
    }
    SDL_Texture *t = SDL_CreateTexture(hle_video.r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    *slot = (HleTex){key, sig, t, px, w, h, ++g_tex_clock};
    n64rt_log("texture %08X: %dx%d fmt %d%s", (uint32_t)key, w, h, fmt, layer >= 0 ? " (font layer)" : "");
    return slot;
}

static int stride_of(int fmt, int w) { return (w * (4 << (fmt & 3)) + 7) / 8; }

HleTex *hle_sprite_texture(uint8_t *rdram, recomp_context *ctx, uint32_t sprite, int layer, bool mask) {
    /* kind: 1 + layer (0 = whole image), +8 for alpha-mask decoding */
    const uint64_t key = (uint64_t)((layer + 1) + (mask ? 8 : 0)) << 32 | (sprite & 0x1FFFFFFFu);
    const int w = n64_r16(rdram, sprite), h = n64_r16(rdram, sprite + 2);
    const int fmt = n64_r8(rdram, sprite + 5) & 0x1F;
    uint32_t pal = 0;
    if ((fmt == 8 || fmt == 9) && layer < 0 && n64_sprite_get_palette) pal = (uint32_t)n64rt_call(ctx, n64_sprite_get_palette, sprite, 0, 0, 0);
    return decode_texture(rdram, key, fmt, w, h, stride_of(fmt, w), sprite + 8, pal, false, layer, mask);
}

/* surface_t { uint16_t flags, width, height, stride; void *buffer; } */
static HleTex *surface_texture(uint8_t *rdram, uint32_t flags_w, uint32_t h_stride, uint32_t buffer) {
    const int fmt = (int)(flags_w >> 16) & 0x1F, w = (int)(flags_w & 0xFFFF);
    const int h = (int)(h_stride >> 16), stride = (int)(h_stride & 0xFFFF);
    const bool ci = fmt == 8 || fmt == 9;
    return decode_texture(rdram, (uint64_t)16 << 32 | (buffer & 0x1FFFFFFFu), fmt, w, h, stride, buffer, 0, ci && R.tlut_loaded, -1, false);
}

static HleTex *tile_texture(uint8_t *rdram, recomp_context *ctx, const Tile *t) {
    if (t->kind == 1) return hle_sprite_texture(rdram, ctx, t->addr, -1, false);
    if (t->kind == 2) return surface_texture(rdram, (uint32_t)t->fmt << 16 | (uint32_t)t->w, (uint32_t)t->h << 16 | (uint32_t)t->stride, t->addr);
    return NULL;
}

/* ================================================================================================
 * Combiner and blender, evaluated per texel
 * ============================================================================================== */
typedef struct { int v[4]; } C4; /* r, g, b, a in 0..255 */

typedef struct { uint8_t ra, rb, rc, rd, aa, ab, ac, ad; } Cycle;

static void decode_cc(uint64_t cc, Cycle c[2]) {
    c[0] = (Cycle){(cc >> 52) & 0xF, (cc >> 28) & 0xF, (cc >> 47) & 0x1F, (cc >> 15) & 7,
                   (cc >> 44) & 7, (cc >> 12) & 7, (cc >> 41) & 7, (cc >> 9) & 7};
    c[1] = (Cycle){(cc >> 37) & 0xF, (cc >> 24) & 0xF, (cc >> 32) & 0x1F, (cc >> 6) & 7,
                   (cc >> 21) & 7, (cc >> 3) & 7, (cc >> 18) & 7, (cc >> 0) & 7};
}

static C4 unpack(uint32_t c) { return (C4){{(int)(c >> 24), (int)(c >> 16) & 255, (int)(c >> 8) & 255, (int)c & 255}}; }

typedef struct { C4 prim, env, shade; int prim_lod; } Consts;

/* Common inputs: 0 COMBINED, 1 TEX0, 2 TEX1, 3 PRIM, 4 SHADE, 5 ENV. */
static inline int common(int s, int ch, const C4 *comb, const C4 *tex, const Consts *k) {
    switch (s) {
        case 0: return comb->v[ch];
        case 1: case 2: return tex->v[ch];
        case 3: return k->prim.v[ch];
        case 4: return k->shade.v[ch];
        case 5: return k->env.v[ch];
        default: return 0;
    }
}
static inline int rgb_a(int s, int ch, const C4 *comb, const C4 *tex, const Consts *k) {
    if (s <= 5) return common(s, ch, comb, tex, k);
    if (s == 6) return 256;                      /* ONE */
    if (s == 7) return SDL_rand(256);            /* NOISE */
    return 0;
}
static inline int rgb_b(int s, int ch, const C4 *comb, const C4 *tex, const Consts *k) {
    return s <= 5 ? common(s, ch, comb, tex, k) : 0; /* KEYCENTER, K4: chroma key / YUV, unused */
}
static inline int rgb_c(int s, int ch, const C4 *comb, const C4 *tex, const Consts *k) {
    if (s <= 5) return common(s, ch, comb, tex, k);
    switch (s) {
        case 7: return comb->v[3];
        case 8: case 9: return tex->v[3];
        case 10: return k->prim.v[3];
        case 11: return k->shade.v[3];
        case 12: return k->env.v[3];
        case 14: return k->prim_lod;
        default: return 0;                       /* KEYSCALE, LOD_FRAC, K5, ZERO */
    }
}
static inline int rgb_d(int s, int ch, const C4 *comb, const C4 *tex, const Consts *k) {
    if (s <= 5) return common(s, ch, comb, tex, k);
    return s == 6 ? 255 : 0;
}
static inline int alpha_abd(int s, const C4 *comb, const C4 *tex, const Consts *k) {
    if (s <= 5) return common(s, 3, comb, tex, k);
    return s == 6 ? 255 : 0;
}
static inline int alpha_c(int s, const C4 *comb, const C4 *tex, const Consts *k) {
    if (s >= 1 && s <= 5) return common(s, 3, comb, tex, k);
    return s == 6 ? k->prim_lod : 0;             /* 0 = LOD_FRAC */
}
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static C4 run_cycle(const Cycle *c, const C4 *comb, const C4 *tex, const Consts *k) {
    C4 o;
    for (int ch = 0; ch < 3; ch++) {
        const int a = rgb_a(c->ra, ch, comb, tex, k), b = rgb_b(c->rb, ch, comb, tex, k);
        const int m = rgb_c(c->rc, ch, comb, tex, k), d = rgb_d(c->rd, ch, comb, tex, k);
        o.v[ch] = clamp255(((a - b) * m + (d << 8) + 0x80) >> 8);
    }
    const int a = alpha_abd(c->aa, comb, tex, k), b = alpha_abd(c->ab, comb, tex, k);
    const int m = alpha_c(c->ac, comb, tex, k), d = alpha_abd(c->ad, comb, tex, k);
    o.v[3] = clamp255(((a - b) * m + (d << 8) + 0x80) >> 8);
    return o;
}

/* Blender P/M inputs: 0 IN (combined), 1 MEMORY, 2 BLEND colour, 3 FOG colour.
 * A: 0 IN alpha, 1 FOG alpha, 2 SHADE alpha, 3 ZERO.  B: 0 1-A, 1 MEMORY coverage, 2 ONE, 3 ZERO. */
enum { BL_IN = 0, BL_MEM = 1, BL_BLEND = 2, BL_FOG = 3 };
typedef struct {
    int cycle;             /* CYC_* */
    Cycle cc[2];
    int ncycles;
    bool blend_on;
    int p, a, m, b;        /* blender, cycle 0 */
    int ac;                /* alpha compare: 0 off, 1 threshold, 3 noise */
    int ac_threshold;
    Consts k;
    C4 blend_c, fog_c;
} DrawState;

static DrawState draw_state(void) {
    DrawState s;
    s.cycle = (int)(R.m.som >> 52) & 3;
    decode_cc(R.m.cc, s.cc);
    s.ncycles = R.m.cc_2pass ? 2 : 1;
    const uint32_t bl = R.m.blend;
    s.blend_on = bl != 0;
    s.p = (bl >> 30) & 3;
    s.a = (bl >> 26) & 3;
    s.m = (bl >> 22) & 3;
    s.b = (bl >> 18) & 3;
    s.ac = (int)(R.m.som & 3);
    s.ac_threshold = (int)(R.blend & 0xFF);
    s.k.prim = unpack(R.prim);
    s.k.env = unpack(R.env);
    s.k.shade = (C4){{0, 0, 0, 0}};             /* rectangles have no shade */
    s.k.prim_lod = R.prim_lod;
    s.blend_c = unpack(R.blend);
    s.fog_c = unpack(R.fog);
    return s;
}

/* How a draw maps onto the SDL renderer. */
typedef struct {
    SDL_BlendMode mode;
    uint8_t alpha_mod;     /* constant source alpha (FOG alpha as the blend factor) */
    bool skip;             /* the result is the framebuffer itself */
} Output;

static Output output_for(const DrawState *s) {
    Output o = {SDL_BLENDMODE_NONE, 255, false};
    if (s->cycle == CYC_COPY) {
        if (s->ac) o.mode = SDL_BLENDMODE_BLEND;
        return o;
    }
    if (s->cycle == CYC_FILL || !s->blend_on) {
        if (s->ac) o.mode = SDL_BLENDMODE_BLEND;
        return o;
    }
    if (s->m == BL_MEM || s->p == BL_MEM) {
        if (s->m == BL_MEM && s->p == BL_MEM) { o.skip = true; return o; }
        o.mode = (s->b == 2) ? SDL_BLENDMODE_ADD : (s->b == 3 && s->m == BL_MEM) ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND;
        if (s->a == 1) o.alpha_mod = (uint8_t)s->fog_c.v[3];
        if (s->ac && o.mode == SDL_BLENDMODE_NONE) o.mode = SDL_BLENDMODE_BLEND;
        return o;
    }
    if (s->ac) o.mode = SDL_BLENDMODE_BLEND;   /* no framebuffer read: opaque (alpha compare cuts) */
    return o;
}

static int blend_color_in(int sel, int ch, const C4 *in, const DrawState *s) {
    switch (sel) {
        case BL_IN: return in->v[ch];
        case BL_BLEND: return s->blend_c.v[ch];
        case BL_FOG: return s->fog_c.v[ch];
        default: return 0;
    }
}

/* One texel (or the constant colour of an untextured rectangle) through combiner, alpha compare
 * and the framebuffer-independent part of the blender. Returns the colour to hand to SDL. */
static C4 shade_texel(const DrawState *s, const Output *o, const C4 *tex) {
    if (s->cycle == CYC_COPY) {
        C4 r = *tex;
        if (s->ac) r.v[3] = tex->v[3] ? 255 : 0;
        return r;
    }
    C4 comb = {{0, 0, 0, 0}};
    comb = run_cycle(&s->cc[0], &comb, tex, &s->k);
    if (s->ncycles == 2) comb = run_cycle(&s->cc[1], &comb, tex, &s->k);
    bool pass = true;
    if (s->ac == 1) pass = comb.v[3] >= s->ac_threshold;
    else if (s->ac == 3) pass = comb.v[3] >= SDL_rand(256);
    C4 r;
    if (!s->blend_on) {
        r = comb;
        r.v[3] = pass ? 255 : 0;
        return r;
    }
    const int a = s->a == 0 ? comb.v[3] : s->a == 1 ? 255 /* via alpha_mod */ : 0;
    if (s->m == BL_MEM) {                       /* P*A + MEM*B */
        for (int ch = 0; ch < 3; ch++) r.v[ch] = blend_color_in(s->p, ch, &comb, s);
        if (o->mode == SDL_BLENDMODE_NONE) {    /* B = ZERO: P*A */
            const int aa = s->a == 1 ? s->fog_c.v[3] : a;
            for (int ch = 0; ch < 3; ch++) r.v[ch] = r.v[ch] * aa / 255;
            r.v[3] = 255;
        } else {
            r.v[3] = a;
        }
    } else if (s->p == BL_MEM) {                /* MEM*A + M*(1-A): draw M with alpha 1-A */
        for (int ch = 0; ch < 3; ch++) r.v[ch] = blend_color_in(s->m, ch, &comb, s);
        r.v[3] = 255 - (s->a == 1 ? s->fog_c.v[3] : a);
    } else {                                    /* no framebuffer read */
        const int aa = s->a == 1 ? s->fog_c.v[3] : a;
        const int bb = s->b == 0 || s->b == 1 ? 255 - aa : s->b == 2 ? 255 : 0;
        for (int ch = 0; ch < 3; ch++) {
            const int p = blend_color_in(s->p, ch, &comb, s), m = blend_color_in(s->m, ch, &comb, s);
            r.v[ch] = clamp255((p * aa + m * bb) / 255);
        }
        r.v[3] = 255;
    }
    if (!pass) r.v[3] = 0;
    return r;
}

/* Identity: the texture as decoded, alpha-blended or opaque. Skips baking. */
static bool is_plain(const DrawState *s) {
    if (s->cycle == CYC_COPY) return !s->ac;
    if (s->cycle != CYC_1 || s->ncycles != 1 || s->ac) return false;
    const Cycle *c = &s->cc[0];
    const bool tex_only = c->ra >= 8 && c->rb >= 8 && c->rc >= 16 && c->rd == 1 && c->aa == 7 && c->ab == 7 && c->ac == 7 && c->ad == 1;
    if (!tex_only) return false;
    return !s->blend_on || (s->p == BL_IN && s->a == 0 && s->m == BL_MEM && s->b == 0);
}

/* Baked textures: (texture, state) -> texture with the combiner applied. */
typedef struct {
    const HleTex *src;
    uint32_t src_sig, h;
    uint8_t hash[64];
    SDL_Texture *tex;
    uint32_t stamp;
} Baked;
#define BAKE_CACHE 160
static Baked g_bake[BAKE_CACHE];
static uint32_t g_bake_clock;

static SDL_Texture *bake(const HleTex *t, const DrawState *s, const Output *o) {
    /* Key: everything the per-texel result depends on. Constant colours only count when used. */
    uint8_t key[64];
    SDL_memset(key, 0, sizeof key);
    SDL_memcpy(key, &R.m.cc, 8);
    key[8] = (uint8_t)(s->cycle | s->ncycles << 2 | s->ac << 4 | s->blend_on << 6);
    key[9] = (uint8_t)(s->p | s->a << 2 | s->m << 4 | s->b << 6);
    key[10] = (uint8_t)s->ac_threshold;
    key[11] = (uint8_t)o->mode;
    SDL_memcpy(key + 12, &s->k.prim, 16);
    SDL_memcpy(key + 28, &s->k.env, 16);
    key[44] = (uint8_t)s->k.prim_lod;
    SDL_memcpy(key + 48, s->fog_c.v, 3);
    key[51] = (uint8_t)s->blend_c.v[0];
    key[52] = (uint8_t)s->blend_c.v[1];
    key[53] = (uint8_t)s->blend_c.v[2];
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof key; i++) h = (h ^ key[i]) * 16777619u;
    Baked *lru = &g_bake[0];
    for (int i = 0; i < BAKE_CACHE; i++) {
        Baked *b = &g_bake[i];
        if (b->tex && b->h == h && b->src == t && b->src_sig == t->sig && !SDL_memcmp(b->hash, key, sizeof key)) {
            b->stamp = ++g_bake_clock;
            return b->tex;
        }
        if (!b->tex) { lru = b; break; }
        if (b->stamp < lru->stamp) lru = b;
    }
    if (lru->tex) SDL_DestroyTexture(lru->tex);
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)t->w * t->h * 4);
    for (int i = 0; i < t->w * t->h; i++) {
        const uint8_t *p = t->px + i * 4;
        const C4 in = {{p[0], p[1], p[2], p[3]}};
        const C4 r = shade_texel(s, o, &in);
        for (int ch = 0; ch < 4; ch++) px[i * 4 + ch] = (uint8_t)r.v[ch];
    }
    SDL_Texture *tex = SDL_CreateTexture(hle_video.r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, t->w, t->h);
    SDL_UpdateTexture(tex, NULL, px, t->w * 4);
    SDL_free(px);
    *lru = (Baked){t, t->sig, h, {0}, tex, ++g_bake_clock};
    SDL_memcpy(lru->hash, key, sizeof key);
    return tex;
}

/* ================================================================================================
 * Drawing
 * ============================================================================================== */
static bool linear_filter(void) { return ((R.m.som >> 44) & 3) >= 2 && ((R.m.som >> 52) & 3) != CYC_COPY; }

/* Draws (part of) a texture with the current render mode. */
static void draw_tex(const HleTex *t, const SDL_FRect *src, const SDL_FRect *dst, SDL_FlipMode flip, double angle, const SDL_FPoint *center) {
    const DrawState s = draw_state();
    if (s.cycle == CYC_FILL) return;
    const Output o = output_for(&s);
    if (o.skip) return;
    const bool plain = is_plain(&s);
    SDL_Texture *tex = plain ? t->tex : bake(t, &s, &o);
    SDL_BlendMode mode = o.mode;
    if (plain) mode = (s.cycle != CYC_COPY && s.blend_on) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
    SDL_SetTextureBlendMode(tex, mode);
    const bool scaled = SDL_fabsf(src->w - dst->w) > 0.01f || SDL_fabsf(src->h - dst->h) > 0.01f || angle != 0;
    SDL_SetTextureScaleMode(tex, (linear_filter() && scaled) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, o.alpha_mod);
    if (angle != 0 || flip != SDL_FLIP_NONE) SDL_RenderTextureRotated(hle_video.r, tex, src, dst, angle, center, flip);
    else SDL_RenderTexture(hle_video.r, tex, src, dst);
}

/* For HLE code that draws on behalf of libdragon (font glyphs): draw with a given 1-cycle
 * combiner, blender and constant colours, leaving the program's render state untouched. */
uint64_t hle_cc1(int ra, int rb, int rc, int rd, int aa, int ab, int ac, int ad) {
    const uint64_t c0 = (uint64_t)ra << 52 | (uint64_t)rc << 47 | (uint64_t)aa << 44 | (uint64_t)ac << 41 |
                        (uint64_t)rb << 28 | (uint64_t)rd << 15 | (uint64_t)ab << 12 | (uint64_t)ad << 9;
    const uint64_t c1 = (uint64_t)ra << 37 | (uint64_t)rc << 32 | (uint64_t)aa << 21 | (uint64_t)ac << 18 |
                        (uint64_t)rb << 24 | (uint64_t)rd << 6 | (uint64_t)ab << 3 | (uint64_t)ad;
    return c0 | c1;
}
void hle_draw_combined(const HleTex *t, const SDL_FRect *src, const SDL_FRect *dst, uint64_t cc, uint32_t blend,
                       uint32_t prim, uint32_t env) {
    const RenderMode m = R.m;
    const uint32_t p = R.prim, e = R.env;
    R.m.cc = cc;
    R.m.cc_2pass = false;
    R.m.som = 0;               /* 1 cycle, point sampling, no alpha compare */
    R.m.blend = blend;
    R.prim = prim;
    R.env = env;
    draw_tex(t, src, dst, SDL_FLIP_NONE, 0, NULL);
    R.m = m;
    R.prim = p;
    R.env = e;
}

static void fill_rect(float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return;
    const SDL_FRect rc = {x0, y0, x1 - x0, y1 - y0};
    const DrawState s = draw_state();
    C4 c;
    SDL_BlendMode mode = SDL_BLENDMODE_NONE;
    uint8_t amod = 255;
    if (s.cycle == CYC_FILL) {
        c = unpack(R.fill);
        c.v[3] = 255;
    } else if (s.cycle == CYC_COPY) {
        return;
    } else {
        const Output o = output_for(&s);
        if (o.skip) return;
        const C4 zero = {{0, 0, 0, 0}};
        c = shade_texel(&s, &o, &zero);
        mode = o.mode;
        amod = o.alpha_mod;
        if (c.v[3] == 0 && mode != SDL_BLENDMODE_NONE && s.ac) return; /* failed alpha compare */
    }
    SDL_SetRenderDrawBlendMode(hle_video.r, mode);
    SDL_SetRenderDrawColor(hle_video.r, (uint8_t)c.v[0], (uint8_t)c.v[1], (uint8_t)c.v[2], (uint8_t)(c.v[3] * amod / 255));
    SDL_RenderFillRect(hle_video.r, &rc);
}

/* TEXTURE_RECTANGLE: screen rect in 10.2, s/t at the top-left in 10.5, steps in 5.10. */
static void texture_rect(uint8_t *rdram, recomp_context *ctx, const uint32_t *w) {
    const float x1 = (float)((w[0] >> 12) & 0xFFF) / 4, y1 = (float)(w[0] & 0xFFF) / 4;
    const int tile = (int)(w[1] >> 24) & 7;
    const float x0 = (float)((w[1] >> 12) & 0xFFF) / 4, y0 = (float)(w[1] & 0xFFF) / 4;
    float s0 = (float)(int16_t)(w[2] >> 16) / 32, t0 = (float)(int16_t)(w[2] & 0xFFFF) / 32;
    const float dsdx = (float)(int16_t)(w[3] >> 16) / 1024, dtdy = (float)(int16_t)(w[3] & 0xFFFF) / 1024;
    const Tile *tl = &R.tile[tile];
    const HleTex *t = tile_texture(rdram, ctx, tl);
    if (!t || x1 <= x0 || y1 <= y0 || dsdx == 0 || dtdy == 0) return;
    /* Texture-space span covered by the rectangle (texparms translate shifts it, scale_log scales). */
    const float ssc = SDL_powf(2.0f, (float)-tl->s_scale), tsc = SDL_powf(2.0f, (float)-tl->t_scale);
    s0 = s0 * ssc - tl->s_shift;
    t0 = t0 * tsc - tl->t_shift;
    const float ds = dsdx * ssc, dt = dtdy * tsc;
    const float u0 = s0, u1 = s0 + (x1 - x0) * ds;
    const float v0 = t0, v1 = t0 + (y1 - y0) * dt;
    const float umin = SDL_min(u0, u1), umax = SDL_max(u0, u1), vmin = SDL_min(v0, v1), vmax = SDL_max(v0, v1);
    const float tw = (float)t->w, th = (float)t->h;
    /* Walk the texture periods the span crosses (one when there's no wrapping). */
    const int ku0 = tl->s_wrap ? (int)SDL_floorf(umin / tw) : 0, ku1 = tl->s_wrap ? (int)SDL_ceilf(umax / tw) - 1 : 0;
    const int kv0 = tl->t_wrap ? (int)SDL_floorf(vmin / th) : 0, kv1 = tl->t_wrap ? (int)SDL_ceilf(vmax / th) - 1 : 0;
    if ((ku1 - ku0 + 1) * (kv1 - kv0 + 1) > 4096) return;
    for (int kv = kv0; kv <= kv1; kv++) {
        const float pv0 = SDL_max(vmin, kv * th), pv1 = SDL_min(vmax, (kv + 1) * th);
        if (pv1 <= pv0) continue;
        for (int ku = ku0; ku <= ku1; ku++) {
            const float pu0 = SDL_max(umin, ku * tw), pu1 = SDL_min(umax, (ku + 1) * tw);
            if (pu1 <= pu0) continue;
            /* screen position of this piece */
            const float sx0 = x0 + ((ds > 0 ? pu0 : pu1) - u0) / ds, sx1 = x0 + ((ds > 0 ? pu1 : pu0) - u0) / ds;
            const float sy0 = y0 + ((dt > 0 ? pv0 : pv1) - v0) / dt, sy1 = y0 + ((dt > 0 ? pv1 : pv0) - v0) / dt;
            bool fx = ds < 0, fy = dt < 0;
            float lu0 = pu0 - ku * tw, lu1 = pu1 - ku * tw, lv0 = pv0 - kv * th, lv1 = pv1 - kv * th;
            if (tl->s_mirror && (ku & 1)) { const float a = tw - lu1; lu1 = tw - lu0; lu0 = a; fx = !fx; }
            if (tl->t_mirror && (kv & 1)) { const float a = th - lv1; lv1 = th - lv0; lv0 = a; fy = !fy; }
            if (!tl->s_wrap) { lu0 = SDL_max(lu0, 0); lu1 = SDL_min(lu1, tw); }
            if (!tl->t_wrap) { lv0 = SDL_max(lv0, 0); lv1 = SDL_min(lv1, th); }
            if (lu1 <= lu0 || lv1 <= lv0) continue;
            const SDL_FRect src = {lu0, lv0, lu1 - lu0, lv1 - lv0};
            const SDL_FRect dst = {SDL_min(sx0, sx1), SDL_min(sy0, sy1), SDL_fabsf(sx1 - sx0), SDL_fabsf(sy1 - sy0)};
            draw_tex(t, &src, &dst, (SDL_FlipMode)((fx ? SDL_FLIP_HORIZONTAL : 0) | (fy ? SDL_FLIP_VERTICAL : 0)), 0, NULL);
        }
    }
}

/* rdpq_blitparms_t at `parms` (11 words, copied): tile, s0, t0, width, height, flip_x|flip_y,
 * cx, cy, scale_x, scale_y, theta. */
static void blit(const HleTex *t, float x, float y, const uint32_t *p) {
    if (!t) return;
    int s0 = 0, t0 = 0, w = 0, h = 0, cx = 0, cy = 0;
    bool flip_x = false, flip_y = false;
    float sx = 1, sy = 1, theta = 0;
    if (p) {
        s0 = (int32_t)p[1];
        t0 = (int32_t)p[2];
        w = (int32_t)p[3];
        h = (int32_t)p[4];
        flip_x = (p[5] >> 24) != 0;
        flip_y = ((p[5] >> 16) & 0xFF) != 0;
        cx = (int32_t)p[6];
        cy = (int32_t)p[7];
        sx = bits_f(p[8]) ? bits_f(p[8]) : 1;
        sy = bits_f(p[9]) ? bits_f(p[9]) : 1;
        theta = bits_f(p[10]);
    }
    const SDL_FRect src = {(float)s0, (float)t0, w ? (float)w : (float)t->w - s0, h ? (float)h : (float)t->h - t0};
    const SDL_FRect dst = {x - cx * sx, y - cy * sy, src.w * sx, src.h * sy};
    const SDL_FPoint c = {cx * sx, cy * sy};
    draw_tex(t, &src, &dst, (SDL_FlipMode)((flip_x ? SDL_FLIP_HORIZONTAL : 0) | (flip_y ? SDL_FLIP_VERTICAL : 0)),
             -theta * 180.0 / SDL_PI_D, &c);
}

/* ================================================================================================
 * Commands: recorded into rspq blocks, or executed
 * ============================================================================================== */
enum {
    OP_RESET_MODE, OP_FIXUP, OP_PUSH, OP_POP, OP_WRITE8, OP_FILL_COLOR32, OP_SCISSOR, OP_FILL_RECT,
    OP_TEX_RECT, OP_TILE_SPRITE, OP_TILE_SURFACE, OP_TLUT, OP_SPRITE_BLIT, OP_TEX_BLIT, OP_CHANGE_SOM,
};
typedef struct {
    uint8_t code;
    uint32_t a[18];
} Op;

typedef struct {
    Op *ops;
    int n, cap;
    bool used;
} Block;
static Block g_blocks[64];
static int g_recording; /* block index + 1 */

static void set_som_bits(uint32_t which, uint32_t and_mask, uint32_t or_val) {
    /* which: 0 = high word, 4 = low word (MODIFY_OTHER_MODES offset) */
    uint32_t hi = (uint32_t)(R.m.som >> 32), lo = (uint32_t)R.m.som;
    if (which & 4) lo = (lo & and_mask) | or_val;
    else hi = (hi & and_mask) | or_val;
    R.m.som = (uint64_t)hi << 32 | lo;
}

static void tile_from_texparms(Tile *t, uint8_t *rdram, uint32_t parms, const uint32_t *copied) {
    /* rdpq_texparms_t: tmem_addr, palette, s {translate, scale_log, repeats, mirror}, t {...} */
    uint32_t p[10];
    for (int i = 0; i < 10; i++) p[i] = copied ? copied[i] : (parms ? n64_r32(rdram, parms + (uint32_t)i * 4) : 0);
    if (!parms && !copied) {
        t->s_shift = t->t_shift = 0;
        t->s_scale = t->t_scale = 0;
        t->s_wrap = t->t_wrap = t->s_mirror = t->t_mirror = false;
        return;
    }
    t->s_shift = bits_f(p[2]);
    t->s_scale = (int32_t)p[3];
    const float srep = bits_f(p[4]);
    t->s_mirror = (p[5] >> 24) != 0;
    t->t_shift = bits_f(p[6]);
    t->t_scale = (int32_t)p[7];
    const float trep = bits_f(p[8]);
    t->t_mirror = (p[9] >> 24) != 0;
    t->s_wrap = srep > 1;
    t->t_wrap = trep > 1;
}

static void exec(uint8_t *rdram, recomp_context *ctx, const Op *op) {
    const uint32_t *a = op->a;
    switch (op->code) {
        case OP_RESET_MODE:
            R.m.cc = (uint64_t)(a[0] & 0x00FFFFFF) << 32 | a[1];
            R.m.cc_2pass = false;
            R.m.som = (uint64_t)a[2] << 32 | a[3];
            R.m.blend = 0;
            R.m.fog = 0;
            break;
        case OP_FIXUP: /* cmd, w0, w1, w2, w3 */
            switch (a[0]) {
                case 0x1B: /* SET_COMBINE_MODE_1PASS */
                    R.m.cc = (uint64_t)(a[1] & 0x00FFFFFF) << 32 | a[2];
                    R.m.cc_2pass = false;
                    break;
                case 0x05: /* SET_COMBINE_MODE_2PASS */
                    R.m.cc = (uint64_t)(a[1] & 0x00FFFFFF) << 32 | a[2];
                    R.m.cc_2pass = true;
                    break;
                case 0x18: R.m.blend = a[2] & ~(1u << 14); break;  /* SET_BLENDING_MODE */
                case 0x19: R.m.fog = a[2]; break;                    /* SET_FOG_MODE */
                case 0x14: set_som_bits(a[1], a[2], a[3]); break;    /* MODIFY_OTHER_MODES */
                default: break;
            }
            break;
        case OP_CHANGE_SOM: set_som_bits(a[0], a[1], a[2]); break;
        case OP_PUSH:
            if (R.sp < (int)SDL_arraysize(R.stack)) R.stack[R.sp++] = R.m;
            break;
        case OP_POP:
            if (R.sp > 0) R.m = R.stack[--R.sp];
            break;
        case OP_WRITE8: /* cmd, arg0, arg1 */
            switch (a[0]) {
                case 0x37: { /* SET_FILL_COLOR: two RGBA5551 pixels (16-bit framebuffer) */
                    uint8_t c[4];
                    rgba16((uint16_t)(a[2] >> 16), c);
                    R.fill = (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | 255;
                } break;
                case 0x38: R.fog = a[2]; break;
                case 0x39: R.blend = a[2]; break;
                case 0x3A: R.prim = a[2]; break;
                case 0x3B: R.env = a[2]; break;
                case 0x3C: R.m.cc = (uint64_t)(a[1] & 0x00FFFFFF) << 32 | a[2]; R.m.cc_2pass = ((R.m.som >> 52) & 3) == CYC_2; break;
                case 0x13: /* SET_PRIM_COLOR_COMPONENT: 0 = colour, 1 = min LOD level and LOD fraction */
                    if (((a[1] >> 16) & 7) == 0) R.prim = a[2];
                    else if (((a[1] >> 16) & 7) == 1) R.prim_lod = (uint8_t)a[1];
                    break;
                default: break;
            }
            break;
        case OP_FILL_COLOR32: R.fill = a[0]; break;
        case OP_SCISSOR: {
            const int x0 = (int)((a[0] >> 12) & 0xFFF) / 4, y0 = (int)(a[0] & 0xFFF) / 4;
            const int x1 = (int)((a[1] >> 12) & 0xFFF) / 4, y1 = (int)(a[1] & 0xFFF) / 4;
            const SDL_Rect rc = {x0, y0, SDL_max(0, x1 - x0), SDL_max(0, y1 - y0)};
            SDL_SetRenderClipRect(hle_video.r, (x0 <= 0 && y0 <= 0 && x1 >= hle_video.w && y1 >= hle_video.h) ? NULL : &rc);
        } break;
        case OP_FILL_RECT:
            fill_rect((float)((a[1] >> 12) & 0xFFF) / 4, (float)(a[1] & 0xFFF) / 4, (float)((a[0] >> 12) & 0xFFF) / 4, (float)(a[0] & 0xFFF) / 4);
            break;
        case OP_TEX_RECT: texture_rect(rdram, ctx, a); break;
        case OP_TILE_SPRITE: { /* tile, sprite, has_parms, parms[10] */
            Tile *t = &R.tile[a[0] & 7];
            const uint32_t sp = a[1];
            t->kind = 1;
            t->addr = sp;
            t->fmt = n64_r8(rdram, sp + 5) & 0x1F;
            t->w = n64_r16(rdram, sp);
            t->h = n64_r16(rdram, sp + 2);
            t->stride = stride_of(t->fmt, t->w);
            tile_from_texparms(t, rdram, 0, a[2] ? a + 3 : NULL);
        } break;
        case OP_TILE_SURFACE: { /* tile, flags|w, h|stride, buffer, has_parms, parms[10] */
            Tile *t = &R.tile[a[0] & 7];
            t->kind = 2;
            t->fmt = (int)(a[1] >> 16) & 0x1F;
            t->w = (int)(a[1] & 0xFFFF);
            t->h = (int)(a[2] >> 16);
            t->stride = (int)(a[2] & 0xFFFF);
            t->addr = a[3];
            tile_from_texparms(t, rdram, 0, a[4] ? a + 5 : NULL);
        } break;
        case OP_TLUT: /* tlut, first, count */
            for (uint32_t i = 0; i < a[2] && a[1] + i < 256; i++) R.tlut[a[1] + i] = n64_r16(rdram, a[0] + i * 2);
            R.tlut_loaded = true;
            break;
        case OP_SPRITE_BLIT: /* sprite, x, y, has_parms, parms[11] */
            blit(hle_sprite_texture(rdram, ctx, a[0], -1, false), bits_f(a[1]), bits_f(a[2]), a[3] ? a + 4 : NULL);
            break;
        case OP_TEX_BLIT: /* flags|w, h|stride, buffer, x, y, has_parms, parms[11] */
            blit(surface_texture(rdram, a[0], a[1], a[2]), bits_f(a[3]), bits_f(a[4]), a[5] ? a + 6 : NULL);
            break;
        default: break;
    }
}

static void emit(uint8_t *rdram, recomp_context *ctx, const Op *op) {
    if (g_recording) {
        Block *b = &g_blocks[g_recording - 1];
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 16;
            b->ops = (Op *)SDL_realloc(b->ops, sizeof(Op) * (size_t)b->cap);
        }
        b->ops[b->n++] = *op;
    } else {
        exec(rdram, ctx, op);
    }
}

#define EMIT(...) do { const Op op_ = {__VA_ARGS__}; emit(rdram, ctx, &op_); } while (0)

void hle_rdpq_frame_begin(void) { SDL_SetRenderClipRect(hle_video.r, NULL); }

/* ================================================================================================
 * The leaves: render mode
 * ============================================================================================== */
/* void __rdpq_reset_render_mode(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) */
void n64___rdpq_reset_render_mode(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_RESET_MODE, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7});
}
/* void __rdpq_fixup_mode(uint32_t cmd_id, uint32_t w0, uint32_t w1) and the 3/4-word variants */
void n64___rdpq_fixup_mode(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_FIXUP, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
void n64___rdpq_fixup_mode3(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_FIXUP, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7});
}
void n64___rdpq_fixup_mode4(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_FIXUP, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7, n64_arg32(rdram, ctx, 4)});
}
/* void __rdpq_change_other_modes(uint32_t w0, uint32_t w1, uint32_t w2): offset, and-mask, or-value */
void n64___rdpq_change_other_modes(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_CHANGE_SOM, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
void n64_rdpq_mode_push(uint8_t *rdram, recomp_context *ctx) { EMIT(OP_PUSH); }
void n64_rdpq_mode_pop(uint8_t *rdram, recomp_context *ctx) { EMIT(OP_POP); }

/* ================================================================================================
 * The leaves: colours, scissor, rectangles
 * ============================================================================================== */
/* void __rdpq_write8_syncchange(uint32_t cmd_id, uint32_t arg0, uint32_t arg1, uint32_t autosync)
 * and its fixup / "syncchangeuse" variants (texture loads through raw tiles are not modelled). */
void n64___rdpq_write8_syncchange(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_WRITE8, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
void n64___rdpq_fixup_write8_syncchange(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_WRITE8, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
void n64___rdpq_write8_syncchangeuse(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_WRITE8, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
void n64___rdpq_set_fill_color(uint8_t *rdram, recomp_context *ctx) { EMIT(OP_FILL_COLOR32, {(uint32_t)ctx->r4}); }
void n64___rdpq_set_scissor(uint8_t *rdram, recomp_context *ctx) { EMIT(OP_SCISSOR, {(uint32_t)ctx->r4, (uint32_t)ctx->r5}); }
/* void __rdpq_fill_rectangle(uint32_t w0, uint32_t w1): w0 = x1,y1; w1 = x0,y0 (10.2, exclusive) */
void n64___rdpq_fill_rectangle(uint8_t *rdram, recomp_context *ctx) { EMIT(OP_FILL_RECT, {(uint32_t)ctx->r4, (uint32_t)ctx->r5}); }
/* void __rdpq_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) */
void n64___rdpq_texture_rectangle(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_TEX_RECT, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7});
}

/* ================================================================================================
 * Textures
 * ============================================================================================== */
static void copy_words(uint8_t *rdram, uint32_t addr, uint32_t *out, int n) {
    for (int i = 0; i < n; i++) out[i] = n64_r32(rdram, addr + (uint32_t)i * 4);
}

/* int rdpq_sprite_upload(rdpq_tile_t tile, sprite_t *sprite, const rdpq_texparms_t *parms) */
void n64_rdpq_sprite_upload(uint8_t *rdram, recomp_context *ctx) {
    Op op = {OP_TILE_SPRITE, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6 != 0}};
    if (ctx->r6) copy_words(rdram, (uint32_t)ctx->r6, op.a + 3, 10);
    emit(rdram, ctx, &op);
    n64_ret32(ctx, 0);
}
/* int rdpq_tex_upload(rdpq_tile_t tile, const surface_t *tex, const rdpq_texparms_t *parms) */
void n64_rdpq_tex_upload(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t s = (uint32_t)ctx->r5;
    Op op = {OP_TILE_SURFACE, {(uint32_t)ctx->r4, n64_r32(rdram, s), n64_r32(rdram, s + 4), n64_r32(rdram, s + 8), (uint32_t)ctx->r6 != 0}};
    if (ctx->r6) copy_words(rdram, (uint32_t)ctx->r6, op.a + 5, 10);
    emit(rdram, ctx, &op);
    n64_ret32(ctx, 0);
}
/* void rdpq_tex_upload_tlut(uint16_t *tlut, int color_idx, int num_colors) */
void n64_rdpq_tex_upload_tlut(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_TLUT, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
/* void rdpq_sprite_blit(sprite_t *sprite, float x0, float y0, const rdpq_blitparms_t *parms) */
void n64_rdpq_sprite_blit(uint8_t *rdram, recomp_context *ctx) {
    Op op = {OP_SPRITE_BLIT, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7 != 0}};
    if (ctx->r7) copy_words(rdram, (uint32_t)ctx->r7, op.a + 4, 11);
    emit(rdram, ctx, &op);
}
/* void rdpq_tex_blit(const surface_t *surf, float x0, float y0, const rdpq_blitparms_t *parms) */
void n64_rdpq_tex_blit(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t s = (uint32_t)ctx->r4;
    Op op = {OP_TEX_BLIT, {n64_r32(rdram, s), n64_r32(rdram, s + 4), n64_r32(rdram, s + 8), (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7 != 0}};
    if (ctx->r7) copy_words(rdram, (uint32_t)ctx->r7, op.a + 6, 11);
    emit(rdram, ctx, &op);
}
/* Multi-texture batching and syncs: nothing to do on the host. */
void n64_rdpq_tex_multi_begin(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_rdpq_tex_multi_end(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 0); }

/* ================================================================================================
 * rspq blocks: record the calls above between begin and end, replay them on run.
 * ============================================================================================== */
void n64_rspq_block_begin_reuse(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    for (int i = 0; i < (int)SDL_arraysize(g_blocks); i++) {
        if (!g_blocks[i].used) {
            g_blocks[i].used = true;
            g_blocks[i].n = 0;
            g_recording = i + 1;
            return;
        }
    }
    n64rt_log("rspq: out of blocks");
}
void n64_rspq_block_begin(uint8_t *rdram, recomp_context *ctx) { n64_rspq_block_begin_reuse(rdram, ctx); }
void n64_rspq_block_end(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    n64_ret32(ctx, g_recording ? 0x80000000u | (uint32_t)g_recording : 0); /* opaque handle */
    g_recording = 0;
}
static Block *block_of(uint32_t h) {
    if ((h & 0xFFFFFF00u) != 0x80000000u) return NULL;
    const int idx = (int)(h & 0xFF) - 1;
    return (idx >= 0 && idx < (int)SDL_arraysize(g_blocks) && g_blocks[idx].used) ? &g_blocks[idx] : NULL;
}
void n64_rspq_block_run(uint8_t *rdram, recomp_context *ctx) {
    const Block *b = block_of((uint32_t)ctx->r4);
    if (!b) return;
    for (int i = 0; i < b->n; ++i) {
        if (g_recording) emit(rdram, ctx, &b->ops[i]); /* a block run inside a recording block */
        else exec(rdram, ctx, &b->ops[i]);
    }
}
void n64_rspq_block_free(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    Block *b = block_of((uint32_t)ctx->r4);
    if (b) { b->used = false; b->n = 0; }
}
