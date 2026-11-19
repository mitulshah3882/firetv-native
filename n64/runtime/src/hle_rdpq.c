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
 * Drawing goes through gfx_gl.c. A rectangle has no per-pixel input besides its texture, so the
 * combiner (1 or 2 cycles), alpha compare, and the blender terms that don't read the framebuffer
 * are evaluated exactly, per texel, into a cached "baked" texture. What does read the framebuffer
 * maps onto the blend modes: P*A + MEM*(1-A) is alpha blending, P*A + MEM additive.
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
    int palette;              /* texparms palette: which 16 TLUT entries a CI4 surface uses */
    /* rdpq_tex_upload_sub: the tile holds only [s_org, s_org + sub_w) x [t_org, t_org + sub_h) of
     * the surface, and texture coordinates stay the surface's (the tile's origin moves), so they
     * wrap within that rectangle. sub_w = 0: the whole surface. */
    int s_org, t_org, sub_w, sub_h;
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
    bool sc_on;                           /* the scissor (also for 3D draws) */
    SDL_Rect sc;
    bool to_depth;                        /* the colour image is the attached depth buffer */
} R;

/* What rdpq_attach attached (surface_t pointers), for rdpq_get_attached and rdpq_set_color_image. */
static uint32_t g_att_color, g_att_depth;
void hle_rdpq_attached(uint32_t color, uint32_t depth) { g_att_color = color; g_att_depth = depth; R.to_depth = false; }

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
                              uint32_t pal, bool host_pal, int pal_base, int layer, bool mask) {
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    uint32_t sig = texture_sig(rdram, fmt, w, h, stride, data) ^ pal;
    if (host_pal) {  /* the same pixels under another palette (or a reloaded TLUT) decode differently */
        const int n = fmt == 8 ? 16 : 256;
        sig ^= 0x5A5A;
        for (int i = 0; i < n; i++) sig = (sig ^ R.tlut[(pal_base + i) & 255]) * 16777619u;
    }
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
        gfx_tex_free(slot->tex);
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
                        rgba16(R.tlut[(pal_base + v) & 255], o);
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
    GfxTex *t = gfx_tex_new(w, h, px);
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
    return decode_texture(rdram, key, fmt, w, h, stride_of(fmt, w), sprite + 8, pal, false, 0, layer, mask);
}

/* surface_t { uint16_t flags, width, height, stride; void *buffer; } */
static HleTex *surface_texture(uint8_t *rdram, uint32_t flags_w, uint32_t h_stride, uint32_t buffer, int palette) {
    const int fmt = (int)(flags_w >> 16) & 0x1F, w = (int)(flags_w & 0xFFFF);
    const int h = (int)(h_stride >> 16), stride = (int)(h_stride & 0xFFFF);
    const bool ci = fmt == 8 || fmt == 9;
    const int base = fmt == 8 ? (palette & 15) * 16 : 0; /* CI4 picks one of 16 palettes; CI8 uses all 256 */
    const bool host_pal = ci && R.tlut_loaded;
    /* The palette's colours are part of the key, so one surface drawn with several palettes (Hydra
     * Harmonics' necks, one per player) keeps a cached texture for each instead of decoding again
     * every time the palette changes. */
    uint32_t kind = 16;
    if (host_pal) {
        uint32_t h = 2166136261u;
        for (int i = 0; i < (fmt == 8 ? 16 : 256); i++) h = (h ^ R.tlut[(base + i) & 255]) * 16777619u;
        kind = 0x80000000u | (h & 0x7FFFFFFFu);
    }
    const uint64_t key = (uint64_t)kind << 32 | (buffer & 0x1FFFFFFFu);
    return decode_texture(rdram, key, fmt, w, h, stride, buffer, 0, host_pal, base, -1, false);
}

static HleTex *tile_texture(uint8_t *rdram, recomp_context *ctx, const Tile *t) {
    if (t->kind == 1) return hle_sprite_texture(rdram, ctx, t->addr, -1, false);
    if (t->kind == 2) return surface_texture(rdram, (uint32_t)t->fmt << 16 | (uint32_t)t->w, (uint32_t)t->h << 16 | (uint32_t)t->stride, t->addr, t->palette);
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

/* How a draw maps onto the renderer. */
typedef struct {
    int mode;              /* GFX_BLEND_* */
    uint8_t alpha_mod;     /* constant source alpha (FOG alpha as the blend factor) */
    bool skip;             /* the result is the framebuffer itself */
} Output;

static Output output_for(const DrawState *s) {
    Output o = {GFX_BLEND_NONE, 255, false};
    if (s->cycle == CYC_COPY) {
        if (s->ac) o.mode = GFX_BLEND_BLEND;
        return o;
    }
    if (s->cycle == CYC_FILL || !s->blend_on) {
        if (s->ac) o.mode = GFX_BLEND_BLEND;
        return o;
    }
    if (s->m == BL_MEM || s->p == BL_MEM) {
        if (s->m == BL_MEM && s->p == BL_MEM) { o.skip = true; return o; }
        o.mode = (s->b == 2) ? GFX_BLEND_ADD : (s->b == 3 && s->m == BL_MEM) ? GFX_BLEND_NONE : GFX_BLEND_BLEND;
        if (s->a == 1) o.alpha_mod = (uint8_t)s->fog_c.v[3];
        if (s->ac && o.mode == GFX_BLEND_NONE) o.mode = GFX_BLEND_BLEND;
        return o;
    }
    if (s->ac) o.mode = GFX_BLEND_BLEND;   /* no framebuffer read: opaque (alpha compare cuts) */
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
 * and the framebuffer-independent part of the blender. Returns the colour to hand to the renderer. */
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
        if (o->mode == GFX_BLEND_NONE) {        /* B = ZERO: P*A */
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
    GfxTex *tex;
    uint32_t stamp;
} Baked;
#define BAKE_CACHE 160
static Baked g_bake[BAKE_CACHE];
static uint32_t g_bake_clock;

static GfxTex *bake(const HleTex *t, const DrawState *s, const Output *o) {
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
    if (lru->tex) gfx_tex_free(lru->tex);
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)t->w * t->h * 4);
    for (int i = 0; i < t->w * t->h; i++) {
        const uint8_t *p = t->px + i * 4;
        const C4 in = {{p[0], p[1], p[2], p[3]}};
        const C4 r = shade_texel(s, o, &in);
        for (int ch = 0; ch < 4; ch++) px[i * 4 + ch] = (uint8_t)r.v[ch];
    }
    GfxTex *tex = gfx_tex_new(t->w, t->h, px);
    SDL_free(px);
    *lru = (Baked){t, t->sig, h, {0}, tex, ++g_bake_clock};
    SDL_memcpy(lru->hash, key, sizeof key);
    return tex;
}

/* ================================================================================================
 * Drawing
 * ============================================================================================== */
static bool linear_filter(void) { return ((R.m.som >> 44) & 3) >= 2 && ((R.m.som >> 52) & 3) != CYC_COPY; }

/* How to draw `t` with the current render mode: the texture (combiner baked in), blend mode,
 * filtering and modulation. False when the mode draws nothing. */
typedef struct {
    GfxTex *tex;
    int blend;
    bool linear;
    uint32_t mod;
} DrawSpec;
static bool mode_texture(const HleTex *t, bool scaled, DrawSpec *d) {
    const DrawState s = draw_state();
    if (s.cycle == CYC_FILL) return false;
    const Output o = output_for(&s);
    if (o.skip) return false;
    const bool plain = is_plain(&s);
    d->tex = plain ? t->tex : bake(t, &s, &o);
    d->blend = plain ? ((s.cycle != CYC_COPY && s.blend_on) ? GFX_BLEND_BLEND : GFX_BLEND_NONE) : o.mode;
    d->linear = linear_filter() && scaled;
    d->mod = 0xFFFFFF00u | o.alpha_mod;
    return d->tex != NULL;
}

/* Draws (part of) a texture with the current render mode. */
static void draw_tex(const HleTex *t, const SDL_FRect *src, const SDL_FRect *dst, int flip, float angle, const SDL_FPoint *center) {
    const bool scaled = SDL_fabsf(src->w - dst->w) > 0.01f || SDL_fabsf(src->h - dst->h) > 0.01f || angle != 0;
    DrawSpec d;
    if (!mode_texture(t, scaled, &d)) return;
    gfx_quad(d.tex, src, dst, flip, angle, center, d.blend, d.linear, d.mod);
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
    draw_tex(t, src, dst, 0, 0, NULL);
    R.m = m;
    R.prim = p;
    R.env = e;
}

static void fill_rect(float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return;
    const SDL_FRect rc = {x0, y0, x1 - x0, y1 - y0};
    const DrawState s = draw_state();
    C4 c;
    int mode = GFX_BLEND_NONE;
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
        if (c.v[3] == 0 && mode != GFX_BLEND_NONE && s.ac) return; /* failed alpha compare */
    }
    gfx_fill(&rc, mode, (uint32_t)c.v[0] << 24 | (uint32_t)c.v[1] << 16 | (uint32_t)c.v[2] << 8 | (uint32_t)(c.v[3] * amod / 255));
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
    s0 = s0 * ssc - tl->s_shift - (float)tl->s_org;
    t0 = t0 * tsc - tl->t_shift - (float)tl->t_org;
    const float ds = dsdx * ssc, dt = dtdy * tsc;
    const float u0 = s0, u1 = s0 + (x1 - x0) * ds;
    const float v0 = t0, v1 = t0 + (y1 - y0) * dt;
    const float umin = SDL_min(u0, u1), umax = SDL_max(u0, u1), vmin = SDL_min(v0, v1), vmax = SDL_max(v0, v1);
    const float tw = (float)(tl->sub_w ? tl->sub_w : t->w), th = (float)(tl->sub_h ? tl->sub_h : t->h);
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
            const SDL_FRect src = {lu0 + (float)tl->s_org, lv0 + (float)tl->t_org, lu1 - lu0, lv1 - lv0};
            const SDL_FRect dst = {SDL_min(sx0, sx1), SDL_min(sy0, sy1), SDL_fabsf(sx1 - sx0), SDL_fabsf(sy1 - sy0)};
            draw_tex(t, &src, &dst, (fx ? GFX_FLIP_H : 0) | (fy ? GFX_FLIP_V : 0), 0, NULL);
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
    draw_tex(t, &src, &dst, (flip_x ? GFX_FLIP_H : 0) | (flip_y ? GFX_FLIP_V : 0), (float)(-theta * 180.0 / SDL_PI_D), &c);
}

/* A triangle from rdpq_triangle, as OP_TRIANGLE packs it: flags (1 = textured, 2 = shaded), tile,
 * then per vertex x, y, s, t (texels) and the shade colour as RGBA8. Drawn as 2D geometry: the
 * perspective term (inv_w) and depth are ignored, which suits the 2D games that use triangles
 * (Hydra Harmonics' stage floor). The texture wraps over its whole surface (sub-tiles and mirrored
 * repeats are approximated). Untextured, unshaded triangles take the primitive colour. */
static void triangle(uint8_t *rdram, recomp_context *ctx, const uint32_t *a) {
    const bool textured = a[0] & 1, shaded = a[0] & 2;
    const Tile *tl = &R.tile[a[1] & 7];
    const HleTex *t = textured ? tile_texture(rdram, ctx, tl) : NULL;
    if (textured && !t) return;
    DrawSpec d = {NULL, GFX_BLEND_NONE, false, 0xFFFFFFFFu};
    if (t) {
        if (!mode_texture(t, true, &d)) return;
        d.linear = linear_filter();
    } else {
        const DrawState st = draw_state();
        if (st.cycle == CYC_FILL) return;
        d.blend = st.blend_on ? GFX_BLEND_BLEND : GFX_BLEND_NONE;
    }
    const float ssc = SDL_powf(2.0f, (float)-tl->s_scale), tsc = SDL_powf(2.0f, (float)-tl->t_scale);
    SDL_Vertex v[3];
    for (int i = 0; i < 3; i++) {
        const uint32_t *p = a + 2 + i * 5;
        v[i].position = (SDL_FPoint){bits_f(p[0]), bits_f(p[1])};
        const uint32_t c = shaded ? p[4] : (t ? 0xFFFFFFFFu : R.prim);
        v[i].color = (SDL_FColor){(float)(c >> 24) / 255.0f, (float)((c >> 16) & 255) / 255.0f,
                                  (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f};
        if (t) {
            v[i].tex_coord = (SDL_FPoint){(bits_f(p[2]) * ssc - tl->s_shift) / (float)t->w,
                                          (bits_f(p[3]) * tsc - tl->t_shift) / (float)t->h};
        } else {
            v[i].tex_coord = (SDL_FPoint){0, 0};
        }
    }
    gfx_geometry(d.tex, v, 3, d.blend, d.linear);
}

/* ================================================================================================
 * Commands: recorded into rspq blocks, or executed
 * ============================================================================================== */
enum {
    OP_RESET_MODE, OP_FIXUP, OP_PUSH, OP_POP, OP_WRITE8, OP_FILL_COLOR32, OP_SCISSOR, OP_FILL_RECT,
    OP_TEX_RECT, OP_TILE_SPRITE, OP_TILE_SURFACE, OP_TLUT, OP_SPRITE_BLIT, OP_TEX_BLIT, OP_CHANGE_SOM,
    OP_TILE_SURFACE_SUB, OP_TILE_REUSE, OP_TRIANGLE, OP_T3D, OP_CALL, OP_GLYPH, OP_COLOR_IMAGE, OP_RUN_BLOCK, OP_SET_PH,
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
/* Blocks and queues share this table; a handle is 0x80000000 | (index + 1). */
static Block g_blocks[256];
static int g_recording; /* block (or current queue) index + 1 */
static int g_last_tile = -1; /* the tile of the last texture upload (rdpq_tex_reuse) */

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
        t->palette = 0;
        t->s_shift = t->t_shift = 0;
        t->s_scale = t->t_scale = 0;
        t->s_wrap = t->t_wrap = t->s_mirror = t->t_mirror = false;
        return;
    }
    t->palette = (int)(p[1] & 15);
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

/* rdpq_sprite_upload and rdpq_sprite_blit also load a CI sprite's palette into TMEM, and it stays
 * there: a game can then draw the sprite's pixels as a plain surface with that palette (Hydra
 * Harmonics draws each hydra's neck from its collar sprite this way). Mirror that in R.tlut. */
static void load_sprite_tlut(uint8_t *rdram, recomp_context *ctx, uint32_t sprite, int palette) {
    const int fmt = n64_r8(rdram, sprite + 5) & 0x1F;
    if ((fmt != 8 && fmt != 9) || !n64_sprite_get_palette) return;
    const uint32_t pal = (uint32_t)n64rt_call(ctx, n64_sprite_get_palette, sprite, 0, 0, 0);
    if (!pal) return;
    const int first = fmt == 8 ? (palette & 15) * 16 : 0, count = fmt == 8 ? 16 : 256;
    for (int i = 0; i < count; i++) R.tlut[first + i] = n64_r16(rdram, pal + (uint32_t)i * 2);
    R.tlut_loaded = true;
}

static Block *block_of(uint32_t h);
static uint32_t g_ph[7];  /* rspq block placeholders 0-6: the blocks they stand for */

/* A Z-buffer word (3-bit exponent, 11-bit mantissa, 2-bit dz) as the 18-bit depth it encodes, 0..1:
 * the scale of gfx_gl.c's depth, since the RDP's depth is (z/w + 1) / 2 of that range too. */
static float z_decode(uint16_t v) {
    static const uint32_t base[8] = {0x00000, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800};
    static const int shift[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    const int e = v >> 13;
    return (float)(base[e] + (((uint32_t)v >> 2 & 0x7FF) << shift[e])) / (float)0x3FFFF;
}

/* A 16-bit texture copied into the depth buffer (drawn while it is the colour image): Cathode Quest
 * clears depth to a Bayer pattern near the far plane, so distant geometry dissolves instead of
 * popping at the far clip. The texels become depth, one per pixel as in copy mode. */
static void depth_rect(uint8_t *rdram, const uint32_t *w) {
    const int x1 = (int)((w[0] >> 12) & 0xFFF) / 4, y1 = (int)(w[0] & 0xFFF) / 4;
    const int x0 = (int)((w[1] >> 12) & 0xFFF) / 4, y0 = (int)(w[1] & 0xFFF) / 4;
    const Tile *tl = &R.tile[(w[1] >> 24) & 7];
    const int s0 = (int16_t)(w[2] >> 16) / 32, t0 = (int16_t)(w[2] & 0xFFFF) / 32;
    if (tl->kind != 2 || tl->fmt != 2 /* FMT_RGBA16 */ || tl->w <= 0 || tl->h <= 0 || tl->w * tl->h > 65536) {
        gfx_fb_clear_depth();
        return;
    }
    float *d = (float *)SDL_malloc(sizeof(float) * (size_t)(tl->w * tl->h));
    if (!d) return;
    for (int y = 0; y < tl->h; y++)
        for (int x = 0; x < tl->w; x++)
            d[y * tl->w + x] = z_decode(n64_r16(rdram, tl->addr + (uint32_t)(y * tl->stride + x * 2)));
    /* copy mode's rectangles include their last column and row */
    gfx_fb_depth_pattern(d, tl->w, tl->h, x0, y0, x1 + 1, y1 + 1, x0 - s0, y0 - t0);
    SDL_free(d);
}

static void exec(uint8_t *rdram, recomp_context *ctx, const Op *op) {
    const uint32_t *a = op->a;
    if (op->code == OP_RUN_BLOCK) {  /* a block or queue, by reference (or a placeholder 0-6) */
        static int depth;
        const Block *b = block_of(a[0] < 7 ? g_ph[a[0]] : a[0]);
        if (!b || depth >= 8) return;  /* the RSP nests blocks 8 deep */
        ++depth;
        for (int i = 0; i < b->n; ++i) exec(rdram, ctx, &b->ops[i]);
        --depth;
        return;
    }
    if (op->code == OP_SET_PH) {
        if (a[0] < 7) g_ph[a[0]] = a[1];
        return;
    }
    if (op->code == OP_T3D) {  /* a tiny3d/tinypx command (hle_t3d.c): a[0] = word count | 0x100 for tinypx, then the words */
        if (a[0] & 0x100) hle_tpx_exec(rdram, ctx, a + 1, (int)(a[0] & 0xFF));
        else hle_t3d_exec(rdram, ctx, a + 1, (int)(a[0] & 0xFF));
        return;
    }
    if (op->code == OP_CALL) {  /* a callback the RSP would call on reaching this point: func, arg */
        hle_t3d_flush(rdram, ctx);
        recomp_func_t *fn = get_function((int32_t)a[0]);
        if (fn) n64rt_call(ctx, fn, a[1], 0, 0, 0);
        return;
    }
    hle_t3d_flush(rdram, ctx);  /* its pending triangles use the render mode before this change */
    if (R.to_depth && op->code == OP_TEX_RECT) {
        depth_rect(rdram, a);
        return;
    }
    if (R.to_depth && (op->code == OP_FILL_RECT || op->code == OP_TEX_RECT || op->code == OP_TRIANGLE ||
                       op->code == OP_SPRITE_BLIT || op->code == OP_TEX_BLIT))
        return;  /* drawing into the depth buffer as colour: see OP_COLOR_IMAGE */
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
            R.sc_on = !(x0 <= 0 && y0 <= 0 && x1 >= hle_video.w && y1 >= hle_video.h);
            R.sc = rc;
            gfx_scissor(R.sc_on ? &rc : NULL);
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
            t->s_org = t->t_org = t->sub_w = t->sub_h = 0;
            tile_from_texparms(t, rdram, 0, a[2] ? a + 3 : NULL);
            g_last_tile = (int)(a[0] & 7);
            load_sprite_tlut(rdram, ctx, sp, t->palette);
        } break;
        case OP_TILE_SURFACE: { /* tile, flags|w, h|stride, buffer, has_parms, parms[10] */
            Tile *t = &R.tile[a[0] & 7];
            t->kind = 2;
            t->fmt = (int)(a[1] >> 16) & 0x1F;
            t->w = (int)(a[1] & 0xFFFF);
            t->h = (int)(a[2] >> 16);
            t->stride = (int)(a[2] & 0xFFFF);
            t->addr = a[3];
            t->s_org = t->t_org = t->sub_w = t->sub_h = 0;
            tile_from_texparms(t, rdram, 0, a[4] ? a + 5 : NULL);
            g_last_tile = (int)(a[0] & 7);
        } break;
        case OP_TILE_SURFACE_SUB: { /* tile, flags|w, h|stride, buffer, s0|t0, s1|t1, has_parms, parms[10] */
            Tile *t = &R.tile[a[0] & 7];
            t->kind = 2;
            t->fmt = (int)(a[1] >> 16) & 0x1F;
            t->w = (int)(a[1] & 0xFFFF);
            t->h = (int)(a[2] >> 16);
            t->stride = (int)(a[2] & 0xFFFF);
            t->addr = a[3];
            t->s_org = (int16_t)(a[4] >> 16);
            t->t_org = (int16_t)(a[4] & 0xFFFF);
            t->sub_w = (int16_t)(a[5] >> 16) - t->s_org;
            t->sub_h = (int16_t)(a[5] & 0xFFFF) - t->t_org;
            tile_from_texparms(t, rdram, 0, a[6] ? a + 7 : NULL);
            g_last_tile = (int)(a[0] & 7);
        } break;
        case OP_TILE_REUSE: { /* tile, has_parms, parms[10]: the last upload, with new parameters */
            Tile *t = &R.tile[a[0] & 7];
            if (g_last_tile >= 0 && t != &R.tile[g_last_tile]) *t = R.tile[g_last_tile];
            tile_from_texparms(t, rdram, 0, a[1] ? a + 2 : NULL);
        } break;
        case OP_TRIANGLE: triangle(rdram, ctx, a); break;
        case OP_TLUT: /* tlut, first, count */
            for (uint32_t i = 0; i < a[2] && a[1] + i < 256; i++) R.tlut[a[1] + i] = n64_r16(rdram, a[0] + i * 2);
            R.tlut_loaded = true;
            break;
        case OP_SPRITE_BLIT: /* sprite, x, y, has_parms, parms[11] */
            blit(hle_sprite_texture(rdram, ctx, a[0], -1, false), bits_f(a[1]), bits_f(a[2]), a[3] ? a + 4 : NULL);
            load_sprite_tlut(rdram, ctx, a[0], 0);
            break;
        case OP_TEX_BLIT: /* flags|w, h|stride, buffer, x, y, has_parms, parms[11] */
            blit(surface_texture(rdram, a[0], a[1], a[2], 0), bits_f(a[3]), bits_f(a[4]), a[5] ? a + 6 : NULL);
            break;
        case OP_GLYPH: hle_draw_glyph(rdram, ctx, a); break;
        case OP_COLOR_IMAGE: { /* buffer of the new colour image */
            /* A program may point the colour image at its depth buffer and draw into it: Cathode
             * Quest "clears" depth that way, with a Bayer pattern just short of the far plane so the
             * distance dithers out. That draws a clear for now (the dither is lost) and skips the
             * draws until the colour image is set back. */
            const uint32_t depth_buf = g_att_depth ? n64_r32(rdram, g_att_depth + 8) : 0;
            const bool to_depth = depth_buf && (a[0] & 0x1FFFFFFFu) == (depth_buf & 0x1FFFFFFFu);
            if (to_depth && !R.to_depth) gfx_fb_clear_depth();
            R.to_depth = to_depth;
            /* Another colour image of the screen's size (a swap-chain surface, BigTex's G-buffer)
             * is its own GL colour target. */
            if (!to_depth && a[0]) gfx_fb_slot(hle_slot_for(a[0], (int)(a[1] & 0xFFFF), (int)(a[1] >> 16), a[2]));
        } break;
        default: break;
    }
}

/* Debug aid: N64_OP_TRACE=<n> logs the first n ops, recorded (into a block or queue) or executed. */
static void trace_op(const Op *op, bool rec) {
    static int left = -1;
    if (left < 0) left = SDL_getenv("N64_OP_TRACE") ? SDL_atoi(SDL_getenv("N64_OP_TRACE")) : 0;
    if (left <= 0) return;
    --left;
    n64rt_log("op %s%d [rec %d] %08X %08X %08X %08X", rec ? "rec " : "", op->code, g_recording, op->a[0], op->a[1], op->a[2], op->a[3]);
}
static void emit(uint8_t *rdram, recomp_context *ctx, const Op *op) {
    trace_op(op, g_recording != 0);
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

void hle_rdpq_frame_begin(void) {
    R.sc_on = false;
    gfx_scissor(NULL);
}

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
/* void __rdpq_write8(uint32_t cmd_id, uint32_t arg0, uint32_t arg1) */
void n64___rdpq_write8(uint8_t *rdram, recomp_context *ctx) {
    EMIT(OP_WRITE8, {(uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6});
}
/* void rdpq_set_color_image(const surface_t *surface) */
void n64_rdpq_set_color_image(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t s = (uint32_t)ctx->r4;  /* buffer, width | height << 16, stride */
    EMIT(OP_COLOR_IMAGE, {s ? n64_r32(rdram, s + 8) : 0, s ? (uint32_t)n64_r16(rdram, s + 2) | (uint32_t)n64_r16(rdram, s + 4) << 16 : 0,
                          s ? n64_r16(rdram, s + 6) : 0});
}
void n64_rdpq_set_z_image(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
/* const surface_t *rdpq_get_attached(void) */
void n64_rdpq_get_attached(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, g_att_color); }
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
/* int rdpq_tex_upload_sub(rdpq_tile_t tile, const surface_t *tex, const rdpq_texparms_t *parms,
 *                         int s0, int t0, int s1, int t1)
 * Returns the TMEM bytes used, which games add up to place the next upload (Hydra Harmonics). */
void n64_rdpq_tex_upload_sub(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t s = (uint32_t)ctx->r5;
    const int s0 = (int)ctx->r7, t0 = (int)n64_arg32(rdram, ctx, 4);
    const int s1 = (int)n64_arg32(rdram, ctx, 5), t1 = (int)n64_arg32(rdram, ctx, 6);
    const uint32_t flags_w = n64_r32(rdram, s);
    Op op = {OP_TILE_SURFACE_SUB, {(uint32_t)ctx->r4, flags_w, n64_r32(rdram, s + 4), n64_r32(rdram, s + 8),
                                   (uint32_t)(uint16_t)s0 << 16 | (uint16_t)t0, (uint32_t)(uint16_t)s1 << 16 | (uint16_t)t1,
                                   (uint32_t)ctx->r6 != 0}};
    if (ctx->r6) copy_words(rdram, (uint32_t)ctx->r6, op.a + 7, 10);
    emit(rdram, ctx, &op);
    const int bits = 4 << ((flags_w >> 16) & 3);
    const int row = (((s1 - s0) * bits / 8) + 7) & ~7;
    n64_ret32(ctx, (uint32_t)SDL_max(0, row * (t1 - t0)));
}
/* int rdpq_tex_reuse(rdpq_tile_t tile, const rdpq_texparms_t *parms) */
void n64_rdpq_tex_reuse(uint8_t *rdram, recomp_context *ctx) {
    Op op = {OP_TILE_REUSE, {(uint32_t)ctx->r4, (uint32_t)ctx->r5 != 0}};
    if (ctx->r5) copy_words(rdram, (uint32_t)ctx->r5, op.a + 2, 10);
    emit(rdram, ctx, &op);
    n64_ret32(ctx, 0);
}
/* void rdpq_triangle(const rdpq_trifmt_t *fmt, const float *v1, const float *v2, const float *v3)
 * rdpq_trifmt_t: pos_offset, shade_offset, shade_flat (bool), tex_offset, tex_tile, tex_mipmaps,
 * z_offset; offsets count floats into each vertex, -1 = absent. */
void n64_rdpq_triangle(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t f = (uint32_t)ctx->r4;
    const int pos = (int)n64_r32(rdram, f), shade = (int)n64_r32(rdram, f + 4), tex = (int)n64_r32(rdram, f + 12);
    const bool flat = n64_r8(rdram, f + 8) != 0;
    Op op = {OP_TRIANGLE, {(tex >= 0 ? 1u : 0u) | (shade >= 0 ? 2u : 0u), n64_r32(rdram, f + 16)}};
    const uint32_t vtx[3] = {(uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7};
    for (int i = 0; i < 3; i++) {
        uint32_t *o = op.a + 2 + i * 5;
        const uint32_t v = vtx[i];
        o[0] = n64_r32(rdram, v + (uint32_t)pos * 4);
        o[1] = n64_r32(rdram, v + (uint32_t)pos * 4 + 4);
        if (tex >= 0) {
            o[2] = n64_r32(rdram, v + (uint32_t)tex * 4);
            o[3] = n64_r32(rdram, v + (uint32_t)tex * 4 + 4);
        }
        if (shade >= 0) {
            const uint32_t sv = flat ? vtx[0] : v;  /* flat shading: the first vertex's colour */
            uint32_t c = 0;
            for (int ch = 0; ch < 4; ch++) {
                const float x = bits_f(n64_r32(rdram, sv + (uint32_t)(shade + ch) * 4));
                c = c << 8 | (uint32_t)SDL_clamp((int)(x * 255.0f + 0.5f), 0, 255);
            }
            o[4] = c;
        }
    }
    emit(rdram, ctx, &op);
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
static int block_alloc(void) {
    for (int i = 0; i < (int)SDL_arraysize(g_blocks); i++) {
        if (!g_blocks[i].used) {
            g_blocks[i].used = true;
            g_blocks[i].n = 0;
            return i + 1;
        }
    }
    n64rt_log("rspq: out of blocks");
    return 0;
}
#define TRACE_Q(...) do { if (SDL_getenv("N64_OP_TRACE")) n64rt_log(__VA_ARGS__); } while (0)
void n64_rspq_block_begin_reuse(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    Block *reuse = block_of((uint32_t)ctx->r4);  /* rspq_block_begin_reuse(block): record it again */
    if (reuse) {
        reuse->n = 0;
        g_recording = (int)((uint32_t)ctx->r4 & 0xFFFF);
    } else {
        g_recording = block_alloc();
    }
    TRACE_Q("block begin -> %d", g_recording);
}
void n64_rspq_block_begin(uint8_t *rdram, recomp_context *ctx) { n64_rspq_block_begin_reuse(rdram, ctx); }
void n64_rspq_block_end(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    TRACE_Q("block end %d", g_recording);
    n64_ret32(ctx, g_recording ? 0x80000000u | (uint32_t)g_recording : 0); /* opaque handle */
    g_recording = 0;
}
static Block *block_of(uint32_t h) {
    if ((h & 0xFFFF0000u) != 0x80000000u) return NULL;
    const int idx = (int)(h & 0xFFFF) - 1;
    return (idx >= 0 && idx < (int)SDL_arraysize(g_blocks) && g_blocks[idx].used) ? &g_blocks[idx] : NULL;
}
/* A block run is recorded by reference, as the RSP calls a block: a block that is recorded again
 * later (rspq_block_begin_reuse) runs with its new commands. Placeholders 0-6 resolve when run. */
void n64_rspq_block_run(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    const uint32_t h = (uint32_t)ctx->r4;
    if (h >= 7 && !block_of(h)) return;
    if (SDL_getenv("N64_OP_TRACE")) n64rt_log("run block/queue %08X (recording %d)", h, g_recording);
    EMIT(OP_RUN_BLOCK, {h});
}
/* void rspq_block_set_placeholder(rspq_block_t *ph, rspq_block_t *ph_target): a command in order */
void n64_rspq_block_set_placeholder(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    EMIT(OP_SET_PH, {(uint32_t)ctx->r4, (uint32_t)ctx->r5});
}
void n64_rspq_block_free(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    Block *b = block_of((uint32_t)ctx->r4);
    if (b) { b->used = false; b->n = 0; }
}

/* ================================================================================================
 * Syncpoints. Commands run when they're written here, so a syncpoint is reached at once; one with a
 * callback (rspq_call_deferred, rdpq_call_deferred) calls it at its place in the command stream,
 * which is later if it's recorded into a block or queue.
 * int rspq_syncpoint_new_cb(void (*func)(void *), void *arg)
 * ============================================================================================== */
void n64_rspq_syncpoint_new_cb(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    const uint32_t fn = (uint32_t)ctx->r4, arg = (uint32_t)ctx->r5;
    EMIT(OP_CALL, {fn, arg});
    n64_ret32(ctx, 1);
}
void n64_rspq_syncpoint_new(uint8_t *rdram, recomp_context *ctx) { hle_rspq_drain(rdram, ctx); n64_ret32(ctx, 1); }
void n64_rspq_check_syncpoint(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 1); }
void n64_rspq_wait_syncpoint(uint8_t *rdram, recomp_context *ctx) { hle_rspq_drain(rdram, ctx); }

/* ================================================================================================
 * rspq queues. Pyrite64 records each draw layer into its own queue while the scene draws, then runs
 * the queues in layer order. A queue here is a block that stays open: rspq_queue_switch(q) makes it
 * the recording target, rspq_queue_switch(NULL) goes back to the main queue (executing), and
 * rspq_queue_run(q) replays it wherever it is called, as rspq_block_run does.
 * ============================================================================================== */
void n64_rspq_queue_create(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const int h = block_alloc();
    n64_ret32(ctx, h ? 0x80000000u | (uint32_t)h : 0);
    TRACE_Q("queue create -> %d", h);
}
void n64_rspq_queue_switch(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    const uint32_t h = (uint32_t)ctx->r4;
    g_recording = block_of(h) ? (int)(h & 0xFFFF) : 0;
    TRACE_Q("queue switch %08X -> recording %d", h, g_recording);
}
void n64_rspq_queue_run(uint8_t *rdram, recomp_context *ctx) { n64_rspq_block_run(rdram, ctx); }
void n64_rspq_queue_clear(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    TRACE_Q("queue clear %08X", (uint32_t)ctx->r4);
    Block *b = block_of((uint32_t)ctx->r4);
    if (b) b->n = 0;
}
void n64_rspq_queue_destroy(uint8_t *rdram, recomp_context *ctx) {
    hle_rspq_drain(rdram, ctx);
    const uint32_t h = (uint32_t)ctx->r4;
    Block *b = block_of(h);
    if (!b) return;
    if (g_recording == (int)(h & 0xFFFF)) g_recording = 0;
    b->used = false;
    b->n = 0;
}

/* ================================================================================================
 * 3D (hle_t3d.c): tiny3d commands as ops, and triangles drawn with the current render mode
 * ============================================================================================== */
void hle_rdpq_glyph(uint8_t *rdram, recomp_context *ctx, const uint32_t a[10]) {
    Op op = {OP_GLYPH, {0}};
    SDL_memcpy(op.a, a, 10 * sizeof(uint32_t));
    emit(rdram, ctx, &op);
}
void hle_rdpq_t3d_op(uint8_t *rdram, recomp_context *ctx, const uint32_t *w, int n, bool tpx) {
    Op op = {OP_T3D, {0}};
    if (n > 17) n = 17;
    op.a[0] = (uint32_t)n | (tpx ? 0x100u : 0u);
    for (int i = 0; i < n; i++) op.a[1 + i] = w[i];
    emit(rdram, ctx, &op);
}

/* The colour combiner as GLSL: (A - B) * C + D per cycle, for RGB and alpha. For particles
 * (g_prim_vtx) PRIM is per rectangle and arrives as the vertex colour; rectangles have no shade. */
static bool g_prim_vtx;
static const char *cc_rgb(int pos, int sel) {
    static const char *common[6] = {"comb.rgb", "tex.rgb", "tex1.rgb", "u_prim.rgb", "v_col.rgb", "u_env.rgb"};
    if (g_prim_vtx && (sel == 3 || sel == 4 || (pos == 2 && (sel == 10 || sel == 11))))
        return sel == 3 ? "v_col.rgb" : sel == 10 ? "vec3(v_col.a)" : "vec3(0.0)";
    if (sel < 6) return common[sel];
    switch (pos) {
        case 0: return sel == 6 ? "vec3(1.0)" : sel == 7 ? "vec3(0.5)" : "vec3(0.0)";  /* 7: noise */
        case 2: {
            static const char *mul[16] = {"", "", "", "", "", "", "vec3(0.0)", "vec3(comb.a)", "vec3(tex.a)", "vec3(tex1.a)",
                                          "vec3(u_prim.a)", "vec3(v_col.a)", "vec3(u_env.a)", "vec3(0.0)", "vec3(u_prim_lod)", "vec3(0.0)"};
            return sel < 16 ? mul[sel] : "vec3(0.0)";
        }
        case 3: return sel == 6 ? "vec3(1.0)" : "vec3(0.0)";
        default: return "vec3(0.0)";
    }
}
static const char *cc_alpha(int pos, int sel) {
    static const char *common[8] = {"comb.a", "tex.a", "tex1.a", "u_prim.a", "v_col.a", "u_env.a", "1.0", "0.0"};
    if (g_prim_vtx && ((sel & 7) == 3 || (sel & 7) == 4)) return (sel & 7) == 3 ? "v_col.a" : "0.0";
    if (pos == 2) {
        static const char *mul[8] = {"0.0", "tex.a", "tex1.a", "u_prim.a", "v_col.a", "u_env.a", "u_prim_lod", "0.0"};
        return mul[sel & 7];
    }
    return common[sel & 7];
}

typedef struct { uint64_t cc; bool two; bool ac; bool pv; int prog; } Prog3D;
static Prog3D g_prog3d[64];
static int g_nprog3d;

static int program_for(const DrawState *s) {
    const bool ac = s->ac == 1;
    for (int i = 0; i < g_nprog3d; i++)
        if (g_prog3d[i].cc == R.m.cc && g_prog3d[i].two == (s->ncycles == 2) && g_prog3d[i].ac == ac && g_prog3d[i].pv == g_prim_vtx)
            return g_prog3d[i].prog;
    char body[2048];
    int len = SDL_snprintf(body, sizeof body, "vec4 tex = texture(u_tex, v_uv);\nvec4 tex1 = texture(u_tex1, v_uv * u_t1.xy + u_t1.zw);\n"
                                              "vec4 comb = vec4(0.0);\n");
    for (int c = 0; c < s->ncycles; c++) {
        const Cycle *k = &s->cc[c];
        len += SDL_snprintf(body + len, sizeof body - (size_t)len,
                            "comb = vec4((%s - %s) * %s + %s, (%s - %s) * %s + %s);\ncomb = clamp(comb, 0.0, 1.0);\n",
                            cc_rgb(0, k->ra), cc_rgb(1, k->rb), cc_rgb(2, k->rc), cc_rgb(3, k->rd), cc_alpha(0, k->aa),
                            cc_alpha(1, k->ab), cc_alpha(2, k->ac), cc_alpha(3, k->ad));
    }
    SDL_snprintf(body + len, sizeof body - (size_t)len, "%so = comb;\n", ac ? "if (comb.a < u_athresh) discard;\n" : "");
    const int prog = gfx_prog3d(body);
    if (prog < 0) n64rt_log("rdpq: 3D combiner %016llX didn't compile", (unsigned long long)R.m.cc);
    if (g_nprog3d < (int)SDL_arraysize(g_prog3d)) g_prog3d[g_nprog3d++] = (Prog3D){R.m.cc, s->ncycles == 2, ac, g_prim_vtx, prog};
    return prog;
}

static bool cc_reads_tex(const DrawState *s) {
    for (int c = 0; c < s->ncycles; c++) {
        const Cycle *k = &s->cc[c];
        if (k->ra == 1 || k->ra == 2 || k->rb == 1 || k->rb == 2 || k->rc == 1 || k->rc == 2 || k->rc == 8 || k->rc == 9 ||
            k->rd == 1 || k->rd == 2 || k->aa == 1 || k->aa == 2 || k->ab == 1 || k->ab == 2 || k->ac == 1 || k->ac == 2 ||
            k->ad == 1 || k->ad == 2)
            return true;
    }
    return false;
}

/* Triangles in clip space (u, v in texels of tile 0), drawn with the current render mode into the
 * viewport (N64 pixels). */
void hle_rdpq_draw3d(uint8_t *rdram, recomp_context *ctx, GfxVert3D *v, int n, int vx, int vy, int vw, int vh) {
    const DrawState s = draw_state();
    if (s.cycle == CYC_FILL || s.cycle == CYC_COPY) return;
    const Output o = output_for(&s);
    if (o.skip) return;
    Gfx3DState st;
    SDL_zero(st);
    st.prog = program_for(&s);
    if (st.prog < 0) return;
    st.t1[0] = st.t1[1] = 1.0f;  /* TEX1's coordinates from TEX0's: identity unless tile 1 has a texture */
    if (cc_reads_tex(&s)) {
        /* v[].u/v arrive in texels; the shader gets them normalised for tile 0, and tile 1 (TEX1:
         * same S,T, its own scale, shift and size) as an affine map of those (u_t1). */
        const Tile *tl = &R.tile[0], *tl1 = &R.tile[1];
        const HleTex *t = tile_texture(rdram, ctx, tl);
        float ssc = 1, tsc = 1, sh = 0, th_ = 0, tw = 1, th = 1;
        if (t) {
            st.tex = t->tex;
            st.wrap = tl->s_wrap || tl->t_wrap;
            st.mirror = (uint8_t)((tl->s_mirror ? 1 : 0) | (tl->t_mirror ? 2 : 0));
            ssc = SDL_powf(2.0f, (float)-tl->s_scale);
            tsc = SDL_powf(2.0f, (float)-tl->t_scale);
            sh = tl->s_shift;
            th_ = tl->t_shift;
            tw = (float)(tl->sub_w ? tl->sub_w : t->w);
            th = (float)(tl->sub_h ? tl->sub_h : t->h);
            for (int i = 0; i < n; i++) {
                v[i].u = (v[i].u * ssc - sh) / tw;
                v[i].v = (v[i].v * tsc - th_) / th;
            }
        }
        const HleTex *t1 = tl1->kind ? tile_texture(rdram, ctx, tl1) : NULL;
        if (t1) {
            st.tex1 = t1->tex;
            st.wrap1 = tl1->s_wrap || tl1->t_wrap;
            st.mirror1 = (uint8_t)((tl1->s_mirror ? 1 : 0) | (tl1->t_mirror ? 2 : 0));
            const float ssc1 = SDL_powf(2.0f, (float)-tl1->s_scale), tsc1 = SDL_powf(2.0f, (float)-tl1->t_scale);
            const float tw1 = (float)(tl1->sub_w ? tl1->sub_w : t1->w), th1 = (float)(tl1->sub_h ? tl1->sub_h : t1->h);
            st.t1[0] = tw * ssc1 / (ssc * tw1);
            st.t1[1] = th * tsc1 / (tsc * th1);
            st.t1[2] = (sh * ssc1 / ssc - tl1->s_shift) / tw1;
            st.t1[3] = (th_ * tsc1 / tsc - tl1->t_shift) / th1;
        }
    }
    st.linear = linear_filter();
    st.blend = o.mode;
    /* Without a framebuffer read the RDP writes what passes the alpha compare opaque. output_for asks
     * for alpha blending there because the 2D path forces alpha to 0 or 255; this shader discards
     * instead, so blending would let semi-transparent texels show the background (coins, foliage). */
    if (s.ac == 1 && st.blend == GFX_BLEND_BLEND && (!s.blend_on || (s.m != BL_MEM && s.p != BL_MEM))) st.blend = GFX_BLEND_NONE;
    st.ztest = (R.m.som >> 4) & 1;   /* SOM_Z_COMPARE */
    st.zwrite = (R.m.som >> 5) & 1;  /* SOM_Z_WRITE */
    const C4 p = unpack(R.prim), e = unpack(R.env);
    for (int i = 0; i < 4; i++) {
        st.prim[i] = (float)p.v[i] / 255.0f;
        st.env[i] = (float)e.v[i] / 255.0f;
    }
    st.prim_lod = (float)R.prim_lod / 255.0f;
    st.athresh = (float)s.ac_threshold / 255.0f;
    st.vx = vx;
    st.vy = vy;
    st.vw = vw;
    st.vh = vh;
    st.sc_on = R.sc_on;
    st.sc = R.sc;
    gfx_draw3d(&st, v, n);
}

void hle_rdpq_draw_particles(uint8_t *rdram, recomp_context *ctx, GfxVert3D *v, int n, int vx, int vy, int vw, int vh) {
    g_prim_vtx = true;
    hle_rdpq_draw3d(rdram, ctx, v, n, vx, vy, vw, vh);
    g_prim_vtx = false;
}
